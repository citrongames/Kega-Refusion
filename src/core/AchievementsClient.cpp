#include "i18n/Localization.h"
#include "AchievementsClient.h"

#include <rc_client.h>
#include <rc_api_request.h>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDevice>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QtEndian>

#include <cstring>
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

namespace {
constexpr qsizetype kMaxCredentialFile = 64 * 1024;
constexpr qsizetype kMaxHttpBody = 8 * 1024 * 1024;
constexpr size_t kMaxPending = 32;
constexpr size_t kMaxProgressBlob = 2 * 1024 * 1024;
constexpr char kProgressMagic[] = "KRACT001";
constexpr qsizetype kProgressHeader = 8 + 32 + 32 + 4;

QString fromUtf8(const char* value) { return value ? QString::fromUtf8(value) : QString(); }
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }

bool isHash(const QByteArray& hash) {
    if (hash.size() != 32) return false;
    for (const char c : hash) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool validRaUrl(const QString& string) {
    const QUrl url(string);
    return url.isValid() && url.scheme() == QStringLiteral("https") &&
           url.host().compare(QStringLiteral("retroachievements.org"), Qt::CaseInsensitive) == 0 &&
           url.userInfo().isEmpty();
}

// Ping's optional "m" field carries Rich Presence. Keep the session heartbeat,
// game ID and hash, while omitting gameplay-derived presence text.
void stripPingPresence(QByteArray& form) {
    const QList<QByteArray> fields = form.split('&');
    if (!fields.contains("r=ping")) return;
    QByteArray filtered;
    for (const QByteArray& field : fields) {
        if (field.startsWith("m=")) continue;
        if (!filtered.isEmpty()) filtered += '&';
        filtered += field;
    }
    form = std::move(filtered);
}
}

struct AchievementsClient::Impl {
    struct Pending {
        rc_client_server_callback_t callback = nullptr;
        void* userdata = nullptr;
    };

    QString dataPath;
    Hooks hooks;
    rc_client_t* client = nullptr;
    AchievementsState state;
    std::unordered_map<quint64, Pending> pending;
    quint64 nextRequest = 1;
    rc_client_async_handle_t* loginHandle = nullptr;
    rc_client_async_handle_t* loadHandle = nullptr;
    rc_client_async_handle_t* mediaHandle = nullptr;
    QByteArray gameHash;
    QByteArray gameDigest;
    QByteArray userDigest;
    QByteArray wantedHash;
    QByteArray mediaHash;
    unsigned wantedConsole = 0;
    bool loginWithToken = false;
    bool shuttingDown = false;
    QElapsedTimer loadTimer;

    Impl(QString path, Hooks supplied) : dataPath(std::move(path)), hooks(std::move(supplied)) {
        state.enabled = false;
        state.status = Localization::text(QStringLiteral("Достижения выключены"));
        client = rc_client_create(&readMemory, &serverCall);
        if (!client) {
            state.status = Localization::text(QStringLiteral("Не удалось создать клиент достижений"));
            return;
        }
        rc_client_set_userdata(client, this);
        rc_client_set_event_handler(client, &event);
        rc_client_set_allow_background_memory_reads(client, 0);
        rc_client_set_hardcore_enabled(client, 0);
    }

    ~Impl() {
        shuttingDown = true;
        if (client) {
            if (loginHandle) rc_client_abort_async(client, loginHandle);
            if (loadHandle) rc_client_abort_async(client, loadHandle);
            if (mediaHandle) rc_client_abort_async(client, mediaHandle);
            // Complete SDK-owned callbacks while the SDK and wrapper are still alive.
            cancelPending();
            rc_client_destroy(client);
        }
    }

    static Impl* self(rc_client_t* client) {
        return static_cast<Impl*>(rc_client_get_userdata(client));
    }

    static uint32_t readMemory(uint32_t address, uint8_t* buffer, uint32_t size, rc_client_t* client) {
        Impl* impl = self(client);
        if (!impl || !impl->hooks.readMemory || !impl->state.enabled) return 0;
        try { return impl->hooks.readMemory(address, buffer, size); }
        catch (...) { return 0; }
    }

    static void serverCall(const rc_api_request_t* request, rc_client_server_callback_t callback,
                           void* userdata, rc_client_t* client) {
        Impl* impl = self(client);
        if (!impl || !callback) return;
        rc_api_server_response_t failed{};
        failed.body = "Request unavailable";
        failed.body_length = std::strlen(failed.body);
        failed.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
        if (impl->shuttingDown || !impl->state.enabled || !impl->hooks.request ||
            impl->pending.size() >= kMaxPending || !request || !request->url) {
            callback(&failed, userdata);
            return;
        }
        AchievementHttpRequest outgoing;
        outgoing.url = fromUtf8(request->url);
        if (!validRaUrl(outgoing.url)) {
            callback(&failed, userdata);
            impl->state.status = Localization::text(QStringLiteral("Недопустимый адрес сервера достижений"));
            impl->publish();
            return;
        }
        if (request->post_data) outgoing.postData = QByteArray(request->post_data);
        stripPingPresence(outgoing.postData);
        if (outgoing.postData.size() > kMaxHttpBody) {
            callback(&failed, userdata);
            return;
        }
        outgoing.contentType = fromUtf8(request->content_type);
        do { outgoing.id = impl->nextRequest++; } while (!outgoing.id || impl->pending.count(outgoing.id));
        impl->pending.emplace(outgoing.id, Pending{callback, userdata});
        try { impl->hooks.request(outgoing); }
        catch (...) {
            auto it = impl->pending.find(outgoing.id);
            if (it != impl->pending.end()) {
                auto pending = it->second;
                impl->pending.erase(it);
                pending.callback(&failed, pending.userdata);
            }
        }
    }

    static void loginComplete(int result, const char* error, rc_client_t* client, void* userdata) {
        auto* impl = static_cast<Impl*>(userdata);
        if (!impl || impl->shuttingDown) return;
        impl->loginHandle = nullptr;
        if (result != RC_OK) {
            impl->state.loggedIn = false;
            impl->state.loading = false;
            impl->state.status = Localization::text(QStringLiteral("Вход в RetroAchievements не выполнен: %1"))
                                     .arg(fromUtf8(error));
            if (impl->loginWithToken && (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN))
                impl->removeCredentials();
            impl->publish();
            if (!impl->wantedHash.isEmpty()) impl->notifyLoadFailure();
            return;
        }
        const rc_client_user_t* user = rc_client_get_user_info(client);
        impl->state.loggedIn = user != nullptr;
        impl->state.username = user ? fromUtf8(user->username) : QString();
        impl->userDigest = digest(impl->state.username.toUtf8());
        impl->state.status = Localization::text(QStringLiteral("RetroAchievements: вход выполнен"));
        if (user && user->username && user->token && *user->token &&
            !impl->saveCredentials(QByteArray(user->username), QByteArray(user->token))) {
            impl->state.status = Localization::text(QStringLiteral("Вход выполнен, но токен не сохранён"));
        }
        impl->publish();
        if (!impl->wantedHash.isEmpty()) impl->startGameLoad();
    }

    static void gameComplete(int result, const char* error, rc_client_t* client, void* userdata) {
        auto* impl = static_cast<Impl*>(userdata);
        if (!impl || impl->shuttingDown) return;
        impl->loadHandle = nullptr;
        impl->state.loading = false;
        if (result != RC_OK) {
            impl->state.status = Localization::text(QStringLiteral("Достижения для игры недоступны: %1")).arg(fromUtf8(error));
            impl->gameHash.clear();
            impl->gameDigest.clear();
            impl->publish();
            impl->notifyLoadFailure();
            return;
        }
        const rc_client_game_t* game = rc_client_get_game_info(client);
        if (!game || !game->id || (impl->wantedConsole && game->console_id != impl->wantedConsole)) {
            rc_client_unload_game(client);
            impl->gameHash.clear();
            impl->gameDigest.clear();
            impl->state.status = Localization::text(QStringLiteral("Игра RetroAchievements не совпадает с запущенной системой"));
            impl->publish();
            impl->notifyLoadFailure();
            return;
        }
        impl->gameHash = impl->wantedHash;
        impl->gameDigest = digest(impl->gameHash);
        impl->state.status = Localization::text(QStringLiteral("Достижения загружены"));
        impl->rebuild();
        AchievementNotice notice;
        notice.kind = AchievementNotice::GameReady;
        notice.title = impl->state.gameTitle;
        notice.badgeUrl = fromUtf8(game->badge_url);
        notice.description = Localization::text(QStringLiteral("Открыто %1 из %2")).arg(impl->state.unlocked).arg(impl->state.total);
        impl->sendNotice(notice);
    }

    static void mediaComplete(int result, const char* error, rc_client_t* client, void* userdata) {
        auto* impl = static_cast<Impl*>(userdata);
        if (!impl || impl->shuttingDown) return;
        impl->mediaHandle = nullptr;
        impl->state.loading = false;
        if (result == RC_OK || result == RC_HARDCORE_DISABLED) {
            impl->gameHash = impl->mediaHash;
            impl->gameDigest = digest(impl->gameHash);
            impl->wantedHash = impl->gameHash;
            impl->state.status = result == RC_OK
                                     ? Localization::text(QStringLiteral("Диск проверен RetroAchievements"))
                                     : Localization::text(QStringLiteral("RetroAchievements отключил Hardcore для этого диска"));
        } else {
            // The core has already switched media. Evaluating the old game's conditions
            // against an unverified disc could award the wrong achievement.
            rc_client_unload_game(client);
            impl->gameHash.clear();
            impl->gameDigest.clear();
            impl->wantedHash.clear();
            impl->state.gameId = impl->state.total = impl->state.unlocked = impl->state.points = 0;
            impl->state.gameTitle.clear();
            impl->state.entries.clear();
            impl->state.status = Localization::text(QStringLiteral("Достижения отключены после смены диска: %1"))
                                     .arg(fromUtf8(error));
        }
        impl->mediaHash.clear();
        impl->publish();
        if (result != RC_OK && result != RC_HARDCORE_DISABLED) impl->notifyLoadFailure();
    }

    static void event(const rc_client_event_t* event, rc_client_t* client) {
        Impl* impl = self(client);
        if (!impl || !event || impl->shuttingDown) return;
        switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
            if (!event->achievement) break;
            AchievementNotice notice;
            notice.kind = AchievementNotice::Unlocked;
            notice.title = fromUtf8(event->achievement->title);
            notice.description = fromUtf8(event->achievement->description);
            notice.badgeUrl = fromUtf8(event->achievement->badge_url);
            notice.hardcore = impl->hardcore();
            notice.points = event->achievement->points;
            notice.rarity = notice.hardcore ? event->achievement->rarity_hardcore : event->achievement->rarity;
            impl->sendNotice(notice);
            impl->rebuild();
            break;
        }
        case RC_CLIENT_EVENT_GAME_COMPLETED: {
            AchievementNotice notice;
            notice.kind = AchievementNotice::Completed;
            if (const auto* game = rc_client_get_game_info(client))
                notice.badgeUrl = fromUtf8(game->badge_url);
            notice.title = impl->state.gameTitle;
            notice.description = impl->hardcore() ? Localization::text(QStringLiteral("Мастерство достигнуто"))
                                                  : Localization::text(QStringLiteral("Все достижения открыты"));
            notice.hardcore = impl->hardcore();
            impl->sendNotice(notice);
            impl->rebuild();
            break;
        }
        case RC_CLIENT_EVENT_DISCONNECTED:
            impl->state.disconnected = true;
            impl->state.status = Localization::text(QStringLiteral("Нет соединения с RetroAchievements; отправка ожидает повтора"));
            impl->publish();
            break;
        case RC_CLIENT_EVENT_RECONNECTED:
            impl->state.disconnected = false;
            impl->state.status = Localization::text(QStringLiteral("Соединение с RetroAchievements восстановлено"));
            impl->publish();
            break;
        case RC_CLIENT_EVENT_SERVER_ERROR:
            impl->state.status = Localization::text(QStringLiteral("Ошибка RetroAchievements: %1"))
                                     .arg(event->server_error ? fromUtf8(event->server_error->error_message) : QString());
            impl->publish();
            break;
        case RC_CLIENT_EVENT_RESET:
            if (impl->hooks.resetRequested) impl->hooks.resetRequested();
            break;
        default:
            break;
        }
    }

    void notifyLoadFailure() {
        AchievementNotice notice;
        notice.kind = AchievementNotice::LoadFailed;
        notice.title = Localization::text(QStringLiteral("Не удалось загрузить достижения. Игра продолжается."));
        sendNotice(notice);
    }

    void sendNotice(const AchievementNotice& value) {
        if (hooks.notice) hooks.notice(value);
    }

    void publish() {
        state.hardcore = hardcore();
        if (hooks.changed) hooks.changed(state);
    }

    bool hardcore() const { return client && state.enabled && rc_client_get_hardcore_enabled(client); }

    QString credentialPath() const { return QDir(dataPath).filePath(QStringLiteral("retroachievements-account.ini")); }

    bool saveCredentials(const QByteArray& user, const QByteArray& token) {
        if (user.isEmpty() || token.isEmpty() || user.size() > 256 || token.size() > 4096) return false;
        if (!QDir().mkpath(dataPath)) return false;
        QSaveFile file(credentialPath());
        if (!file.open(QIODevice::WriteOnly)) return false;
        if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            file.cancelWriting();
            return false;
        }
        QByteArray body = "[Account]\nusername64=" + user.toBase64() + "\ntoken64=" + token.toBase64() + '\n';
        if (body.size() > kMaxCredentialFile || file.write(body) != body.size()) {
            file.cancelWriting();
            return false;
        }
        return file.commit();
    }

    bool readCredentials(QByteArray& user, QByteArray& token) const {
        QFile file(credentialPath());
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > kMaxCredentialFile) return false;
        const QByteArray bytes = file.readAll();
        if (bytes.size() != file.size() || !bytes.startsWith("[Account]\n")) return false;
        for (const QByteArray& line : bytes.split('\n')) {
            if (line.startsWith("username64=")) user = QByteArray::fromBase64(line.mid(11));
            else if (line.startsWith("token64=")) token = QByteArray::fromBase64(line.mid(8));
        }
        return !user.isEmpty() && !token.isEmpty() && user.size() <= 256 && token.size() <= 4096;
    }

    void removeCredentials() { QFile::remove(credentialPath()); }

    void cancelPending() {
        // Abort any user-facing async operations before draining their network callbacks.
        auto callbacks = std::move(pending);
        pending.clear();
        rc_api_server_response_t failed{};
        failed.body = "Request cancelled";
        failed.body_length = std::strlen(failed.body);
        failed.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
        for (const auto& item : callbacks) {
            if (item.second.callback) item.second.callback(&failed, item.second.userdata);
        }
        // Callbacks might start new requests; refuse them during explicit cancellation.
    }

    void checkTimeout() {
        if ((!state.loading && !loginHandle) || !loadTimer.isValid() ||
            loadTimer.elapsed() < 15000) return;
        shuttingDown = true;
        const bool loginTimedOut = loginHandle != nullptr;
        const bool gameRequested = !wantedHash.isEmpty() || !mediaHash.isEmpty();
        if (loginHandle) rc_client_abort_async(client, loginHandle);
        if (loadHandle) rc_client_abort_async(client, loadHandle);
        if (mediaHandle) rc_client_abort_async(client, mediaHandle);
        cancelPending();
        if (loadHandle || mediaHandle) rc_client_unload_game(client);
        if (loginTimedOut) {
            rc_client_logout(client);
            state.loggedIn = false;
            state.username.clear();
            userDigest.clear();
        }
        if (loadHandle || mediaHandle) {
            gameHash.clear();
            gameDigest.clear();
            state.gameId = state.total = state.unlocked = state.points = 0;
            state.gameTitle.clear();
            state.entries.clear();
        }
        loginHandle = loadHandle = mediaHandle = nullptr;
        wantedHash.clear();
        mediaHash.clear();
        state.loading = false;
        state.status = Localization::text(QStringLiteral("Время ожидания RetroAchievements истекло"));
        shuttingDown = false;
        publish();
        if (gameRequested) notifyLoadFailure();
    }

    void startGameLoad() {
        if (!client || !state.enabled || !state.loggedIn || wantedHash.isEmpty()) return;
        state.loading = true;
        loadTimer.start();
        state.status = Localization::text(QStringLiteral("Загрузка достижений…"));
        publish();
        loadHandle = rc_client_begin_load_game(client, wantedHash.constData(), &gameComplete, this);
    }

    void rebuild() {
        state.entries.clear();
        const rc_client_game_t* game = client ? rc_client_get_game_info(client) : nullptr;
        state.gameId = game ? game->id : 0;
        state.gameTitle = game ? fromUtf8(game->title) : QString();
        state.total = state.unlocked = state.points = 0;
        if (!game || !game->id) { publish(); return; }
        rc_client_user_game_summary_t summary{};
        rc_client_get_user_game_summary(client, &summary);
        state.total = summary.num_core_achievements;
        state.unlocked = summary.num_unlocked_achievements;
        state.points = summary.points_unlocked;
        rc_client_achievement_list_t* list = rc_client_create_achievement_list(
            client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
        if (list) {
            QSet<quint32> seen;
            for (uint32_t b = 0; b < list->num_buckets; ++b) {
                const auto& bucket = list->buckets[b];
                for (uint32_t a = 0; a < bucket.num_achievements; ++a) {
                    const rc_client_achievement_t* entry = bucket.achievements[a];
                    if (!entry || seen.contains(entry->id)) continue;
                    seen.insert(entry->id);
                    AchievementEntry row;
                    row.id = entry->id;
                    row.points = entry->points;
                    row.title = fromUtf8(entry->title);
                    row.description = fromUtf8(entry->description);
                    row.badgeUrl = fromUtf8(entry->badge_url);
                    row.lockedBadgeUrl = fromUtf8(entry->badge_locked_url);
                    row.progress = fromUtf8(entry->measured_progress);
                    row.rarity = entry->rarity;
                    row.hardcoreRarity = entry->rarity_hardcore;
                    row.softcoreUnlocked = entry->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE;
                    row.hardcoreUnlocked = entry->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE;
                    row.unsupported = entry->state == RC_CLIENT_ACHIEVEMENT_STATE_DISABLED ||
                                      entry->bucket == RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED;
                    row.type = entry->type;
                    state.entries.append(std::move(row));
                }
            }
            rc_client_destroy_achievement_list(list);
            // Lock-state buckets must not move rows when an achievement unlocks.
            std::sort(state.entries.begin(), state.entries.end(),
                [](const AchievementEntry& a, const AchievementEntry& b) { return a.id < b.id; });
        }
        publish();
    }
};

AchievementsClient::AchievementsClient(QString dataPath, Hooks hooks)
    : impl_(std::make_unique<Impl>(std::move(dataPath), std::move(hooks))) {}
AchievementsClient::~AchievementsClient() = default;

void AchievementsClient::configure(bool enabled, bool hardcore) {
    auto& p = *impl_;
    if (!p.client) { p.publish(); return; }
    const bool wasEnabled = p.state.enabled;
    if (!enabled && wasEnabled) {
        p.shuttingDown = true;
        if (p.loginHandle) rc_client_abort_async(p.client, p.loginHandle);
        if (p.loadHandle) rc_client_abort_async(p.client, p.loadHandle);
        if (p.mediaHandle) rc_client_abort_async(p.client, p.mediaHandle);
        p.cancelPending();
        rc_client_unload_game(p.client);
        rc_client_logout(p.client);
        p.loginHandle = p.loadHandle = p.mediaHandle = nullptr;
        p.wantedHash.clear();
        p.mediaHash.clear();
        p.gameHash.clear();
        p.gameDigest.clear();
        p.userDigest.clear();
        p.state = AchievementsState{};
        p.state.enabled = false;
        p.state.status = Localization::text(QStringLiteral("Достижения выключены"));
        p.shuttingDown = false;
        p.publish();
        return;
    }
    p.state.enabled = enabled;
    if (!enabled) { p.publish(); return; }
    if (!wasEnabled) {
        rc_client_set_hardcore_enabled(p.client, hardcore ? 1 : 0);
        p.state.status = Localization::text(QStringLiteral("Войдите в RetroAchievements"));
        p.publish();
        QByteArray user, token;
        if (p.readCredentials(user, token)) {
            p.loginWithToken = true;
            p.loadTimer.start();
            p.state.status = Localization::text(QStringLiteral("Вход в RetroAchievements…"));
            p.publish();
            p.loginHandle = rc_client_begin_login_with_token(p.client, user.constData(), token.constData(),
                                                              &Impl::loginComplete, &p);
        }
    } else {
        rc_client_set_hardcore_enabled(p.client, hardcore ? 1 : 0);
        p.publish();
    }
}

void AchievementsClient::login(QString user, QString password) {
    auto& p = *impl_;
    if (!p.client || !p.state.enabled) return;
    if (p.loginHandle || p.state.loggedIn || p.loadHandle || p.mediaHandle) {
        p.shuttingDown = true;
        if (p.loginHandle) rc_client_abort_async(p.client, p.loginHandle);
        if (p.loadHandle) rc_client_abort_async(p.client, p.loadHandle);
        if (p.mediaHandle) rc_client_abort_async(p.client, p.mediaHandle);
        p.cancelPending();
        if (p.gameHash.isEmpty()) p.gameHash = p.wantedHash;
        if (!p.gameHash.isEmpty()) p.wantedHash = p.gameHash;
        rc_client_unload_game(p.client);
        rc_client_logout(p.client);
        p.loginHandle = nullptr;
        p.loadHandle = nullptr;
        p.mediaHandle = nullptr;
        p.mediaHash.clear();
        p.state.loggedIn = false;
        p.state.username.clear();
        p.userDigest.clear();
        p.state.gameId = p.state.total = p.state.unlocked = p.state.points = 0;
        p.state.gameTitle.clear();
        p.state.entries.clear();
        p.gameHash.clear();
        p.gameDigest.clear();
        p.shuttingDown = false;
    }
    p.removeCredentials();
    p.loginWithToken = false;
    p.loadTimer.start();
    p.state.status = Localization::text(QStringLiteral("Вход в RetroAchievements…"));
    p.publish();
    const QByteArray u = user.toUtf8(), secret = password.toUtf8();
    p.loginHandle = rc_client_begin_login_with_password(p.client, u.constData(), secret.constData(),
                                                         &Impl::loginComplete, &p);
}

void AchievementsClient::logout() {
    auto& p = *impl_;
    const bool wasEnabled = p.state.enabled;
    p.removeCredentials();
    if (!p.client) return;
    p.shuttingDown = true;
    if (p.loginHandle) rc_client_abort_async(p.client, p.loginHandle);
    if (p.loadHandle) rc_client_abort_async(p.client, p.loadHandle);
    if (p.mediaHandle) rc_client_abort_async(p.client, p.mediaHandle);
    p.cancelPending();
    rc_client_unload_game(p.client);
    rc_client_logout(p.client);
    p.loginHandle = p.loadHandle = p.mediaHandle = nullptr;
    p.wantedHash.clear();
    p.mediaHash.clear();
    p.gameHash.clear();
    p.gameDigest.clear();
    p.userDigest.clear();
    p.state = AchievementsState{};
    p.state.enabled = wasEnabled;
    p.state.status = Localization::text(QStringLiteral("Выход из RetroAchievements выполнен"));
    p.shuttingDown = false;
    p.publish();
}

void AchievementsClient::loadGame(QString hash, unsigned consoleId) {
    auto& p = *impl_;
    if (!p.client || !p.state.enabled) return;
    QByteArray bytes = hash.toLatin1();
    if (!isHash(bytes)) {
        unloadGame();
        p.state.status = Localization::text(QStringLiteral("Неверный хеш игры RetroAchievements"));
        p.publish();
        return;
    }
    if (p.loadHandle) rc_client_abort_async(p.client, p.loadHandle);
    if (p.mediaHandle) rc_client_abort_async(p.client, p.mediaHandle);
    p.mediaHandle = nullptr;
    p.mediaHash.clear();
    rc_client_unload_game(p.client);
    p.gameHash.clear();
    p.gameDigest.clear();
    p.wantedHash = std::move(bytes);
    p.wantedConsole = consoleId;
    p.loadTimer.start();
    p.state.gameId = p.state.total = p.state.unlocked = p.state.points = 0;
    p.state.gameTitle.clear();
    p.state.entries.clear();
    if (p.state.loggedIn) p.startGameLoad();
    else {
        p.state.loading = p.loginHandle != nullptr;
        p.state.status = p.state.loading ? Localization::text(QStringLiteral("Ожидание входа в RetroAchievements…"))
                                         : Localization::text(QStringLiteral("Для достижений нужен вход в RetroAchievements"));
        p.publish();
    }
}

void AchievementsClient::changeMedia(QString hash, unsigned consoleId) {
    auto& p = *impl_;
    if (!p.client || !p.state.enabled || !p.state.loggedIn) return;
    const QByteArray bytes = hash.toLatin1();
    const rc_client_game_t* game = rc_client_get_game_info(p.client);
    if (!isHash(bytes) || !game || !game->id || (consoleId && game->console_id != consoleId)) {
        unloadGame();
        p.state.status = Localization::text(QStringLiteral("Невозможно проверить новый диск RetroAchievements"));
        p.publish();
        return;
    }
    if (p.loadHandle) {
        p.state.status = Localization::text(QStringLiteral("Дождитесь загрузки достижений перед сменой диска"));
        p.publish();
        return;
    }
    if (p.mediaHandle) rc_client_abort_async(p.client, p.mediaHandle);
    p.mediaHash = bytes;
    p.state.loading = true;
    p.state.status = Localization::text(QStringLiteral("Проверка нового диска…"));
    p.loadTimer.start();
    p.publish();
    p.mediaHandle = rc_client_begin_change_media(p.client, bytes.constData(), &Impl::mediaComplete, &p);
}

void AchievementsClient::unloadGame() {
    auto& p = *impl_;
    if (!p.client) return;
    if (p.loadHandle) rc_client_abort_async(p.client, p.loadHandle);
    if (p.mediaHandle) rc_client_abort_async(p.client, p.mediaHandle);
    p.loadHandle = nullptr;
    p.mediaHandle = nullptr;
    p.mediaHash.clear();
    rc_client_unload_game(p.client);
    p.gameHash.clear();
    p.gameDigest.clear();
    p.wantedHash.clear();
    p.state.loading = false;
    p.state.gameId = p.state.total = p.state.unlocked = p.state.points = 0;
    p.state.gameTitle.clear();
    p.state.entries.clear();
    p.publish();
}

void AchievementsClient::respond(quint64 id, int httpStatus, QByteArray body) {
    auto& p = *impl_;
    auto it = p.pending.find(id);
    if (it == p.pending.end()) return;
    const auto callback = it->second;
    p.pending.erase(it);
    rc_api_server_response_t response{};
    if (body.size() > kMaxHttpBody) {
        body = "RetroAchievements response exceeded size limit";
        response.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    } else if (httpStatus <= 0) {
        body = "RetroAchievements network request failed";
        response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    } else {
        response.http_status_code = httpStatus;
    }
    response.body = body.constData();
    response.body_length = size_t(body.size());
    callback.callback(&response, callback.userdata);
    if (p.state.hardcore != p.hardcore()) p.publish();
}

void AchievementsClient::frame() {
    auto& p = *impl_;
    if (p.client && p.state.enabled) {
        p.checkTimeout();
        rc_client_do_frame(p.client);
        if (p.state.hardcore != p.hardcore()) p.publish();
    }
}
void AchievementsClient::idle() {
    auto& p = *impl_;
    if (p.client && p.state.enabled) {
        p.checkTimeout();
        rc_client_idle(p.client);
        if (p.state.hardcore != p.hardcore()) p.publish();
    }
}
void AchievementsClient::reset() {
    auto& p = *impl_;
    if (p.client && p.state.enabled) rc_client_reset(p.client);
}
bool AchievementsClient::hardcore() const { return impl_->hardcore(); }
bool AchievementsClient::canPause(quint32* remaining) {
    auto& p = *impl_;
    uint32_t frames = 0;
    const bool allowed = !p.client || !p.state.enabled || rc_client_can_pause(p.client, &frames);
    if (remaining) *remaining = frames;
    return allowed;
}
bool AchievementsClient::wantsGame() const {
    const auto& p = *impl_;
    return p.state.enabled && (p.state.loggedIn || p.loginHandle != nullptr);
}
bool AchievementsClient::loading() const { return impl_->state.loading; }
AchievementsState AchievementsClient::snapshot() const { return impl_->state; }
void AchievementsClient::refresh() { impl_->rebuild(); }

QByteArray AchievementsClient::saveProgress(bool checksum) {
    auto& p = *impl_;
    if (!p.client || !p.state.enabled || !p.state.loggedIn || p.gameHash.isEmpty()) return {};
    const size_t size = rc_client_progress_size(p.client);
    if (size > kMaxProgressBlob - size_t(kProgressHeader) - 32) return {};
    QByteArray progress(qsizetype(size), Qt::Uninitialized);
    if (rc_client_serialize_progress_sized(p.client, reinterpret_cast<uint8_t*>(progress.data()), size) != RC_OK)
        return {};
    QByteArray output(kProgressMagic, 8);
    output += p.gameDigest;
    output += p.userDigest;
    const quint32 length = qToLittleEndian(quint32(size));
    output.append(reinterpret_cast<const char*>(&length), sizeof(length));
    output += progress;
    output += checksum ? digest(output) : QByteArray(32, '\0');
    return output;
}

void AchievementsClient::restoreProgress(QByteArray bytes, bool trusted) {
    auto& p = *impl_;
    if (!p.client || !p.state.enabled || !p.state.loggedIn || p.gameHash.isEmpty()) return;
    bool valid = bytes.size() >= kProgressHeader + 32 && bytes.left(8) == QByteArray(kProgressMagic, 8) &&
                 bytes.mid(8, 32) == p.gameDigest &&
                 bytes.mid(40, 32) == p.userDigest;
    quint32 length = 0;
    if (valid) {
        length = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + 72));
        valid = length <= kMaxProgressBlob - size_t(kProgressHeader) - 32 &&
                bytes.size() == kProgressHeader + qsizetype(length) + 32 &&
                (trusted || bytes.right(32) == digest(bytes.left(bytes.size() - 32)));
    }
    if (!valid) {
        rc_client_deserialize_progress(p.client, nullptr);
        p.state.status = Localization::text(QStringLiteral("Прогресс условий достижений сброшен: состояние не совпадает"));
        p.publish();
    } else {
        const auto* raw = reinterpret_cast<const uint8_t*>(bytes.constData() + kProgressHeader);
        if (rc_client_deserialize_progress_sized(p.client, raw, length) != RC_OK) {
            p.state.status = Localization::text(QStringLiteral("Не удалось восстановить прогресс условий достижений"));
            p.publish();
        }
    }
}
