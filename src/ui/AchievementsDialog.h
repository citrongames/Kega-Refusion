#pragma once

#include "../core/Achievements.h"

#include <QDialog>
#include <QImage>

class AchievementModel;
class QLabel;
class QEvent;
class QTableView;

class AchievementsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit AchievementsDialog(QWidget* parent = nullptr);
    void setState(const AchievementsState& state);
    void setBadge(const QString& url, const QImage& image);

signals:
    void badgeRequested(const QString& url);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void requestVisibleBadges();
    AchievementModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QLabel* account_ = nullptr;
    QLabel* game_ = nullptr;
    QLabel* status_ = nullptr;
};
