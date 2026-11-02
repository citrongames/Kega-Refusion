#pragma once
#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QtGlobal>

// Set only after RetroAchievements has approved this product User-Agent for
// server Hardcore. Unknown clients may be silently downgraded to Softcore.
inline constexpr bool AchievementsHardcoreApproved = false;

struct AchievementEntry {
    quint32 id = 0, points = 0;
    QString title, description, badgeUrl, lockedBadgeUrl, progress;
    double rarity = 0.0, hardcoreRarity = 0.0;
    bool softcoreUnlocked = false, hardcoreUnlocked = false, unsupported = false;
    int type = 0; // rcheevos standard / missable / progression / win
};
struct AchievementsState {
    bool enabled = true, loggedIn = false, loading = false, hardcore = false;
    bool disconnected = false;
    QString username, gameTitle, status;
    quint32 gameId = 0, total = 0, unlocked = 0, points = 0;
    QList<AchievementEntry> entries;
};
struct AchievementNotice {
    enum Kind { GameReady, Unlocked, Completed } kind = Unlocked;
    QString title, description, badgeUrl;
    bool hardcore = false;
};
struct AchievementHttpRequest {
    quint64 id = 0;
    QString url;
    QByteArray postData;
    QString contentType;
};
Q_DECLARE_METATYPE(AchievementEntry)
Q_DECLARE_METATYPE(AchievementsState)
Q_DECLARE_METATYPE(AchievementNotice)
Q_DECLARE_METATYPE(AchievementHttpRequest)
