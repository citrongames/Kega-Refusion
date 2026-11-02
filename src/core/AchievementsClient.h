#pragma once

#include "Achievements.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <cstdint>
#include <functional>
#include <memory>

// Owned and called exclusively by the emulation worker thread. HTTP is supplied by Hooks.
class AchievementsClient final {
public:
    struct Hooks {
        std::function<uint32_t(uint32_t, uint8_t*, uint32_t)> readMemory;
        std::function<void(const AchievementHttpRequest&)> request;
        std::function<void(const AchievementsState&)> changed;
        std::function<void(const AchievementNotice&)> notice;
        std::function<void()> resetRequested;
    };

    explicit AchievementsClient(QString dataPath, Hooks hooks);
    ~AchievementsClient();
    AchievementsClient(const AchievementsClient&) = delete;
    AchievementsClient& operator=(const AchievementsClient&) = delete;

    void configure(bool enabled, bool hardcore);
    void login(QString user, QString password);
    void logout();
    void loadGame(QString hash, unsigned consoleId);
    void changeMedia(QString hash, unsigned consoleId);
    void unloadGame();
    void respond(quint64 id, int httpStatus, QByteArray body);
    void frame();
    void idle();
    void reset();
    bool hardcore() const;
    bool canPause(quint32* remaining = nullptr);
    bool wantsGame() const;
    bool loading() const;
    AchievementsState snapshot() const;
    void refresh();
    QByteArray saveProgress(bool checksum = true);
    void restoreProgress(QByteArray progress, bool trusted = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
