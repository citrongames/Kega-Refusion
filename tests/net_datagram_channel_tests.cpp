#include "net/NetDatagramChannel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QtEndian>
#include <openssl/hmac.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

using kega::net::NetDatagramChannel;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool waitFor(const std::function<bool()>& done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(2);
    }
    return done();
}

QByteArray signedClientPacket(quint64 source, quint64 target, quint64 sequence,
                              const QByteArray& payload, const QByteArray& key) {
    QByteArray packet(34 + payload.size() + 32, Qt::Uninitialized);
    auto* p = reinterpret_cast<uchar*>(packet.data());
    std::memcpy(p, "KGDU", 4);
    p[4] = 1; p[5] = 1; p[6] = 3; p[7] = 0;
    qToBigEndian<quint64>(source, p + 8);
    qToBigEndian<quint64>(target, p + 16);
    qToBigEndian<quint64>(sequence, p + 24);
    qToBigEndian<quint16>(quint16(payload.size()), p + 32);
    std::memcpy(p + 34, payload.constData(), size_t(payload.size()));
    unsigned int length = 0;
    require(HMAC(EVP_sha256(), key.constData(), key.size(), p, size_t(34 + payload.size()),
                 p + 34 + payload.size(), &length) && length == 32, "test HMAC failed");
    return packet;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--timeout")) {
            NetDatagramChannel client;
            int errors = 0;
            QString reported;
            QObject::connect(&client, &NetDatagramChannel::error, &app,
                             [&](const QString& message) { ++errors; reported = message; });
            require(client.join(QHostAddress::LocalHost, 1, 11, QByteArray(32, 'T')),
                    "UDP timeout test join failed");
            QElapsedTimer elapsed;
            elapsed.start();
            while (!errors && elapsed.elapsed() < 12000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
                QThread::msleep(2);
            }
            require(errors == 1 && elapsed.elapsed() >= 10000 && client.localPort() == 0 &&
                    reported.contains(QStringLiteral("UDP")),
                    "UDP handshake did not stop once after ten seconds");
            std::puts("UDP handshake timeout passed");
            return 0;
        }
        const QByteArray firstKey(32, 'A'), secondKey(32, 'B');
        NetDatagramChannel host, first, second;
        QString error;
        require(host.host(0, &error), "UDP host bind failed");
        require(host.localPort() != 0, "UDP host has no port");
        require(host.addPeer(11, firstKey) && host.addPeer(22, secondKey), "peer registration failed");
        require(!host.addPeer(33, firstKey), "duplicate secret was accepted");
        int readyCount = 0;
        QObject::connect(&first, &NetDatagramChannel::ready, &app,
                         [&](quint64 peer) { if (peer == 0) ++readyCount; });
        QObject::connect(&second, &NetDatagramChannel::ready, &app,
                         [&](quint64 peer) { if (peer == 0) ++readyCount; });
        require(first.join(QHostAddress::LocalHost, host.localPort(), 11, firstKey, &error),
                "first UDP join failed");
        require(second.join(QHostAddress::LocalHost, host.localPort(), 22, secondKey, &error),
                "second UDP join failed");
        require(waitFor([&] { return readyCount == 2; }), "authenticated hello/ack did not complete");

        QList<QPair<quint64, QByteArray>> hostPackets, firstPackets, secondPackets;
        QObject::connect(&host, &NetDatagramChannel::packet, &app,
                         [&](quint64 id, const QByteArray& data) { hostPackets.append({id, data}); });
        QObject::connect(&first, &NetDatagramChannel::packet, &app,
                         [&](quint64 id, const QByteArray& data) { firstPackets.append({id, data}); });
        QObject::connect(&second, &NetDatagramChannel::packet, &app,
                         [&](quint64 id, const QByteArray& data) { secondPackets.append({id, data}); });
        require(first.send(0, "to-host") && host.send(11, "to-first") &&
                first.send(22, "relay"), "authenticated send failed");
        require(waitFor([&] { return hostPackets.size() == 1 && firstPackets.size() == 1 &&
                                      secondPackets.size() == 1; }), "host or relay delivery failed");
        require(hostPackets[0].first == 11 && hostPackets[0].second == "to-host" &&
                firstPackets[0].first == 0 && firstPackets[0].second == "to-first" &&
                secondPackets[0].first == 11 && secondPackets[0].second == "relay",
                "sender identity or payload changed in relay");
        require(!first.send(0, QByteArray(NetDatagramChannel::maxPayloadBytes + 1, 'X')),
                "oversized payload was accepted");

        QUdpSocket attacker;
        require(attacker.bind(QHostAddress::LocalHost, 0), "spoof socket bind failed");
        QByteArray spoof = signedClientPacket(11, 0, 1000, "spoof", firstKey);
        spoof[spoof.size() - 1] ^= 1;
        attacker.writeDatagram(spoof, QHostAddress::LocalHost, host.localPort());
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        require(hostPackets.size() == 1, "invalid MAC reached host");
        require(host.send(11, "after-spoof") &&
                waitFor([&] { return firstPackets.size() == 2; }),
                "unauthenticated packet changed the peer endpoint");

        const auto newer = signedClientPacket(11, 0, 100, "newer", firstKey);
        const auto older = signedClientPacket(11, 0, 99, "older", firstKey);
        attacker.writeDatagram(newer, QHostAddress::LocalHost, host.localPort());
        attacker.writeDatagram(older, QHostAddress::LocalHost, host.localPort());
        require(waitFor([&] { return hostPackets.size() == 3; }),
                "authenticated out-of-order packets were not accepted");
        require(hostPackets[1].second == "newer" && hostPackets[2].second == "older",
                "reordered packet payload mismatch");
        attacker.writeDatagram(older, QHostAddress::LocalHost, host.localPort());
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        require(hostPackets.size() == 3, "replayed sequence was accepted");
        host.removePeer(11);
        attacker.writeDatagram(signedClientPacket(11, 0, 101, "removed", firstKey),
                               QHostAddress::LocalHost, host.localPort());
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        require(hostPackets.size() == 3, "removed peer remained authorized");

        // Exercise the same dual-stack host socket over IPv6 when available.
        first.stop(); second.stop(); host.stop();
        QUdpSocket ipv6Probe;
        if (ipv6Probe.bind(QHostAddress::LocalHostIPv6, 0)) {
            ipv6Probe.close();
            const QByteArray ipv6Key(32, 'V');
            require(host.host(0, &error) && host.addPeer(44, ipv6Key), "IPv6 host setup failed");
            int ipv6Ready = 0;
            QObject::connect(&first, &NetDatagramChannel::ready, &app,
                             [&](quint64 peer) { if (peer == 0) ++ipv6Ready; });
            require(first.join(QHostAddress::LocalHostIPv6, host.localPort(), 44, ipv6Key, &error),
                    "IPv6 join failed");
            require(waitFor([&] { return ipv6Ready == 1; }), "IPv6 authenticated hello failed");
            require(first.send(0, "ipv6") &&
                    waitFor([&] { return hostPackets.size() == 4; }) &&
                    hostPackets[3].first == 44 && hostPackets[3].second == "ipv6",
                    "IPv6 UDP delivery failed");
        }
        std::puts("authenticated UDP relay, spoof rejection and reordering passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "UDP test failed: %s\n", e.what());
        return 1;
    }
}
