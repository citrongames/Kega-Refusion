#include "AchievementPositionPicker.h"
#include "i18n/Localization.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QPainter>
#include <QRadioButton>
#include <QPainterPath>
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
        const bool dark = qGray(palette().color(QPalette::Base).rgb()) < 128;
        const QColor ink = dark ? QColor(239, 241, 244) : QColor(36, 41, 48);
        const QColor paper = dark ? Qt::black : Qt::white;
        const QRectF zone = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);

        if (isChecked()) {
            painter.setPen(QPen(ink, 2));
            painter.setBrush(ink);
            painter.drawRoundedRect(zone, 4, 4);

            const QPointF center = zone.center();
            QPainterPath check(center + QPointF(-8, 0));
            check.lineTo(center + QPointF(-2, 6));
            check.lineTo(center + QPointF(9, -6));
            painter.setPen(QPen(paper, 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(check);
        } else {
            const QSize badge(29, 13);
            const int x = (width() - badge.width()) * (position_ % 3) / 2;
            const int y = position_ < 3 ? 0 : height() - badge.height();
            const QRectF card(x + 0.5, y + 0.5, badge.width() - 1, badge.height() - 1);
            QColor outline = ink;
            outline.setAlpha(underMouse() ? 215 : 140);
            painter.setPen(outline);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(card, 2, 2);
            painter.drawRect(QRectF(x + 4, y + 4, 4, 4));
            painter.drawLine(x + 12, y + 5, x + 24, y + 5);
            painter.drawLine(x + 12, y + 8, x + 20, y + 8);
        }

        if (hasFocus() && testAttribute(Qt::WA_KeyboardFocusChange)) {
            painter.setPen(QPen(isChecked() ? paper : ink, 1, Qt::DotLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(zone.adjusted(2, 2, -2, -2), 2, 2);
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
