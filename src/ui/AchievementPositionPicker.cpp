#include "AchievementPositionPicker.h"
#include "i18n/Localization.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QPainter>
#include <QRadioButton>
#include <QStyle>
#include <QStyleOptionFocusRect>
#include <array>

namespace {
class PositionButton final : public QRadioButton {
public:
    PositionButton(int position, const QString& name, QWidget* parent)
        : QRadioButton(parent), position_(position) {
        setAccessibleName(name);
        setToolTip(name);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

protected:
    bool hitButton(const QPoint& point) const override { return rect().contains(point); }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QSize badge(29, 13);
        const int x = (width() - badge.width()) * (position_ % 3) / 2;
        const int y = position_ < 3 ? 0 : height() - badge.height();
        const QRectF card(x + 0.5, y + 0.5, badge.width() - 1, badge.height() - 1);
        const auto colors = palette();
        const QColor edge = colors.color(isChecked() || underMouse() ? QPalette::Highlight : QPalette::Mid);
        painter.setPen(edge);
        painter.setBrush(colors.color(isChecked() ? QPalette::Highlight : QPalette::Base));
        painter.drawRoundedRect(card, 2, 2);
        painter.setPen(colors.color(isChecked() ? QPalette::HighlightedText : QPalette::Text));
        painter.drawRect(QRectF(x + 4, y + 4, 4, 4));
        painter.drawLine(x + 12, y + 5, x + 24, y + 5);
        painter.drawLine(x + 12, y + 8, x + 20, y + 8);
        if (hasFocus()) {
            QStyleOptionFocusRect focus;
            focus.initFrom(this);
            focus.rect = rect().adjusted(1, 1, -1, -1);
            style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
        }
    }

private:
    int position_;
};
}

AchievementPositionPicker::AchievementPositionPicker(int selectedPosition, QWidget* parent)
    : QFrame(parent), positions_(new QButtonGroup(this)) {
    setObjectName(QStringLiteral("achievementPosition"));
    setAccessibleName(Localization::text(QStringLiteral("Положение уведомлений:")));
    setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);
    setFixedSize(164, 82);
    setLayoutDirection(Qt::LeftToRight);
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(5, 5, 5, 5);
    grid->setSpacing(0);
    const std::array names{
        QStringLiteral("Сверху слева"), QStringLiteral("Сверху по центру"), QStringLiteral("Сверху справа"),
        QStringLiteral("Снизу слева"), QStringLiteral("Снизу по центру"), QStringLiteral("Снизу справа")
    };
    for (int i = 0; i < int(names.size()); ++i) {
        auto* button = new PositionButton(i, Localization::text(names[i]), this);
        positions_->addButton(button, i);
        grid->addWidget(button, i / 3, i % 3);
    }
    auto* selected = positions_->button(qBound(0, selectedPosition, 5));
    selected->setChecked(true);
    setFocusProxy(selected);
}

int AchievementPositionPicker::position() const {
    return positions_->checkedId();
}
