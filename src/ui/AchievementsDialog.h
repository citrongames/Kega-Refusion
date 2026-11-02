#pragma once

#include "../core/Achievements.h"

#include <QDialog>
#include <QImage>
#include <QSet>

class AchievementModel;
class QLabel;
class QEvent;
class QTableView;
class QSortFilterProxyModel;

class AchievementsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit AchievementsDialog(QWidget* parent = nullptr);
    void setState(const AchievementsState& state);
    void setBadge(const QString& url, const QImage& image);
    void clearBadgeImages();

signals:
    void badgeRequested(const QString& url);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void scheduleVisibleBadges();
    void requestVisibleBadges();
    AchievementModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTableView* table_ = nullptr;
    QLabel* account_ = nullptr;
    QLabel* game_ = nullptr;
    QLabel* status_ = nullptr;
    QSet<QString> requestedBadges_;
    quint32 displayedGameId_ = 0;
    bool visibleScanPending_ = false;
    bool badgeRequestsPaused_ = false;
};
