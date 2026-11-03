#include "core/EmulationSession.h"
#include "core/CheatSearch.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <cstdio>
#include <functional>
#include <stdexcept>

namespace {
void check(bool okay, const char* reason) {
    if (!okay) throw std::runtime_error(reason);
}
void waitFor(QElapsedTimer& budget, const std::function<bool()>& ready, const char* reason) {
    while (!ready() && budget.elapsed() < 12000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    check(ready(), reason);
}
QJsonObject configuration(const QByteArray& packet) {
    if (packet.size() < 8) return {};
    const auto size = qFromBigEndian<quint32>(packet.constData());
    if (size > 4 * 1024 * 1024 || quint64(size) + 4 > quint64(packet.size())) return {};
    return QJsonDocument::fromJson(packet.mid(4, size)).object()
        .value(QStringLiteral("configuration")).toObject();
}
bool matches(const QJsonObject& config, const CheatList& expected) {
    const auto entries = config.value(QStringLiteral("cheats")).toArray();
    if (entries.size() != expected.size()) return false;
    for (qsizetype i = 0; i < expected.size(); ++i) {
        const auto item = entries[int(i)].toObject();
        if (item.value(QStringLiteral("code")).toString() != expected[i].code ||
            item.value(QStringLiteral("description")).toString() != expected[i].description ||
            item.value(QStringLiteral("enabled")).toBool() != expected[i].enabled) return false;
    }
    return true;
}
CheatList catalog() {
    CheatList entries;
    for (int i = 0; i < 120; ++i)
        entries.append({QStringLiteral("FF%1:0001").arg(i, 4, 16, QLatin1Char('0')).toUpper(),
                        QStringLiteral("Catalog %1").arg(i), false});
    entries.append({QStringLiteral("FF0100:0002+FF0101:0003"),
                    QStringLiteral("Enabled compound"), true});
    return entries;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        check(argc == 2, "usage: cheat_session_tests CORE");
        QTemporaryDir temp;
        check(temp.isValid(), "temporary directory");

        QByteArray rom(32768, char(0));
        qToBigEndian<quint32>(0xffff00, rom.data());
        qToBigEndian<quint32>(0x200, rom.data() + 4);
        rom.replace(0x100, 16, QByteArray("SEGA MEGA DRIVE ", 16));
        rom.replace(0x1f0, 3, "USA");
        rom[0x200] = char(0x60); rom[0x201] = char(0xfe); // BRA.S to self
        const auto path = temp.filePath(QStringLiteral("original.md"));
        QFile file(path);
        check(file.open(QIODevice::WriteOnly) && file.write(rom) == rom.size(),
              "original ROM fixture");
        file.close();

        CheatList enabled = catalog();
        CheatList disabled = enabled;
        disabled.last().enabled = false;
        QByteArray enabledSnapshot, disabledSnapshot;
        {
        EmulationSession session(QString::fromLocal8Bit(argv[1]), temp.path());
        QElapsedTimer budget; budget.start();
        bool loaded = false, closed = false;
        int forwarded = 0, cheatNotices = 0;
        CheatList current;
        QByteArray lastCheatCommand;
        QList<QJsonObject> snapshots;
        QMap<quint64, QJsonObject> searches;
        QStringList errors;
        QObject::connect(&session, &EmulationSession::networkCommandRequested, &app,
                         [&](const QByteArray& command) {
            ++forwarded;
            if (QJsonDocument::fromJson(command).object().value(QStringLiteral("cheats")).isArray())
                lastCheatCommand = command;
            session.applyNetworkCommand(command);
        });
        QObject::connect(&session, &EmulationSession::gameLoaded, &app,
                         [&](const QString&, double, const QString&) { loaded = true; });
        QObject::connect(&session, &EmulationSession::gameClosed, &app,
                         [&] { closed = true; });
        QObject::connect(&session, &EmulationSession::cheatsChanged, &app,
                         [&](const CheatList& entries, int, bool, const QString&) {
            current = entries; ++cheatNotices;
        });
        QObject::connect(&session, &EmulationSession::networkSnapshotReady, &app,
                         [&](const QByteArray& packet) {
            const auto config = configuration(packet);
            snapshots.append(config);
            if (matches(config, enabled)) enabledSnapshot = packet;
            if (matches(config, disabled)) disabledSnapshot = packet;
            session.acknowledgeNetworkSnapshot();
        });
        QObject::connect(&session, &EmulationSession::cheatSearchResults, &app,
                         [&](quint64 id, const QByteArray& bytes) {
            searches.insert(id, QJsonDocument::fromJson(bytes).object());
        });
        QObject::connect(&session, &EmulationSession::errorOccurred, &app,
                         [&](const QString& error) { errors.append(error); });
        QObject::connect(&session, &EmulationSession::networkModeFailed, &app,
                         [&](const QString& error) { errors.append(error); });

        session.setNetworkRole(1);
        session.setNetworkMode(1, QStringLiteral("0123456789abcdef0123456789abcdef"));
        const QJsonArray roster{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("0")},
                        {QStringLiteral("role"), 1}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("1")},
                        {QStringLiteral("role"), 0}}
        };
        session.setNetworkRoster(QJsonDocument(roster).toJson(QJsonDocument::Compact));
        session.loadGame(path);
        waitFor(budget, [&] { return loaded && session.hasGame(); }, "host game load");
        check(errors.isEmpty(), "network mode or game load error");

        session.setCheats(enabled);
        waitFor(budget, [&] { return current == enabled && cheatNotices >= 2; },
                "network cheat command was not applied");
        waitFor(budget, [&] {
            for (const auto& config : snapshots) if (matches(config, enabled)) return true;
            return false;
        }, "snapshot omitted full enabled catalog");
        check(forwarded >= 1 && !lastCheatCommand.isEmpty() &&
              QJsonDocument::fromJson(lastCheatCommand).object()
                  .value(QStringLiteral("cheats")).toArray().size() == enabled.size(),
              "network command omitted catalog");

        session.setCheats(disabled);
        waitFor(budget, [&] { return current == disabled; }, "disabled group not applied");
        waitFor(budget, [&] {
            for (const auto& config : snapshots) if (matches(config, disabled)) return true;
            return false;
        }, "snapshot did not carry disabled group");

        const int beforeReject = snapshots.size();
        QJsonObject malformed = QJsonDocument::fromJson(lastCheatCommand).object();
        malformed.insert(QStringLiteral("cheats"), QStringLiteral("not an array"));
        session.applyNetworkCommand(QJsonDocument(malformed).toJson(QJsonDocument::Compact));
        session.applyNetworkCommand(QByteArray(Cheats::MaxFileBytes + 1, 'x'));
        session.requestNetworkSnapshot();
        waitFor(budget, [&] { return snapshots.size() > beforeReject; },
                "explicit snapshot after malformed commands");
        check(matches(snapshots.last(), disabled) && errors.isEmpty(),
              "malformed or oversized command changed host cheats");

        session.searchCheats(1, 1, Cheats::Search::Unknown, 0, true);
        waitFor(budget, [&] { return searches.contains(1); }, "main RAM search");
        const auto found = searches.value(1);
        check(found.value(QStringLiteral("error")).toString().isEmpty() &&
              found.value(QStringLiteral("base")).toDouble() == 0xff0000 &&
              found.value(QStringLiteral("bigEndian")).toBool() &&
              found.value(QStringLiteral("count")).toDouble() > 256 &&
              found.value(QStringLiteral("results")).toArray().size() == 256,
              "main RAM search metadata or result bound");

        session.closeGame();
        waitFor(budget, [&] { return closed && !session.hasGame(); }, "network game close");
        session.searchCheats(2, 1, Cheats::Search::Changed, 0, false);
        waitFor(budget, [&] { return searches.contains(2); }, "post-close search result");
        const auto after = searches.value(2);
        check(!after.value(QStringLiteral("error")).toString().isEmpty() &&
              after.value(QStringLiteral("count")).toDouble() == 0 &&
              after.value(QStringLiteral("results")).toArray().isEmpty(),
              "game close retained cheat search");
        check(errors.isEmpty() && !enabledSnapshot.isEmpty() && !disabledSnapshot.isEmpty(),
              "host session or captured snapshots");
        session.shutdown();
        QCoreApplication::processEvents();
        }

        // A second core instance is created only after the host worker has stopped.
        const auto clientData = temp.filePath(QStringLiteral("client"));
        check(QDir().mkpath(clientData), "client data directory");
        EmulationSession client(QString::fromLocal8Bit(argv[1]), clientData);
        QElapsedTimer clientBudget; clientBudget.start();
        bool clientLoaded = false;
        int applied = 0;
        CheatList clientCheats;
        QStringList clientErrors;
        QObject::connect(&client, &EmulationSession::gameLoaded, &app,
                         [&](const QString&, double, const QString&) { clientLoaded = true; });
        QObject::connect(&client, &EmulationSession::cheatsChanged, &app,
                         [&](const CheatList& entries, int, bool, const QString&) {
            clientCheats = entries;
        });
        QObject::connect(&client, &EmulationSession::networkSnapshotApplied, &app,
                         [&](bool okay, const QString& error) {
            if (okay) ++applied;
            else clientErrors.append(error);
        });
        QObject::connect(&client, &EmulationSession::networkModeFailed, &app,
                         [&](const QString& error) { clientErrors.append(error); });
        QObject::connect(&client, &EmulationSession::errorOccurred, &app,
                         [&](const QString& error) { clientErrors.append(error); });
        client.setNetworkPeerId(1);
        client.setNetworkRole(0);
        client.setNetworkMode(2, QStringLiteral("0123456789abcdef0123456789abcdef"));
        client.loadGame(path);
        waitFor(clientBudget, [&] { return clientLoaded && client.hasGame(); },
                "client game load");
        check(clientErrors.isEmpty(), "client mode or game load error");
        client.receiveNetworkSnapshot(enabledSnapshot);
        waitFor(clientBudget, [&] { return applied == 1 || !clientErrors.isEmpty(); },
                "enabled snapshot apply");
        check(clientErrors.isEmpty() && clientCheats == enabled,
              "client lost enabled compound or catalog");
        client.receiveNetworkSnapshot(disabledSnapshot);
        waitFor(clientBudget, [&] { return applied == 2 || !clientErrors.isEmpty(); },
                "disabled snapshot apply");
        check(clientErrors.isEmpty() && clientCheats == disabled,
              "client did not apply disabled group");
        client.shutdown();
        std::puts("PASS: host commands, full cheat snapshots, client restore and bounded RAM search");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "FAIL: %s\n", failure.what());
        return 1;
    }
}
