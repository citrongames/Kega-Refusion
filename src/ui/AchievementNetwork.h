#pragma once

#include "../core/Achievements.h"

#include <QCache>
#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <QThreadPool>

class AchievementNetwork final : public QObject {
    Q_OBJECT
public:
    explicit AchievementNetwork(const QString& dataPath, QObject* parent = nullptr);
    ~AchievementNetwork() override;
    void perform(const AchievementHttpRequest& request);
    void requestBadge(const QString& url);
    void prefetchBadges(const QStringList& urls);

signals:
    void response(quint64 id, int status, const QByteArray& body);
    void badgeReady(const QString& url, const QImage& image);

private:
    void startBadges();
    void startBadgeNetwork(const QString& url, quint64 generation);
    void finishBadge(const QString& url, const QImage& image);
    void scheduleStartBadges();
    QString badgePath(const QString& url) const;

    QNetworkAccessManager manager_;
    QThreadPool badgeIoPool_;
    QString badgeDirectory_;
    QCache<QString, QImage> images_{16 * 1024 * 1024};
    QQueue<QString> badgeQueue_;
    QStringList prefetchUrls_;
    qsizetype prefetchCursor_ = 0;
    quint64 prefetchGeneration_ = 0;
    QSet<QString> prefetchSeen_;
    QSet<QString> badgePending_;
    QSet<QString> badgeWanted_;
    QSet<QString> badgeFailed_;
    int apiInFlight_ = 0;
    int badgeInFlight_ = 0;
    bool badgeStartScheduled_ = false;
};
