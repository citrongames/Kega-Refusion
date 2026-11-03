#include "../src/core/AchievementsClient.h"

#include <QFile>
#include <QList>
#include <QTemporaryDir>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void require(bool condition, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "achievements_client_tests: %s\n", text);
        std::abort();
    }
}

constexpr char kHash[] = "0123456789ABCDEF0123456789ABCDEF";
constexpr char kLogin[] =
    "{\"Success\":true,\"User\":\"User\",\"Token\":\"ApiToken\",\"Score\":123,"
    "\"SoftcoreScore\":12,\"Messages\":0,\"Permissions\":1,\"AccountType\":\"Registered\"}";
constexpr char kGame[] =
    "{\"Success\":true,\"GameId\":1234,\"Title\":\"Sample Game\",\"ConsoleId\":17,"
    "\"ImageIconUrl\":\"https://retroachievements.org/Images/112233.png\","
    "\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\",\"Sets\":[{"
    "\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":null,\"Type\":\"core\","
    "\"ImageIconUrl\":\"https://retroachievements.org/Images/112233.png\","
    "\"Achievements\":[{\"ID\":5501,\"Title\":\"First\",\"Description\":\"First reward\","
    "\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH0001=3\",\"Author\":\"User\","
    "\"BadgeName\":\"00234\",\"Created\":1367266583,\"Modified\":1376929305}],"
    "\"Leaderboards\":[]}]}";

AchievementHttpRequest take(QList<AchievementHttpRequest>& requests) {
    require(!requests.isEmpty(), "expected mock HTTP request");
    return requests.takeFirst();
}
}

int main() {
    QTemporaryDir data;
    require(data.isValid(), "temporary data directory");
    QList<AchievementHttpRequest> requests;
    QList<AchievementNotice> notices;
    std::array<uint8_t, 64> memory{};
    AchievementsClient::Hooks hooks;
    hooks.readMemory = [&](uint32_t address, uint8_t* out, uint32_t size) {
        if (address > memory.size() || size > memory.size() - address) return uint32_t(0);
        std::memcpy(out, memory.data() + address, size);
        return size;
    };
    hooks.request = [&](const AchievementHttpRequest& request) { requests.append(request); };
    hooks.notice = [&](const AchievementNotice& notice) { notices.append(notice); };
    hooks.changed = [](const AchievementsState&) {};

    {
        AchievementsClient client(data.path(), hooks);
        require(requests.isEmpty(), "constructor did not dispatch HTTP before hooks were wired");
        client.configure(true, false);
        require(requests.isEmpty(), "no anonymous request");
        require(!client.wantsGame(), "signed-out client does not request ROM hashing");
        client.login(QStringLiteral("User"), QStringLiteral("Pa$$word"));
        require(client.wantsGame(), "pending login may request ROM hashing");
        client.loadGame(QString::fromLatin1(kHash), 17);
        require(client.loading(), "load waits for login");
        auto req = take(requests);
        require(req.url.startsWith(QStringLiteral("https://retroachievements.org/")), "login host");
        client.respond(req.id, 200, kLogin);
        require(client.snapshot().loggedIn, "mock login succeeded");
        req = take(requests);
        client.respond(req.id, 200, kGame);
        req = take(requests);
        client.respond(req.id, 200, "{\"Success\":true}");
        client.idle(); // SDK activates game on the worker thread when background reads are disabled.
        require(!client.loading(), "game load completed");
        auto state = client.snapshot();
        require(state.gameId == 1234 && state.gameTitle == QStringLiteral("Sample Game"), "identified game");
        require(state.total == 1 && state.entries.size() == 1, "achievement list");
        require(notices.size() == 1 && notices[0].kind == AchievementNotice::GameReady,
                "only game-ready placard emitted on load");

        client.changeMedia(QStringLiteral("6a2305a2b6675a97ff792709be1ca857"), 17);
        require(client.loading(), "media verification pauses game processing");
        req = take(requests);
        require(req.postData.contains("r=gameid"), "media change resolves hash without new session");
        client.respond(req.id, 200, "{\"Success\":true,\"GameID\":1234}");
        require(!client.loading() && client.snapshot().gameId == 1234,
                "same-game disc keeps current achievement set");
        require(notices.size() == 1, "media change emits no second game placard");

        client.frame(); // Let a false initial condition leave its waiting state.
        memory[1] = 3;
        client.frame();
        require(notices.size() >= 2 && notices[1].kind == AchievementNotice::Unlocked,
                "unlock notification emitted");
        req = take(requests);
        client.respond(req.id, 200,
                       "{\"Success\":true,\"Score\":128,\"SoftcoreScore\":17,"
                       "\"AchievementID\":5501,\"AchievementsRemaining\":0}");
        client.frame();
        require(notices.size() == 3 && notices[2].kind == AchievementNotice::Completed,
                "completion notification emitted after server acknowledgement");
        require(!notices[2].hardcore, "Casual completion is not mastery");
        const QByteArray progress = client.saveProgress();
        require(!progress.isEmpty(), "progress saved with context");
        QByteArray damaged = progress;
        damaged[8] = char(damaged[8] ^ 1);
        client.restoreProgress(damaged);
        require(client.snapshot().unlocked == 1, "invalid progress does not roll back an award");
        QFile account(data.filePath(QStringLiteral("retroachievements-account.ini")));
        require(account.open(QIODevice::ReadOnly), "token persisted");
        require(!account.readAll().contains("Pa$$word"), "password not persisted");

        client.changeMedia(QStringLiteral("4989b063a40dcfa28291ff8d675050e3"), 17);
        req = take(requests);
        client.respond(req.id, 500, "server unavailable");
        require(!client.loading() && client.snapshot().gameId == 0,
                "failed disc verification stops evaluation against old game");
    }
    requests.clear();
    notices.clear();
    {
        AchievementsClient client(data.path(), hooks);
        require(requests.isEmpty(), "constructor still performs no request with saved token");
        client.configure(true, false);
        auto req = take(requests);
        require(req.postData.contains("ApiToken"), "remembered token used after configure");
        client.respond(req.id, 200, kLogin);
        require(client.snapshot().loggedIn, "token login succeeded");
        auto loadFailures = [&] {
            int count = 0;
            for (const auto& notice : notices)
                if (notice.kind == AchievementNotice::LoadFailed) ++count;
            return count;
        };
        client.loadGame(QStringLiteral("11111111111111111111111111111111"), 17);
        req = take(requests);
        client.respond(req.id, 200,
            "{\"Success\":true,\"GameId\":0,\"Title\":\"Unknown Game\","
            "\"ConsoleId\":17,\"ImageIconUrl\":\"https://retroachievements.org/Images/000000.png\",\"Sets\":[]}");
        client.idle();
        require(!client.loading() && client.snapshot().gameId == 0 && loadFailures() == 0,
                "unknown game must not report a load failure");

        client.loadGame(QStringLiteral("22222222222222222222222222222222"), 17);
        req = take(requests);
        client.respond(req.id, 200,
            "{\"Success\":true,\"GameId\":2345,\"Title\":\"Empty Game\",\"ConsoleId\":17,"
            "\"ImageIconUrl\":\"https://retroachievements.org/Images/112233.png\","
            "\"RichPresenceGameId\":2345,\"RichPresencePatch\":\"\",\"Sets\":[{"
            "\"AchievementSetId\":2222,\"GameId\":2345,\"Title\":null,\"Type\":\"core\","
            "\"ImageIconUrl\":\"https://retroachievements.org/Images/112233.png\","
            "\"Achievements\":[],\"Leaderboards\":[]}]}");
        req = take(requests);
        client.respond(req.id, 200, "{\"Success\":true}");
        client.idle();
        require(!client.loading() && client.snapshot().gameId == 2345 &&
                client.snapshot().total == 0 && loadFailures() == 0,
                "recognized game with no achievements must not report a load failure");

        client.loadGame(QStringLiteral("33333333333333333333333333333333"), 17);
        req = take(requests);
        client.respond(req.id, 500, "server unavailable");
        require(!client.loading() && loadFailures() == 1,
                "HTTP game load failure must still be reported");
        client.logout();
        require(!client.wantsGame(), "logout disables ROM hashing");
        require(!QFile::exists(data.filePath(QStringLiteral("retroachievements-account.ini"))),
                "logout removes token file");
    }
    return 0;
}
