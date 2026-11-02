#include "net/NetTransport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QSslCipher>
#include <QSslConfiguration>
#include <QSslPreSharedKeyAuthenticator>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <cstring>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <limits>

namespace kega::net {
namespace {
constexpr int kSecretSize = 32;
constexpr quint32 kMaxFrame = 256 * 1024;
constexpr qint64 kMaxQueuedBytes = NetTransport::maxQueuedBytes;
constexpr int kMaxFramesPerSecond = 4096;
constexpr int kMaxBytesPerSecond = 128 * 1024 * 1024;
constexpr int kDeadLinkMs = 5000;
constexpr int kMaxBufferedBytes = 512 * 1024;
constexpr int kProtocol = 1;
constexpr int kHandshakeMs = 10000;
constexpr quint8 kInternalKind = 0;

QByteArray identityPublicKey(const QByteArray& privateSeed) {
    if (privateSeed.size() != kSecretSize) return {};
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(privateSeed.constData()), size_t(privateSeed.size()));
    if (!key) return {};
    QByteArray result(kSecretSize, Qt::Uninitialized);
    size_t length = size_t(result.size());
    const int ok = EVP_PKEY_get_raw_public_key(key,
        reinterpret_cast<unsigned char*>(result.data()), &length);
    EVP_PKEY_free(key);
    return ok == 1 && length == kSecretSize ? result : QByteArray();
}

QByteArray signatureData(const QByteArray& challenge) {
    return QByteArrayLiteral("KegaRefusion identity proof v1:") + challenge;
}

QByteArray signIdentity(const QByteArray& privateSeed, const QByteArray& challenge) {
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(privateSeed.constData()), size_t(privateSeed.size()));
    if (!key) return {};
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    QByteArray signature(64, Qt::Uninitialized);
    size_t length = size_t(signature.size());
    const QByteArray data = signatureData(challenge);
    const int ok = context && EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestSign(context, reinterpret_cast<unsigned char*>(signature.data()), &length,
                       reinterpret_cast<const unsigned char*>(data.constData()), size_t(data.size())) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return ok && length == 64 ? signature : QByteArray();
}

bool verifyIdentity(const QByteArray& publicKey, const QByteArray& challenge, const QByteArray& signature) {
    if (publicKey.size() != kSecretSize || challenge.size() != kSecretSize || signature.size() != 64) return false;
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(publicKey.constData()), size_t(publicKey.size()));
    if (!key) return false;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const QByteArray data = signatureData(challenge);
    const bool ok = context && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestVerify(context, reinterpret_cast<const unsigned char*>(signature.constData()),
                         size_t(signature.size()), reinterpret_cast<const unsigned char*>(data.constData()),
                         size_t(data.size())) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return ok;
}

QString fingerprint(const QByteArray& secret) {
    return QString::fromLatin1(QCryptographicHash::hash(secret, QCryptographicHash::Sha256).toHex());
}

bool equalSecret(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    return difference == 0;
}

QString cleanHost(QString host) {
    host = host.trimmed();
    if (host.size() > 2 && host.front() == QLatin1Char(91) && host.back() == QLatin1Char(93))
        host = host.mid(1, host.size() - 2);
    return host;
}

QString encoded(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray decoded(const QString& text) {
    return QByteArray::fromBase64(text.toLatin1(), QByteArray::Base64UrlEncoding);
}

QJsonObject peerJson(const PeerInfo& peer) {
    return {{QStringLiteral("id"), QString::number(peer.id)},
            {QStringLiteral("nickname"), peer.nickname},
            {QStringLiteral("identity"), peer.identityFingerprint},
            {QStringLiteral("role"), int(peer.role)}};
}

PeerInfo parsePeer(const QJsonObject& object) {
    PeerInfo peer;
    peer.id = object.value(QStringLiteral("id")).toString().toULongLong();
    peer.nickname = object.value(QStringLiteral("nickname")).toString();
    peer.identityFingerprint = object.value(QStringLiteral("identity")).toString();
    const int role = object.value(QStringLiteral("role")).toInt();
    peer.role = role >= 0 && role <= 2 ? Role(role) : Role::Spectator;
    peer.connected = true;
    return peer;
}

QByteArray json(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QList<QSslCipher> pskCiphers() {
    QList<QSslCipher> result;
    for (const auto& cipher : QSslConfiguration::supportedCiphers()) {
        if (cipher.protocol() == QSsl::TlsV1_2 &&
            (cipher.name() == QStringLiteral("DHE-PSK-AES256-GCM-SHA384") ||
             cipher.name() == QStringLiteral("ECDHE-PSK-CHACHA20-POLY1305") ||
             cipher.name() == QStringLiteral("DHE-PSK-CHACHA20-POLY1305")))
            result.append(cipher);
    }
    return result;
}

QString socketReason(QSslSocket* socket) {
    const QString error = socket->errorString();
    return error.isEmpty() ? QStringLiteral("Connection closed") : error;
}
} // namespace

class NetTransport::Server final : public QTcpServer {
public:
    using QTcpServer::QTcpServer;
    std::function<void(qintptr)> onIncoming;
protected:
    void incomingConnection(qintptr descriptor) override {
        if (onIncoming) onIncoming(descriptor);
    }
};

struct NetTransport::Connection {
    QSslSocket* socket = nullptr;
    QByteArray receive;
    quint64 peerId = 0;
    QString identityFingerprint;
    bool serverSide = false;
    bool authenticated = false;
    bool closing = false;
    QByteArray challenge;
    QByteArray pendingPublicKey;
    QString pendingNickname;
    QString pendingFingerprint;
    QElapsedTimer lastReceive;
    QElapsedTimer rateWindow;
    QElapsedTimer pingSchedule;
    QElapsedTimer pingSent;
    bool pingOutstanding = false;
    int framesInWindow = 0;
    int bytesInWindow = 0;
    int inputFramesInWindow = 0;
    int commandFramesInWindow = 0;
};

NetTransport::NetTransport(QObject* parent) : QObject(parent) {
    qRegisterMetaType<PeerInfo>();
    qRegisterMetaType<MessageKind>();
    qRegisterMetaType<Role>();
}

NetTransport::~NetTransport() { stop(); }

bool NetTransport::validateTls(QString* error) const {
    if (!QSslSocket::availableBackends().contains(QStringLiteral("openssl")) ||
        (QSslSocket::activeBackend() != QStringLiteral("openssl") &&
         !QSslSocket::setActiveBackend(QStringLiteral("openssl"))) ||
        !QSslSocket::supportsSsl() || pskCiphers().isEmpty()) {
        if (error) *error = QStringLiteral("TLS with PSK ciphers is unavailable. Install the Qt OpenSSL TLS backend and OpenSSL runtime.");
        return false;
    }
    return true;
}

bool NetTransport::host(const HostOptions& options, QString* error) {
    stop();
    if (options.inviteSecret.size() != kSecretSize || options.maxPeers < 1 || options.maxPeers > 64) {
        if (error) *error = QStringLiteral("Invalid invitation secret or peer limit");
        return false;
    }
    if (!validateTls(error)) return false;
    hostOptions_ = options;
    loadBans();
    server_ = new Server(this);
    server_->onIncoming = [this](qintptr descriptor) { accept(descriptor); };
    connect(server_, &QTcpServer::acceptError, this, [this](QAbstractSocket::SocketError) {
        emit transportError(server_->errorString());
    });
    if (!server_->listen(options.bindAddress, options.port)) {
        if (error) *error = server_->errorString();
        delete server_;
        server_ = nullptr;
        return false;
    }
    hosting_ = true;
    emit hosting(server_->serverPort());
    return true;
}

bool NetTransport::join(const JoinOptions& options, QString* error) {
    stop();
    if (cleanHost(options.host).isEmpty() || !options.port ||
        options.inviteSecret.size() != kSecretSize || identityPublicKey(options.identitySecret).size() != kSecretSize ||
        options.nickname.trimmed().isEmpty() || options.nickname.size() > 48) {
        if (error) *error = QStringLiteral("Invalid host, invitation, nickname, or identity");
        return false;
    }
    if (!validateTls(error)) return false;
    joinOptions_ = options;
    auto* socket = new QSslSocket(this);
    attach(socket, false);
    socket->connectToHostEncrypted(cleanHost(options.host), options.port);
    return true;
}

void NetTransport::accept(qintptr descriptor) {
    if (!hosting_ || connections_.size() >= hostOptions_.maxPeers + 8) {
        QTcpSocket reject;
        if (reject.setSocketDescriptor(descriptor)) reject.abort();
        return;
    }
    auto* socket = new QSslSocket(this);
    if (!socket->setSocketDescriptor(descriptor)) {
        socket->deleteLater();
        return;
    }
    attach(socket, true);
    socket->startServerEncryption();
}

void NetTransport::attach(QSslSocket* socket, bool serverSide) {
    auto* connection = new Connection;
    connection->socket = socket;
    connection->serverSide = serverSide;
    connections_.insert(connection);
    connection->lastReceive.start();
    connection->rateWindow.start();
    connection->pingSchedule.start();
    QSslConfiguration config = socket->sslConfiguration();
    config.setProtocol(QSsl::TlsV1_2);
    config.setCiphers(pskCiphers());
    config.setPeerVerifyMode(QSslSocket::VerifyNone);
    socket->setSslConfiguration(config);
    socket->setReadBufferSize(kMaxBufferedBytes);
    connect(socket, &QSslSocket::preSharedKeyAuthenticationRequired, this,
            [this, serverSide](QSslPreSharedKeyAuthenticator* auth) {
        auth->setIdentity(QByteArrayLiteral("KegaRefusion-v1"));
        auth->setPreSharedKey(serverSide ? hostOptions_.inviteSecret : joinOptions_.inviteSecret);
    });
    connect(socket, &QSslSocket::encrypted, this, [this, connection] {
        if (!connections_.contains(connection)) return;
        if (!connection->serverSide) {
            sendControl(connection, json({{QStringLiteral("type"), QStringLiteral("hello")},
                {QStringLiteral("protocol"), kProtocol},
                {QStringLiteral("nickname"), joinOptions_.nickname.trimmed()},
                {QStringLiteral("publicKey"), encoded(identityPublicKey(joinOptions_.identitySecret))},
                {QStringLiteral("password"), joinOptions_.password}}));
        }
    });
    connect(socket, &QSslSocket::readyRead, this, [this, connection] { read(connection); });
    connect(socket, &QSslSocket::disconnected, this, [this, connection] {
        if (connections_.contains(connection)) close(connection, QStringLiteral("Disconnected"));
    });
    connect(socket, &QSslSocket::errorOccurred, this, [this, connection](QAbstractSocket::SocketError) {
        if (connections_.contains(connection) && !connection->closing)
            close(connection, socketReason(connection->socket));
    });
    auto* heartbeat = new QTimer(socket);
    heartbeat->setInterval(1000);
    connect(heartbeat, &QTimer::timeout, this, [this, connection] {
        if (!connections_.contains(connection) || !connection->authenticated) return;
        if (connection->lastReceive.elapsed() > kDeadLinkMs ||
            (connection->pingOutstanding && connection->pingSent.elapsed() > kDeadLinkMs)) {
            close(connection, QStringLiteral("Connection timed out"));
            return;
        }
        if (!connection->pingOutstanding && connection->pingSchedule.elapsed() >= 2000) {
            connection->pingSchedule.restart();
            connection->pingSent.start();
            connection->pingOutstanding = true;
            sendControl(connection, json({{QStringLiteral("type"), QStringLiteral("ping")}}));
        }
    });
    heartbeat->start();
    QPointer<QSslSocket> guard(socket);
    QTimer::singleShot(kHandshakeMs, socket, [this, connection, guard] {
        if (guard && connections_.contains(connection) && !connection->authenticated)
            close(connection, QStringLiteral("Authentication timed out"));
    });
}

void NetTransport::read(Connection* connection) {
    if (!connections_.contains(connection)) return;
    int bytesRead = 0;
    int frames = 0;
    auto* socket = connection->socket;
    while (socket->bytesAvailable() > 0 && bytesRead < kMaxBufferedBytes && frames < 32) {
        const QByteArray chunk = socket->read(qMin<qint64>(socket->bytesAvailable(), 64 * 1024));
        if (chunk.isEmpty()) break;
        bytesRead += chunk.size();
        connection->receive.append(chunk);
        if (connection->receive.size() > kMaxBufferedBytes) {
            close(connection, QStringLiteral("Receive buffer limit exceeded"));
            return;
        }
        while (connection->receive.size() >= 4 && frames < 32) {
            const auto* p = reinterpret_cast<const unsigned char*>(connection->receive.constData());
            const quint32 length = (quint32(p[0]) << 24) | (quint32(p[1]) << 16) |
                                   (quint32(p[2]) << 8) | quint32(p[3]);
            if (length < 1 || length > kMaxFrame) {
                close(connection, QStringLiteral("Invalid frame length"));
                return;
            }
            if (connection->receive.size() < qsizetype(length + 4)) break;
            const auto kind = MessageKind(p[4]);
            if (connection->rateWindow.elapsed() >= 1000) {
                connection->rateWindow.restart();
                connection->framesInWindow = 0;
                connection->bytesInWindow = 0;
                connection->inputFramesInWindow = 0;
                connection->commandFramesInWindow = 0;
            }
            ++connection->framesInWindow;
            if (kind == MessageKind::Input) ++connection->inputFramesInWindow;
            if (kind == MessageKind::Command) ++connection->commandFramesInWindow;
            connection->bytesInWindow += int(length + 4);
            if (connection->framesInWindow > kMaxFramesPerSecond ||
                connection->bytesInWindow > kMaxBytesPerSecond ||
                connection->inputFramesInWindow > 240 ||
                connection->commandFramesInWindow > 60) {
                close(connection, QStringLiteral("Incoming rate limit exceeded"));
                return;
            }
            connection->lastReceive.restart();
            const QByteArray payload = connection->receive.mid(5, length - 1);
            connection->receive.remove(0, length + 4);
            ++frames;
            process(connection, kind, payload);
            if (!connections_.contains(connection)) return;
        }
    }
    if (connections_.contains(connection) && (socket->bytesAvailable() > 0 ||
         (connection->receive.size() >= 4 &&
          connection->receive.size() >= qsizetype(4 +
             ((quint32(quint8(connection->receive[0])) << 24) |
              (quint32(quint8(connection->receive[1])) << 16) |
              (quint32(quint8(connection->receive[2])) << 8) |
              quint32(quint8(connection->receive[3]))))))) {
        QTimer::singleShot(0, socket, [this, connection] { if (connections_.contains(connection)) read(connection); });
    }
}

void NetTransport::process(Connection* connection, MessageKind kind, const QByteArray& payload) {
    if (quint8(kind) == kInternalKind) {
        if (payload.size() > 16 * 1024) { close(connection, QStringLiteral("Control message too large")); return; }
        const auto document = QJsonDocument::fromJson(payload);
        if (!document.isObject()) { close(connection, QStringLiteral("Invalid control message")); return; }
        const auto object = document.object();
        const QString type = object.value(QStringLiteral("type")).toString();
        if (!connection->authenticated) {
            if (connection->serverSide && type == QStringLiteral("hello")) handleHello(connection, payload);
            else if (connection->serverSide && type == QStringLiteral("proof")) handleProof(connection, payload);
            else if (!connection->serverSide && type == QStringLiteral("challenge")) handleChallenge(connection, payload);
            else if (!connection->serverSide && type == QStringLiteral("welcome")) handleWelcome(connection, payload);
            else close(connection, QStringLiteral("Unexpected authentication message"));
            return;
        }
        if (type == QStringLiteral("ping")) {
            sendControl(connection, json({{QStringLiteral("type"), QStringLiteral("pong")}}));
            return;
        }
        if (type == QStringLiteral("pong")) {
            if (connection->pingOutstanding) {
                connection->pingOutstanding = false;
                emit peerLatency(connection->peerId, int(connection->pingSent.elapsed()));
            }
            return;
        }
        if (connection->serverSide) { close(connection, QStringLiteral("Unauthorized control message")); return; }
        if (type == QStringLiteral("joined")) {
            const PeerInfo peer = parsePeer(object.value(QStringLiteral("peer")).toObject());
            if (peer.id) {
                if (!sessionPeers_.contains(peer.identityFingerprint) && sessionPeers_.size() >= 512) {
                    for (auto it = sessionPeers_.begin(); it != sessionPeers_.end(); ++it) {
                        if (!it->connected) { sessionPeers_.erase(it); break; }
                    }
                }
                sessionPeers_.insert(peer.identityFingerprint, peer);
                emit peerJoined(peer);
            }
        } else if (type == QStringLiteral("left")) {
            const quint64 id = object.value(QStringLiteral("id")).toString().toULongLong();
            const QString reason = object.value(QStringLiteral("reason")).toString();
            for (auto it = sessionPeers_.begin(); it != sessionPeers_.end(); ++it)
                if (it->id == id) { it->connected = false; break; }
            emit peerLeft(id, reason);
        } else if (type == QStringLiteral("role")) {
            const quint64 id = object.value(QStringLiteral("id")).toString().toULongLong();
            const int role = object.value(QStringLiteral("role")).toInt(-1);
            if (role < 0 || role > 2) return;
            for (auto it = sessionPeers_.begin(); it != sessionPeers_.end(); ++it)
                if (it->id == id) { it->role = Role(role); break; }
            emit peerRoleChanged(id, Role(role));
        } else if (type == QStringLiteral("kick")) {
            close(connection, object.value(QStringLiteral("reason")).toString(QStringLiteral("Kicked by host")));
        } else {
            close(connection, QStringLiteral("Invalid control message"));
        }
        return;
    }
    if (!connection->authenticated || quint8(kind) < 16 || quint8(kind) > 23 ||
        payload.size() > maxPayloadFor(kind) ||
        (connection->serverSide && (kind == MessageKind::Frame ||
                                    kind == MessageKind::Snapshot || kind == MessageKind::Content))) {
        close(connection, QStringLiteral("Unexpected message"));
        return;
    }
    emit messageReceived(connection->peerId, kind, payload);
}

void NetTransport::handleHello(Connection* connection, const QByteArray& payload) {
    if (!connection->challenge.isEmpty()) { close(connection, QStringLiteral("Duplicate hello")); return; }
    const auto object = QJsonDocument::fromJson(payload).object();
    const QString nickname = object.value(QStringLiteral("nickname")).toString().trimmed();
    const QByteArray publicKey = decoded(object.value(QStringLiteral("publicKey")).toString());
    const QString identity = fingerprint(publicKey);
    const QString remoteAddress = connection->socket->peerAddress().toString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto failures = passwordFailures_.value(remoteAddress);
    if (failures.first && now - failures.first >= 60000) {
        passwordFailures_.remove(remoteAddress);
        failures = {};
    }
    if (failures.second >= 5) {
        close(connection, QStringLiteral("Too many password attempts"));
        return;
    }
    const bool passwordMatches = equalSecret(
        object.value(QStringLiteral("password")).toString().toUtf8(), hostOptions_.password.toUtf8());
    if (!passwordMatches) {
        if (passwordFailures_.size() >= 256 && !passwordFailures_.contains(remoteAddress)) {
            for (auto it = passwordFailures_.begin(); it != passwordFailures_.end(); ) {
                if (now - it.value().first >= 60000) it = passwordFailures_.erase(it);
                else ++it;
            }
            if (passwordFailures_.size() >= 256) passwordFailures_.erase(passwordFailures_.begin());
        }
        passwordFailures_.insert(remoteAddress,
            qMakePair(failures.first ? failures.first : now, failures.second + 1));
    }
    int active = 0;
    bool replacing = false;
    for (auto* candidate : connections_) if (candidate->authenticated) {
        ++active;
        if (candidate->identityFingerprint == identity) replacing = true;
    }
    if (object.value(QStringLiteral("protocol")).toInt() != kProtocol || nickname.isEmpty() ||
        nickname.size() > 48 || publicKey.size() != kSecretSize ||
        (active >= hostOptions_.maxPeers && !replacing) || bannedIdentities_.contains(identity) ||
        sessionBannedIdentities_.contains(identity) ||
        !passwordMatches) {
        close(connection, QStringLiteral("Access denied or lobby full"));
        return;
    }
    passwordFailures_.remove(remoteAddress);
    connection->challenge = generateSecret();
    if (connection->challenge.size() != kSecretSize) {
        close(connection, QStringLiteral("Random generator unavailable"));
        return;
    }
    connection->pendingPublicKey = publicKey;
    connection->pendingNickname = nickname;
    connection->pendingFingerprint = identity;
    sendControl(connection, json({{QStringLiteral("type"), QStringLiteral("challenge")},
                                  {QStringLiteral("nonce"), encoded(connection->challenge)}}));
}

void NetTransport::handleChallenge(Connection* connection, const QByteArray& payload) {
    if (!connection->challenge.isEmpty()) { close(connection, QStringLiteral("Duplicate challenge")); return; }
    const auto object = QJsonDocument::fromJson(payload).object();
    const QByteArray nonce = decoded(object.value(QStringLiteral("nonce")).toString());
    if (nonce.size() != kSecretSize) { close(connection, QStringLiteral("Invalid challenge")); return; }
    connection->challenge = nonce;
    const QByteArray signature = signIdentity(joinOptions_.identitySecret, nonce);
    if (signature.size() != 64) { close(connection, QStringLiteral("Identity signing failed")); return; }
    sendControl(connection, json({{QStringLiteral("type"), QStringLiteral("proof")},
                                  {QStringLiteral("signature"), encoded(signature)}}));
}

void NetTransport::handleProof(Connection* connection, const QByteArray& payload) {
    if (connection->challenge.size() != kSecretSize || connection->pendingPublicKey.size() != kSecretSize) {
        close(connection, QStringLiteral("Unexpected identity proof"));
        return;
    }
    const QByteArray signature = decoded(QJsonDocument::fromJson(payload).object()
        .value(QStringLiteral("signature")).toString());
    if (!verifyIdentity(connection->pendingPublicKey, connection->challenge, signature)) {
        close(connection, QStringLiteral("Identity proof failed"));
        return;
    }
    const QString identity = connection->pendingFingerprint;
    int active = 0;
    bool replacing = false;
    for (auto* candidate : connections_) if (candidate->authenticated) {
        ++active;
        if (candidate->identityFingerprint == identity) replacing = true;
    }
    if (active >= hostOptions_.maxPeers && !replacing) {
        close(connection, QStringLiteral("Lobby full"));
        return;
    }
    if (bannedIdentities_.contains(identity) || sessionBannedIdentities_.contains(identity)) {
        close(connection, QStringLiteral("Access denied"));
        return;
    }
    PeerInfo peer;
    if (sessionPeers_.contains(identity)) peer = sessionPeers_.value(identity);
    else {
        if (sessionPeers_.size() >= 512) {
            for (auto it = sessionPeers_.begin(); it != sessionPeers_.end(); ++it) {
                if (!it->connected) { sessionPeers_.erase(it); break; }
            }
        }
        peer.id = nextPeerId_++;
        peer.role = Role::Spectator;
    }
    for (auto* previous : QList<Connection*>(connections_.begin(), connections_.end())) {
        if (connections_.contains(previous) && previous != connection &&
            previous->authenticated && previous->identityFingerprint == identity) {
            close(previous, QStringLiteral("Reconnected from another connection"));
            break;
        }
    }
    peer.nickname = connection->pendingNickname;
    peer.identityFingerprint = identity;
    peer.connected = true;
    connection->identityFingerprint = identity;
    connection->peerId = peer.id;
    connection->authenticated = true;
    connection->pingSchedule.restart();
    connection->challenge.clear();
    connection->pendingPublicKey.clear();
    sessionPeers_.insert(identity, peer);
    QJsonArray roster;
    for (auto it = sessionPeers_.cbegin(); it != sessionPeers_.cend(); ++it)
        if (it->connected && it->id != peer.id) roster.append(peerJson(*it));
    sendControl(connection, json({{QStringLiteral("type"), QStringLiteral("welcome")},
        {QStringLiteral("protocol"), kProtocol}, {QStringLiteral("self"), peerJson(peer)},
        {QStringLiteral("peers"), roster}}));
    if (!connections_.contains(connection)) return;
    const QByteArray joined = json({{QStringLiteral("type"), QStringLiteral("joined")},
                                    {QStringLiteral("peer"), peerJson(peer)}});
    for (auto* other : QList<Connection*>(connections_.begin(), connections_.end()))
        if (connections_.contains(other) && other != connection && other->authenticated) sendControl(other, joined);
    emit peerJoined(peer);
}

void NetTransport::handleWelcome(Connection* connection, const QByteArray& payload) {
    const auto object = QJsonDocument::fromJson(payload).object();
    const PeerInfo self = parsePeer(object.value(QStringLiteral("self")).toObject());
    if (object.value(QStringLiteral("protocol")).toInt() != kProtocol || !self.id ||
        self.identityFingerprint != fingerprint(identityPublicKey(joinOptions_.identitySecret))) {
        close(connection, QStringLiteral("Invalid welcome message"));
        return;
    }
    connection->peerId = 0; // The connected endpoint is the authoritative host.
    connection->authenticated = true;
    connection->pingSchedule.restart();
    localPeerId_ = self.id;
    sessionPeers_.insert(self.identityFingerprint, self);
    for (const auto& value : object.value(QStringLiteral("peers")).toArray()) {
        const PeerInfo peer = parsePeer(value.toObject());
        if (peer.id) {
            if (!sessionPeers_.contains(peer.identityFingerprint) && sessionPeers_.size() >= 512) {
                for (auto it = sessionPeers_.begin(); it != sessionPeers_.end(); ++it) {
                    if (!it->connected) { sessionPeers_.erase(it); break; }
                }
            }
            sessionPeers_.insert(peer.identityFingerprint, peer);
            emit peerJoined(peer);
        }
    }
    emit connected(self);
}

bool NetTransport::enqueue(Connection* connection, MessageKind kind, const QByteArray& payload) {
    if (!connection || !connections_.contains(connection) || payload.size() + 1 > kMaxFrame ||
        (quint8(kind) != kInternalKind && payload.size() > maxPayloadFor(kind)) ||
        connection->socket->state() != QAbstractSocket::ConnectedState ||
        !connection->socket->isEncrypted()) return false;
    const qint64 length = payload.size() + 1;
    if (connection->socket->bytesToWrite() + length + 4 > kMaxQueuedBytes) {
        close(connection, QStringLiteral("Send queue limit exceeded"));
        return false;
    }
    QByteArray frame;
    frame.resize(5 + payload.size());
    frame[0] = char((length >> 24) & 0xff);
    frame[1] = char((length >> 16) & 0xff);
    frame[2] = char((length >> 8) & 0xff);
    frame[3] = char(length & 0xff);
    frame[4] = char(quint8(kind));
    if (!payload.isEmpty()) memcpy(frame.data() + 5, payload.constData(), size_t(payload.size()));
    if (connection->socket->write(frame) != frame.size()) {
        close(connection, QStringLiteral("Send failed"));
        return false;
    }
    return true;
}

void NetTransport::sendControl(Connection* connection, const QByteArray& data) {
    enqueue(connection, MessageKind(kInternalKind), data);
}

NetTransport::Connection* NetTransport::byPeerId(quint64 peerId) const {
    for (auto* connection : connections_)
        if (connection->authenticated && connection->peerId == peerId) return connection;
    return nullptr;
}

bool NetTransport::send(quint64 peerId, MessageKind kind, const QByteArray& payload) {
    if (quint8(kind) < 16 || quint8(kind) > 23) return false;
    return enqueue(byPeerId(peerId), kind, payload);
}

void NetTransport::broadcast(MessageKind kind, const QByteArray& payload) {
    if (quint8(kind) < 16 || quint8(kind) > 23) return;
    for (auto* connection : QList<Connection*>(connections_.begin(), connections_.end()))
        if (connections_.contains(connection) && connection->authenticated) enqueue(connection, kind, payload);
}

bool NetTransport::kick(quint64 peerId, bool persistentBan, bool banSession) {
    if (!hosting_) return false;
    auto* connection = byPeerId(peerId);
    if (!connection) return false;
    if (persistentBan && hostOptions_.banFilePath.isEmpty()) {
        emit transportError(QStringLiteral("Persistent ban file path is missing"));
        return false;
    }
    if (banSession && !sessionBannedIdentities_.contains(connection->identityFingerprint) &&
        sessionBannedIdentities_.size() >= 4096) {
        emit transportError(QStringLiteral("Session ban limit exceeded"));
        return false;
    }
    if (persistentBan) {
        const QString identity = connection->identityFingerprint;
        const bool hadBan = bannedIdentities_.contains(identity);
        const QString previousName = bannedIdentities_.value(identity);
        bannedIdentities_.insert(identity, sessionPeers_.value(identity).nickname);
        if (!saveBans()) {
            if (hadBan) bannedIdentities_.insert(identity, previousName);
            else bannedIdentities_.remove(identity);
            return false;
        }
    }
    if (banSession) sessionBannedIdentities_.insert(connection->identityFingerprint);
    sendControl(connection, json({{QStringLiteral("type"), QStringLiteral("kick")},
        {QStringLiteral("reason"), QStringLiteral("Kicked by host")}}));
    if (!connections_.contains(connection)) return true;
    connection->socket->disconnectFromHost();
    QPointer<QSslSocket> guard(connection->socket);
    QTimer::singleShot(1000, connection->socket, [this, connection, guard] {
        if (guard && connections_.contains(connection)) close(connection, QStringLiteral("Kicked by host"));
    });
    return true;
}

QJsonArray NetTransport::bans() const {
    QJsonArray result;
    QSet<QString> identities = sessionBannedIdentities_;
    for (auto it = bannedIdentities_.cbegin(); it != bannedIdentities_.cend(); ++it)
        identities.insert(it.key());
    for (const QString& identity : identities) {
        const QString nickname = bannedIdentities_.contains(identity)
            ? bannedIdentities_.value(identity) : sessionPeers_.value(identity).nickname;
        result.append(QJsonObject{{QStringLiteral("fingerprint"), identity},
                                  {QStringLiteral("nickname"), nickname},
                                  {QStringLiteral("persistent"), bannedIdentities_.contains(identity)}});
    }
    return result;
}

bool NetTransport::unban(const QString& identityFingerprint) {
    const bool hadPersistent = bannedIdentities_.contains(identityFingerprint);
    const QString oldName = bannedIdentities_.value(identityFingerprint);
    const bool hadSession = sessionBannedIdentities_.contains(identityFingerprint);
    if (!hadPersistent && !hadSession) return false;
    bannedIdentities_.remove(identityFingerprint);
    sessionBannedIdentities_.remove(identityFingerprint);
    if (hadPersistent && !saveBans()) {
        bannedIdentities_.insert(identityFingerprint, oldName);
        if (hadSession) sessionBannedIdentities_.insert(identityFingerprint);
        return false;
    }
    return true;
}

bool NetTransport::setRole(quint64 peerId, Role role) {
    if (!hosting_ || int(role) > 2) return false;
    auto peer = sessionPeers_.end();
    for (auto it = sessionPeers_.begin(); it != sessionPeers_.end(); ++it) {
        if (it->id == peerId) { peer = it; break; }
    }
    if (peer == sessionPeers_.end()) return false;
    peer->role = role;
    const QByteArray data = json({{QStringLiteral("type"), QStringLiteral("role")},
        {QStringLiteral("id"), QString::number(peerId)}, {QStringLiteral("role"), int(role)}});
    for (auto* other : QList<Connection*>(connections_.begin(), connections_.end()))
        if (connections_.contains(other) && other->authenticated) sendControl(other, data);
    emit peerRoleChanged(peerId, role);
    return true;
}

void NetTransport::close(Connection* connection, const QString& reason) {
    if (!connections_.remove(connection)) return;
    connection->closing = true;
    if (connection->authenticated && connection->serverSide) {
        auto it = sessionPeers_.find(connection->identityFingerprint);
        if (it != sessionPeers_.end() && it->id == connection->peerId) it->connected = false;
        const QByteArray left = json({{QStringLiteral("type"), QStringLiteral("left")},
            {QStringLiteral("id"), QString::number(connection->peerId)}, {QStringLiteral("reason"), reason}});
        for (auto* other : QList<Connection*>(connections_.begin(), connections_.end()))
            if (connections_.contains(other) && other->authenticated) sendControl(other, left);
        emit peerLeft(connection->peerId, reason);
    } else if (connection->authenticated) {
        localPeerId_ = 0;
        emit peerLeft(0, reason);
    } else if (!connection->serverSide) {
        emit transportError(reason);
    }
    connection->socket->disconnect(this);
    connection->socket->abort();
    connection->socket->deleteLater();
    delete connection;
}

void NetTransport::stop() {
    if (server_) { server_->close(); delete server_; server_ = nullptr; }
    const bool wasActive = hosting_ || !connections_.isEmpty();
    hosting_ = false;
    for (auto* connection : QList<Connection*>(connections_.begin(), connections_.end()))
        if (connections_.contains(connection)) close(connection, QStringLiteral("Session stopped"));
    sessionPeers_.clear();
    bannedIdentities_.clear();
    sessionBannedIdentities_.clear();
    passwordFailures_.clear();
    hostOptions_ = {};
    joinOptions_ = {};
    nextPeerId_ = 1;
    localPeerId_ = 0;
    if (wasActive) emit stopped();
}

bool NetTransport::isHosting() const { return hosting_; }
bool NetTransport::isConnected() const {
    for (auto* connection : connections_) if (!connection->serverSide && connection->authenticated) return true;
    return false;
}
quint16 NetTransport::localPort() const { return server_ ? server_->serverPort() : 0; }
quint64 NetTransport::localPeerId() const { return localPeerId_; }
QList<PeerInfo> NetTransport::peers() const {
    QList<PeerInfo> result;
    for (auto it = sessionPeers_.cbegin(); it != sessionPeers_.cend(); ++it)
        if (it->connected) result.append(*it);
    return result;
}

int NetTransport::maxPayloadFor(MessageKind kind) {
    switch (kind) {
    case MessageKind::Input: return 4096;
    case MessageKind::Frame: return 64 * 1024;
    case MessageKind::Command: return 16 * 1024;
    case MessageKind::Snapshot: return maxPayloadBytes;
    case MessageKind::Content: return maxPayloadBytes;
    case MessageKind::FileRequest: return 16 * 1024;
    case MessageKind::FileChunk: return maxPayloadBytes;
    case MessageKind::Control: return 16 * 1024;
    }
    return 0;
}

QByteArray NetTransport::generateSecret() {
    QByteArray random(kSecretSize, Qt::Uninitialized);
    return RAND_bytes(reinterpret_cast<unsigned char*>(random.data()), kSecretSize) == 1
        ? random : QByteArray();
}

QByteArray NetTransport::loadOrCreateIdentity(const QString& path, QString* error) {
    if (path.isEmpty()) { if (error) *error = QStringLiteral("Identity path is empty"); return {}; }
    QFile existing(path);
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly)) {
            if (error) *error = existing.errorString();
            return {};
        }
        const QByteArray data = existing.read(33);
        if (data.size() != kSecretSize) {
            if (error) *error = QStringLiteral("Identity file has an invalid length");
            return {};
        }
        return data;
    }
    const QByteArray identity = generateSecret();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create identity directory");
        return {};
    }
    if (identity.size() != kSecretSize) {
        if (error) *error = QStringLiteral("Random generator unavailable");
        return {};
    }
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error) *error = output.errorString();
        return {};
    }
    output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (output.write(identity) != identity.size() || !output.commit()) {
        if (error) *error = output.errorString();
        return {};
    }
    return identity;
}

QString NetTransport::makeInvitation(const QString& publicHost, quint16 port, const QByteArray& secret) {
    if (cleanHost(publicHost).isEmpty() || !port || secret.size() != kSecretSize) return {};
    QUrl url;
    url.setScheme(QStringLiteral("kegarefusion"));
    url.setHost(QStringLiteral("join"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("host"), cleanHost(publicHost));
    query.addQueryItem(QStringLiteral("port"), QString::number(port));
    query.addQueryItem(QStringLiteral("key"), encoded(secret));
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

bool NetTransport::parseInvitation(const QString& invitation, QString* host, quint16* port,
                                    QByteArray* secret, QString* error) {
    const QUrl url(invitation);
    const QUrlQuery query(url);
    bool ok = false;
    const uint parsedPort = query.queryItemValue(QStringLiteral("port")).toUInt(&ok);
    const QString parsedHost = cleanHost(query.queryItemValue(QStringLiteral("host")));
    const QString key = query.queryItemValue(QStringLiteral("key"));
    const QByteArray parsedSecret = decoded(key);
    if (!url.isValid() || url.scheme() != QStringLiteral("kegarefusion") ||
        url.host() != QStringLiteral("join") || parsedHost.isEmpty() || !ok ||
        parsedPort == 0 || parsedPort > std::numeric_limits<quint16>::max() ||
        parsedSecret.size() != kSecretSize || encoded(parsedSecret) != key) {
        if (error) *error = QStringLiteral("Invalid invitation link");
        return false;
    }
    if (host) *host = parsedHost;
    if (port) *port = quint16(parsedPort);
    if (secret) *secret = parsedSecret;
    return true;
}

void NetTransport::loadBans() {
    bannedIdentities_.clear();
    sessionBannedIdentities_.clear();
    if (hostOptions_.banFilePath.isEmpty()) return;
    QFile file(hostOptions_.banFilePath);
    if (!file.exists()) return;
    if (!file.open(QIODevice::ReadOnly)) { emit transportError(file.errorString()); return; }
    if (file.size() > 64 * 1024) { emit transportError(QStringLiteral("Ban file is too large")); return; }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isObject()) {
        const auto object = doc.object();
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it.key().size() == 64 && it.value().isString())
                bannedIdentities_.insert(it.key(), it.value().toString());
    } else if (doc.isArray()) {
        // Read the earlier array form so a current build can retain existing bans.
        for (const auto& value : doc.array()) {
            const QJsonObject item = value.toObject();
            const QString identity = value.isString() ? value.toString()
                : item.value(QStringLiteral("fingerprint")).toString();
            if (identity.size() == 64)
                bannedIdentities_.insert(identity, item.value(QStringLiteral("nickname")).toString());
        }
    } else {
        emit transportError(QStringLiteral("Ban file is invalid"));
    }
}

bool NetTransport::saveBans() {
    if (hostOptions_.banFilePath.isEmpty()) return false;
    if (!QDir().mkpath(QFileInfo(hostOptions_.banFilePath).absolutePath())) {
        emit transportError(QStringLiteral("Cannot create ban directory"));
        return false;
    }
    QSaveFile file(hostOptions_.banFilePath);
    if (!file.open(QIODevice::WriteOnly)) { emit transportError(file.errorString()); return false; }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QJsonObject object;
    for (auto it = bannedIdentities_.cbegin(); it != bannedIdentities_.cend(); ++it)
        object.insert(it.key(), it.value());
    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (data.size() > 64 * 1024) {
        emit transportError(QStringLiteral("Ban file limit exceeded"));
        file.cancelWriting();
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        emit transportError(file.errorString());
        return false;
    }
    return true;
}

} // namespace kega::net
