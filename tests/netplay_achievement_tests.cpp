#include "core/EmulationSession.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QUrlQuery>
#include <QtEndian>

#include <atomic>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void waitFor(const std::function<bool()>& condition, const char* message, int timeoutMs = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    require(condition(), message);
}

// Original 68000 fixture. Each VBlank increments CPU RAM byte 2; Up sets CPU
// byte 1 to 3. Genesis Plus GX exposes each 16-bit RAM pair swapped to RA on
// little-endian hosts, so the mock conditions read raw offsets 3 and 0.
QByteArray makeRom() {
    QByteArray rom(64 * 1024, char(0xff));
    qToBigEndian<quint32>(0x00ff0000, rom.data());
    qToBigEndian<quint32>(0x00000200, rom.data() + 4);
    rom.replace(0x100, 16, "SEGA GENESIS    ");
    rom.replace(0x120, 16, "NET ACHIEVEMENTS");
    rom.replace(0x180, 2, "GM");
    qToBigEndian<quint32>(quint32(rom.size() - 1), rom.data() + 0x1a4);
    rom[0x1f0] = 'U';
    int pc = 0x200;
    const auto word = [&](quint16 value) { qToBigEndian(value, rom.data() + pc); pc += 2; };
    const auto writeByte = [&](quint16 value, quint32 address) {
        word(0x13fc); word(value); word(quint16(address >> 16)); word(quint16(address));
    };
    const auto branch = [&](int from, int to) {
        const int displacement = to - from - 2;
        require(displacement >= -128 && displacement <= 127 && displacement != 0,
                "ROM branch displacement");
        rom[from + 1] = char(displacement);
    };
    word(0x46fc); word(0x2700); // Mask interrupts.
    word(0x33fc); word(0x8004); word(0x00c0); word(0x0004);
    word(0x33fc); word(0x8144); word(0x00c0); word(0x0004);
    writeByte(0x40, 0x00a10009); writeByte(0x40, 0x00a10003); // TH high.
    word(0x7000); // MOVEQ #0,D0: VBlank count.
    const int waitOn = pc;
    word(0x3239); word(0x00c0); word(0x0004); // VDP status.
    word(0x0241); word(0x0008);
    const int branchOn = pc; word(0x6700);
    word(0x5200); // ADDQ.B #1,D0.
    word(0x13c0); word(0x00ff); word(0x0002); // Store count.
    word(0x1239); word(0x00a1); word(0x0003); // Read P1 pad.
    word(0x0801); word(0x0000); // BTST #0,D1: Up is active low.
    const int branchClear = pc; word(0x6600);
    writeByte(3, 0x00ff0001);
    const int branchDone = pc; word(0x6000);
    const int clear = pc; writeByte(0, 0x00ff0001);
    const int waitOff = pc;
    word(0x3239); word(0x00c0); word(0x0004);
    word(0x0241); word(0x0008);
    const int branchOff = pc; word(0x6600);
    const int repeat = pc; word(0x6000);
    branch(branchOn, waitOn); branch(branchClear, clear); branch(branchDone, waitOff);
    branch(branchOff, waitOff); branch(repeat, waitOn);
    return rom;
}

constexpr char gameReply[] =
    "{\"Success\":true,\"GameId\":1234,\"Title\":\"Original Net Fixture\",\"ConsoleId\":1,"
    "\"ImageIconUrl\":\"https://retroachievements.org/Images/112233.png\","
    "\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\",\"Sets\":[{"
    "\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":null,\"Type\":\"core\","
    "\"ImageIconUrl\":\"https://retroachievements.org/Images/112233.png\",\"Achievements\":["
    "{\"ID\":5501,\"Title\":\"Up\",\"Description\":\"Press Up\",\"Flags\":3,\"Points\":5,"
    "\"MemAddr\":\"0xH0000=3\",\"Author\":\"Fixture\",\"BadgeName\":\"00234\","
    "\"Created\":1367266583,\"Modified\":1376929305},"
    "{\"ID\":5502,\"Title\":\"Time\",\"Description\":\"Wait\",\"Flags\":3,\"Points\":5,"
    "\"MemAddr\":\"0xH0003=180\",\"Author\":\"Fixture\",\"BadgeName\":\"00235\","
    "\"Created\":1367266583,\"Modified\":1376929305}],"
    "\"Leaderboards\":[]}]}";
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        require(argc == 2, "core argument required");
        QTemporaryDir temp;
        require(temp.isValid(), "temporary data directory");
        const QString path = temp.filePath("original.md");
        QFile romFile(path);
        const auto rom = makeRom();
        require(romFile.open(QIODevice::WriteOnly) && romFile.write(rom) == rom.size(),
                "original ROM fixture write");
        romFile.close();

        EmulationSession session(QString::fromLocal8Bit(argv[1]), temp.path());
        std::atomic<int> frames{0};
        int snapshots = 0, ready = 0, errors = 0, unlocked = 0;
        bool loaded = false;
        bool hardcore = false, unexpectedRequest = false;
        QList<int> awards;
        QStringList operations;
        AchievementsState state;
        QObject::connect(&session, &EmulationSession::videoFrame, &app,
                         [&](const QImage&, double) { ++frames; }, Qt::DirectConnection);
        QObject::connect(&session, &EmulationSession::gameLoaded, &app,
                         [&](const QString&, double, const QString&) { loaded = true; });
        QObject::connect(&session, &EmulationSession::networkSnapshotReady, &app,
                         [&](const QByteArray&) { ++snapshots; session.acknowledgeNetworkSnapshot(); });
        QObject::connect(&session, &EmulationSession::networkModeFailed, &app,
                         [&](const QString& error) { ++errors; std::fprintf(stderr, "network: %s\n", qPrintable(error)); });
        QObject::connect(&session, &EmulationSession::errorOccurred, &app,
                         [&](const QString& error) { ++errors; std::fprintf(stderr, "core: %s\n", qPrintable(error)); });
        QObject::connect(&session, &EmulationSession::achievementsChanged, &app,
                         [&](const AchievementsState& value) { state = value; });
        QObject::connect(&session, &EmulationSession::achievementsHardcoreChanged, &app,
                         [&](bool value) { hardcore |= value; });
        QObject::connect(&session, &EmulationSession::achievementNotice, &app,
                         [&](const AchievementNotice& notice) {
            if (notice.kind == AchievementNotice::GameReady) ++ready;
            if (notice.kind == AchievementNotice::Unlocked) ++unlocked;
        });
        // Every RA request terminates here. This test never contacts a service or account.
        QObject::connect(&session, &EmulationSession::achievementHttpRequest, &app,
                         [&](const AchievementHttpRequest& request) {
            const QUrlQuery query(QString::fromUtf8(request.postData));
            const QString operation = query.queryItemValue("r");
            operations.append(operation);
            QByteArray reply;
            if (operation == "login2")
                reply = R"({"Success":true,"User":"Fixture","Token":"FixtureToken","Score":0,"SoftcoreScore":0,"Messages":0,"Permissions":1,"AccountType":"Registered"})";
            else if (operation == "achievementsets") reply = gameReply;
            else if (operation == "startsession" || operation == "ping") reply = R"({"Success":true})";
            else if (operation == "awardachievement") {
                const int id = query.queryItemValue("a").toInt();
                awards.append(id);
                reply = QByteArray("{\"Success\":true,\"Score\":10,\"SoftcoreScore\":10,\"AchievementID\":") +
                        QByteArray::number(id) + ",\"AchievementsRemaining\":0}";
            } else {
                unexpectedRequest = true;
                std::fprintf(stderr, "unexpected RA request: %s\n", qPrintable(operation));
                reply = R"({"Success":false,"Error":"Unexpected fixture request"})";
            }
            session.achievementResponse(request.id, 200, reply);
        });

        // Load the generated ROM before entering the lobby. Network commands
        // are routed through the controller once the host session is active.
        session.loadGame(path);
        waitFor([&] { return loaded && frames.load() >= 3; }, "original ROM did not start");
        session.setNetworkRole(0);
        session.setNetworkMode(1, QStringLiteral("0123456789abcdef0123456789abcdef"));
        session.configureAchievements(true, true); // Unapproved Hardcore stays Softcore.
        session.loginAchievements(QStringLiteral("Fixture"), QStringLiteral("FixturePassword"));
        {
            QElapsedTimer timer;
            timer.start();
            while (!(ready == 1 && state.gameId == 1234 && snapshots >= 1) && timer.elapsed() < 8000) {
                QCoreApplication::processEvents();
                QThread::msleep(2);
            }
            if (!(ready == 1 && state.gameId == 1234 && snapshots >= 1))
                std::fprintf(stderr,
                    "ready diagnostic: ready=%d game=%u snapshots=%d frames=%d loaded=%d "
                    "enabled=%d loggedIn=%d loading=%d active=%d status=%s operations=%s\n",
                    ready, state.gameId, snapshots, frames.load(), loaded, state.enabled,
                    state.loggedIn, state.loading, session.networkActive(), qPrintable(state.status),
                    qPrintable(operations.join(',')));
            require(ready == 1 && state.gameId == 1234 && snapshots >= 1,
                    "spectator game and achievements did not become ready");
        }
        require(state.enabled && state.loggedIn && !state.hardcore && !hardcore,
                "network Hardcore approval gate failed");
        const int spectatorStart = frames.load();
        session.setButtons(0, 0x10); // RETRO_DEVICE_ID_JOYPAD_UP.
        waitFor([&] { return frames.load() >= spectatorStart + 190; },
                "spectator frames did not advance");
        require(awards.isEmpty() && unlocked == 0,
                "spectator evaluated or submitted a local achievement");

        session.setButtons(0, 0);
        session.setNetworkRole(1);
        session.setNetworkRoster(QJsonDocument(QJsonArray{QJsonObject{{"id", "0"}, {"role", 1}}})
                                     .toJson(QJsonDocument::Compact));
        waitFor([&] { return snapshots >= 2; }, "host role change did not start a new GGPO epoch");
        const int playerStart = frames.load();
        waitFor([&] { return frames.load() >= playerStart + 5; },
                "host role did not advance the false input condition");
        require(awards.isEmpty() && unlocked == 0 && !state.hardcore && !hardcore,
                "spectator achievement leaked into the new host role");
        session.setButtons(0, 0x10);
        waitFor([&] { return awards.contains(5501); },
                "confirmed Up input did not unlock its achievement");
        session.setButtons(0, 0);
        const int afterAwards = frames.load();
        waitFor([&] { return frames.load() >= afterAwards + 20; },
                "host did not keep advancing after awards");
        require(awards.size() == 1 && unlocked == 1 && awards.first() == 5501 &&
                    !unexpectedRequest && errors == 0,
                "confirmed host achievements were not submitted exactly once");
        session.logoutAchievements();
        waitFor([&] { return !state.loggedIn; }, "logout did not finish");
        const int signedOutFrame=frames.load();
        waitFor([&] { return frames.load()>=signedOutFrame+20; }, "signed-out network game stalled");
        require(awards.size()==1 && errors==0, "signed-out player produced awards or errors");
        session.shutdown();
        std::puts("Network achievements: spectator suppression, confirmed host Softcore award, Hardcore downgrade, no duplicate award passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "netplay_achievement_tests: %s\n", error.what());
        return 1;
    }
}
