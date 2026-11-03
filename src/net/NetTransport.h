#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>

class QSslSocket;
class QTcpServer;

namespace kega::net {

enum class MessageKind : quint8 {
    Input = 16,
    Frame = 17,
    Command = 18,
    Snapshot = 19,
    Content = 20,
    FileRequest = 21,
    FileChunk = 22,
    Control = 23
};

enum class Role : quint8 { Spectator = 0, Player1 = 1, Player2 = 2, Player3 = 3, Player4 = 4, Player5 = 5, Player6 = 6, Player7 = 7, Player8 = 8 };

struct PeerInfo {
    quint64 id = 0;
    QString nickname;
    QString identityFingerprint;
    Role role = Role::Spectator;
    bool connected = false;
};

struct HostOptions {
    QHostAddress bindAddress = QHostAddress::Any;
    quint16 port = 0;
    QByteArray inviteSecret; // Exactly 32 random bytes, shared in the invitation.
    QString password;        // Optional additional gate, checked after TLS authentication.
    QString banFilePath;     // Optional JSON file; host settings directory is suitable.
    int maxPeers = 16;
};

struct JoinOptions {
    QString host;
    quint16 port = 0;
    QByteArray inviteSecret; // Exactly 32 bytes from the invitation.
    QString password;
    QString nickname;
    QByteArray identitySecret; // Stable, private 32-byte Ed25519 seed from a separate identity file.
};

class NetTransport final : public QObject {
    Q_OBJECT
public:
    explicit NetTransport(QObject* parent = nullptr);
    ~NetTransport() override;

    // All methods must be called on the object thread. Sockets are asynchronous and never use waitFor*.
    bool host(const HostOptions& options, QString* error = nullptr);
    bool join(const JoinOptions& options, QString* error = nullptr);
    void stop();
    bool send(quint64 peerId, MessageKind kind, const QByteArray& payload);
    void broadcast(MessageKind kind, const QByteArray& payload);
    bool kick(quint64 peerId, bool persistentBan = false, bool banSession = true);
    QJsonArray bans() const;
    bool unban(const QString& identityFingerprint);
    bool setRole(quint64 peerId, Role role);

    bool isHosting() const;
    bool isConnected() const;
    quint16 localPort() const;
    QList<PeerInfo> peers() const;
    quint64 localPeerId() const;
    QHostAddress peerAddress(quint64 peerId) const;

    static constexpr int maxPayloadBytes = 256 * 1024 - 1;
    static constexpr qint64 maxQueuedBytes = 4 * 1024 * 1024;
    static int maxPayloadFor(MessageKind kind);
    static QByteArray generateSecret();
    static QByteArray loadOrCreateIdentity(const QString& path, QString* error = nullptr);
    static QString makeInvitation(const QString& publicHost, quint16 port, const QByteArray& secret);
    static bool parseInvitation(const QString& invitation, QString* host, quint16* port,
                                QByteArray* secret, QString* error = nullptr);

signals:
    void hosting(quint16 port);
    void connected(const kega::net::PeerInfo& self);
    void peerJoined(const kega::net::PeerInfo& peer);
    void peerLeft(quint64 peerId, const QString& reason);
    // An explicit host decision, not an unexpected loss of the connection.
    void sessionEnded(const QString& cause, const QString& detail);
    void peerRoleChanged(quint64 peerId, kega::net::Role role);
    void peerLatency(quint64 peerId, int milliseconds);
    void messageReceived(quint64 peerId, kega::net::MessageKind kind, const QByteArray& payload);
    void transportError(const QString& message);
    void stopped();

private:
    struct Connection;
    class Server;
    void accept(qintptr descriptor);
    void attach(QSslSocket* socket, bool serverSide);
    void read(Connection* connection);
    void process(Connection* connection, MessageKind kind, const QByteArray& payload);
    void close(Connection* connection, const QString& reason);
    bool enqueue(Connection* connection, MessageKind kind, const QByteArray& payload);
    void sendControl(Connection* connection, const QByteArray& json);
    void reject(Connection* connection, const QString& reason, const QString& cause = QStringLiteral("rejected"));
    void handleHello(Connection* connection, const QByteArray& payload);
    void handleWelcome(Connection* connection, const QByteArray& payload);
    void handleChallenge(Connection* connection, const QByteArray& payload);
    void handleProof(Connection* connection, const QByteArray& payload);
    void loadBans();
    bool saveBans();
    bool validateTls(QString* error) const;
    Connection* byPeerId(quint64 peerId) const;

    Server* server_ = nullptr;
    QSet<Connection*> connections_;
    HostOptions hostOptions_;
    JoinOptions joinOptions_;
    QHash<QString, QString> bannedIdentities_;
    QSet<QString> sessionBannedIdentities_;
    QHash<QString, QPair<qint64, int>> passwordFailures_;
    QHash<QString, PeerInfo> sessionPeers_;
    quint64 nextPeerId_ = 1;
    quint64 localPeerId_ = 0;
    bool hosting_ = false;
};

} // namespace kega::net

Q_DECLARE_METATYPE(kega::net::PeerInfo)
Q_DECLARE_METATYPE(kega::net::MessageKind)
Q_DECLARE_METATYPE(kega::net::Role)
