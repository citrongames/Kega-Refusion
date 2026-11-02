#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

namespace kega::net {

// Authenticated, unordered datagrams for GGPO. The TCP lobby distributes one
// fresh 32-byte secret per peer; GGPO owns game-level reliability and rollback.
class NetDatagramChannel final : public QObject {
    Q_OBJECT
public:
    explicit NetDatagramChannel(QObject* parent = nullptr);
    ~NetDatagramChannel() override;

    bool host(quint16 port, QString* error = nullptr);
    bool join(const QHostAddress& hostAddress, quint16 port, quint64 localId,
              const QByteArray& key, QString* error = nullptr);
    bool addPeer(quint64 peerId, const QByteArray& key);
    void removePeer(quint64 peerId);
    void stop();
    bool send(quint64 peerId, const QByteArray& payload);
    quint16 localPort() const;
    static constexpr int maxPayloadBytes = 8200;

signals:
    void packet(quint64 sourceId, const QByteArray& payload);
    void ready(quint64 peerId);
    void error(const QString& message);

private:
    struct ReplayWindow {
        quint64 newest = 0;
        quint64 bits = 0;
        bool accept(quint64 sequence);
    };
    struct RateWindow {
        qint64 sinceMs = 0;
        int packets = 0;
        qint64 bytes = 0;
        bool accept(qint64 nowMs, int size, int maxPackets, qint64 maxBytes);
    };
    struct Peer {
        QByteArray key;
        QHostAddress address;
        quint16 port = 0;
        quint64 nextSequence = 0;
        qint64 lastSentMs = 0;
        ReplayWindow replay;
        RateWindow rate;
        bool ready = false;
    };
    enum class Mode { Stopped, Host, Client };
    enum class Kind : quint8 { Hello = 1, Ack = 2, Data = 3, Keepalive = 4 };

    void readPending();
    void tick();
    void receive(const QByteArray& datagram, const QHostAddress& address, quint16 port);
    bool write(Peer& peer, quint64 sourceId, quint64 targetId, Kind kind,
               const QByteArray& payload);
    static QHostAddress normalized(const QHostAddress& address);
    static void eraseKey(QByteArray& key);

    QUdpSocket socket_;
    QTimer timer_;
    QElapsedTimer clock_;
    QHash<quint64, Peer> peers_;
    Peer hostPeer_;
    Mode mode_ = Mode::Stopped;
    quint64 localId_ = 0;
    RateWindow inboundRate_;
    bool drainQueued_ = false;
};

} // namespace kega::net
