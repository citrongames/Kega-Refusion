#pragma once

#include <QFrame>

class QButtonGroup;

class AchievementPositionPicker final : public QFrame {
public:
    explicit AchievementPositionPicker(int selectedPosition, QWidget* parent = nullptr);
    int position() const;

private:
    QButtonGroup* positions_;
};
