#pragma once

#include "../core/Achievements.h"

#include <QCache>
#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QSet>

class QNetworkDiskCache;

class AchievementNetwork final : public QObject {
    Q_OBJECT
public:
    explicit AchievementNetwork(const QString& dataPath, QObject* parent = nullptr);
    void perform(const AchievementHttpRequest& request);
    void requestBadge(const QString& url);

signals:
    void response(quint64 id, int status, const QByteArray& body);
    void badgeReady(const QString& url, const QImage& image);

private:
    void startBadges();
    QNetworkAccessManager manager_;
    QNetworkDiskCache* badgeCache_ = nullptr;
    QCache<QString, QImage> images_{16 * 1024 * 1024};
    QQueue<QString> badgeQueue_;
    QSet<QString> badgePending_;
    QSet<QString> badgeFailed_;
    int apiInFlight_ = 0;
    int badgeInFlight_ = 0;
};
