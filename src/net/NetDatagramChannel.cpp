#include "net/NetDatagramChannel.h"

#include <QNetworkProxy>
#include <QtEndian>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace kega::net {
namespace {
constexpr int kHeaderBytes = 34;
constexpr int kTagBytes = 32;
constexpr int kMaxDatagramBytes = kHeaderBytes + NetDatagramChannel::maxPayloadBytes + kTagBytes;
constexpr int kDrainBatch = 64;
constexpr int kPeerPacketsPerSecond = 8192;
constexpr qint64 kPeerBytesPerSecond = 64LL * 1024 * 1024;
constexpr int kAllPacketsPerSecond = 32768;
constexpr qint64 kAllBytesPerSecond = 256LL * 1024 * 1024;
constexpr qint64 kMaxQueuedBytes = 256 * 1024;
constexpr quint8 kVersion = 1;
constexpr quint8 kClientToHost = 1;
constexpr quint8 kHostToClient = 2;
}

bool NetDatagramChannel::ReplayWindow::accept(quint64 sequence) {
    if (!sequence) return false;
    if (sequence > newest) {
        const quint64 shift = sequence - newest;
        bits = shift >= 64 ? 1 : (bits << shift) | 1;
        newest = sequence;
        return true;
    }
    const quint64 behind = newest - sequence;
    if (behind >= 64 || (bits & (quint64(1) << behind))) return false;
    bits |= quint64(1) << behind;
    return true;
}

bool NetDatagramChannel::RateWindow::accept(qint64 nowMs, int size, int maxPackets, qint64 maxBytes) {
    if (nowMs - sinceMs >= 1000) { sinceMs = nowMs; packets = 0; bytes = 0; }
    if (packets >= maxPackets || bytes > maxBytes - size) return false;
    ++packets;
    bytes += size;
    return true;
}

QHostAddress NetDatagramChannel::normalized(const QHostAddress& address) {
    bool isV4 = false;
    const quint32 v4 = address.toIPv4Address(&isV4);
    return isV4 ? QHostAddress(v4) : address;
}

void NetDatagramChannel::eraseKey(QByteArray& key) {
    if (!key.isEmpty()) OPENSSL_cleanse(key.data(), size_t(key.size()));
    key.clear();
}

NetDatagramChannel::NetDatagramChannel(QObject* parent) : QObject(parent) {
    socket_.setProxy(QNetworkProxy::NoProxy);
    connect(&socket_, &QUdpSocket::readyRead, this, &NetDatagramChannel::readPending);
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, &NetDatagramChannel::tick);
}

NetDatagramChannel::~NetDatagramChannel() { stop(); }

bool NetDatagramChannel::host(quint16 port, QString* errorText) {
    stop();
    if (!socket_.bind(QHostAddress::Any, port)) {
        const QString message = socket_.errorString();
        if (errorText) *errorText = message;
        emit error(message);
        return false;
    }
    mode_ = Mode::Host;
    clock_.start();
    timer_.start();
    return true;
}

bool NetDatagramChannel::join(const QHostAddress& hostAddress, quint16 port, quint64 localId,
                              const QByteArray& key, QString* errorText) {
    stop();
    const QHostAddress address = normalized(hostAddress);
    if (!localId || !port || key.size() != 32 || address.isNull() || address.isMulticast() ||
        address == QHostAddress::Any || address == QHostAddress::AnyIPv4 ||
        address == QHostAddress::AnyIPv6 || address == QHostAddress::Broadcast) {
        const QString message = QStringLiteral("Invalid UDP host, peer ID or session key");
        if (errorText) *errorText = message;
        emit error(message);
        return false;
    }
    const QHostAddress bindAddress = address.protocol() == QAbstractSocket::IPv4Protocol
        ? QHostAddress::AnyIPv4 : QHostAddress::AnyIPv6;
    if (!socket_.bind(bindAddress, 0)) {
        const QString message = socket_.errorString();
        if (errorText) *errorText = message;
        emit error(message);
        return false;
    }
    mode_ = Mode::Client;
    localId_ = localId;
    hostPeer_.key = key;
    hostPeer_.address = address;
    hostPeer_.port = port;
    clock_.start();
    write(hostPeer_, localId_, 0, Kind::Hello, {});
    timer_.start();
    return true;
}

bool NetDatagramChannel::addPeer(quint64 peerId, const QByteArray& key) {
    if (mode_ != Mode::Host || !peerId || key.size() != 32 ||
        (peers_.size() >= 64 && !peers_.contains(peerId))) return false;
    for (auto it = peers_.cbegin(); it != peers_.cend(); ++it)
        if (it.key() != peerId && CRYPTO_memcmp(it->key.constData(), key.constData(), 32) == 0)
            return false;
    removePeer(peerId);
    Peer peer;
    peer.key = key;
    peers_.insert(peerId, std::move(peer));
    return true;
}

void NetDatagramChannel::removePeer(quint64 peerId) {
    auto it = peers_.find(peerId);
    if (it == peers_.end()) return;
    eraseKey(it->key);
    peers_.erase(it);
}

void NetDatagramChannel::stop() {
    timer_.stop();
    socket_.close();
    for (auto it = peers_.begin(); it != peers_.end(); ++it) eraseKey(it->key);
    peers_.clear();
    eraseKey(hostPeer_.key);
    hostPeer_ = Peer{};
    mode_ = Mode::Stopped;
    localId_ = 0;
    inboundRate_ = RateWindow{};
    drainQueued_ = false;
    clock_.invalidate();
}

quint16 NetDatagramChannel::localPort() const { return socket_.localPort(); }

bool NetDatagramChannel::write(Peer& peer, quint64 sourceId, quint64 targetId, Kind kind,
                               const QByteArray& payload) {
    if (mode_ == Mode::Stopped || peer.key.size() != 32 || peer.address.isNull() ||
        !peer.port || payload.size() > maxPayloadBytes ||
        peer.nextSequence == std::numeric_limits<quint64>::max() ||
        socket_.bytesToWrite() > kMaxQueuedBytes) return false;
    QByteArray datagram(kHeaderBytes + payload.size() + kTagBytes, Qt::Uninitialized);
    auto* p = reinterpret_cast<uchar*>(datagram.data());
    std::memcpy(p, "KGDU", 4);
    p[4] = kVersion;
    p[5] = mode_ == Mode::Client ? kClientToHost : kHostToClient;
    p[6] = quint8(kind);
    p[7] = 0;
    qToBigEndian<quint64>(sourceId, p + 8);
    qToBigEndian<quint64>(targetId, p + 16);
    qToBigEndian<quint64>(++peer.nextSequence, p + 24);
    qToBigEndian<quint16>(quint16(payload.size()), p + 32);
    if (!payload.isEmpty()) std::memcpy(p + kHeaderBytes, payload.constData(), size_t(payload.size()));
    unsigned int tagSize = 0;
    if (!HMAC(EVP_sha256(), peer.key.constData(), 32, p,
              size_t(kHeaderBytes + payload.size()), p + kHeaderBytes + payload.size(), &tagSize) ||
        tagSize != kTagBytes) return false;
    if (socket_.writeDatagram(datagram, peer.address, peer.port) != datagram.size()) return false;
    peer.lastSentMs = clock_.elapsed();
    return true;
}

bool NetDatagramChannel::send(quint64 peerId, const QByteArray& payload) {
    if (payload.size() > maxPayloadBytes) return false;
    if (mode_ == Mode::Client)
        return peerId != localId_ && hostPeer_.ready &&
               write(hostPeer_, localId_, peerId, Kind::Data, payload);
    if (mode_ != Mode::Host) return false;
    auto it = peers_.find(peerId);
    return it != peers_.end() && it->ready && write(*it, 0, peerId, Kind::Data, payload);
}

void NetDatagramChannel::tick() {
    if (mode_ == Mode::Client) {
        if (!hostPeer_.ready) {
            if (clock_.elapsed() >= 10000) {
                stop();
                emit error(QStringLiteral("Не удалось подключить игровой UDP-канал. Проверьте доступ к UDP-порту хоста."));
                return;
            }
            write(hostPeer_, localId_, 0, Kind::Hello, {});
        } else if (clock_.elapsed() - hostPeer_.lastSentMs >= 2000)
            write(hostPeer_, localId_, 0, Kind::Keepalive, {});
    } else if (mode_ == Mode::Host) {
        const auto ids = peers_.keys();
        for (quint64 id : ids) {
            auto it = peers_.find(id);
            if (it != peers_.end() && it->ready && clock_.elapsed() - it->lastSentMs >= 2000)
                write(*it, 0, id, Kind::Keepalive, {});
        }
    }
}

void NetDatagramChannel::readPending() {
    if (mode_ == Mode::Stopped) return;
    int processed = 0;
    while (socket_.hasPendingDatagrams() && processed++ < kDrainBatch) {
        const qint64 size = socket_.pendingDatagramSize();
        QHostAddress address;
        quint16 port = 0;
        if (size < kHeaderBytes + kTagBytes || size > kMaxDatagramBytes) {
            char discard;
            socket_.readDatagram(&discard, 1, &address, &port);
            continue;
        }
        QByteArray datagram(int(size), Qt::Uninitialized);
        if (socket_.readDatagram(datagram.data(), size, &address, &port) != size) continue;
        receive(datagram, normalized(address), port);
        if (mode_ == Mode::Stopped) return;
    }
    if (socket_.hasPendingDatagrams() && !drainQueued_) {
        drainQueued_ = true;
        QTimer::singleShot(0, this, [this] {
            drainQueued_ = false;
            readPending();
        });
    }
}

void NetDatagramChannel::receive(const QByteArray& datagram, const QHostAddress& address, quint16 port) {
    if (!port || !inboundRate_.accept(clock_.elapsed(), datagram.size(),
                                      kAllPacketsPerSecond, kAllBytesPerSecond)) return;
    const auto* p = reinterpret_cast<const uchar*>(datagram.constData());
    if (std::memcmp(p, "KGDU", 4) || p[4] != kVersion || p[7] != 0) return;
    const quint16 payloadSize = qFromBigEndian<quint16>(p + 32);
    if (payloadSize > maxPayloadBytes || datagram.size() != kHeaderBytes + payloadSize + kTagBytes) return;
    const auto kind = Kind(p[6]);
    const quint64 sourceId = qFromBigEndian<quint64>(p + 8);
    const quint64 targetId = qFromBigEndian<quint64>(p + 16);
    const quint64 sequence = qFromBigEndian<quint64>(p + 24);
    Peer* peer = nullptr;
    if (mode_ == Mode::Host) {
        if (p[5] != kClientToHost || !sourceId) return;
        auto it = peers_.find(sourceId);
        if (it == peers_.end()) return;
        if ((kind == Kind::Hello || kind == Kind::Keepalive) && (targetId || payloadSize)) return;
        if (kind != Kind::Hello && kind != Kind::Keepalive && kind != Kind::Data) return;
        peer = &*it;
    } else if (mode_ == Mode::Client) {
        if (p[5] != kHostToClient || targetId != localId_) return;
        if ((kind == Kind::Ack || kind == Kind::Keepalive) && (sourceId || payloadSize)) return;
        if (kind != Kind::Ack && kind != Kind::Keepalive && kind != Kind::Data) return;
        peer = &hostPeer_;
    } else return;
    unsigned char expected[kTagBytes];
    unsigned int tagSize = 0;
    if (!HMAC(EVP_sha256(), peer->key.constData(), 32, p,
              size_t(kHeaderBytes + payloadSize), expected, &tagSize) || tagSize != kTagBytes ||
        CRYPTO_memcmp(expected, p + kHeaderBytes + payloadSize, kTagBytes) != 0) return;
    if (!peer->rate.accept(clock_.elapsed(), datagram.size(),
                           kPeerPacketsPerSecond, kPeerBytesPerSecond) ||
        !peer->replay.accept(sequence)) return;
    peer->address = address;
    peer->port = port;
    if (mode_ == Mode::Host) {
        if (kind == Kind::Hello) {
            const bool firstReady = !peer->ready;
            peer->ready = true;
            write(*peer, 0, sourceId, Kind::Ack, {});
            if (firstReady) emit ready(sourceId);
            return;
        }
        if (!peer->ready || kind == Kind::Keepalive) return;
        const QByteArray payload = datagram.mid(kHeaderBytes, payloadSize);
        if (!targetId) { emit packet(sourceId, payload); return; }
        auto recipient = peers_.find(targetId);
        if (recipient != peers_.end() && recipient->ready)
            write(*recipient, sourceId, targetId, Kind::Data, payload);
        return;
    }
    if (kind == Kind::Ack) {
        if (!hostPeer_.ready) { hostPeer_.ready = true; emit ready(0); }
        return;
    }
    if (hostPeer_.ready && kind == Kind::Data)
        emit packet(sourceId, datagram.mid(kHeaderBytes, payloadSize));
}

} // namespace kega::net
