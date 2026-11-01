#include "ControllerDiagram.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QtMath>

namespace {
constexpr int Up = 4;
constexpr int Down = 5;
constexpr int Left = 6;
constexpr int Right = 7;
const QColor ink(31, 35, 41);
const QColor cream(222, 219, 203);
const QColor accent(64, 180, 198);
const QColor selected(239, 184, 73);
}

ControllerDiagram::ControllerDiagram(QWidget* parent) : QWidget(parent) {
    setMinimumSize(290, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::PointingHandCursor);
}

QSize ControllerDiagram::sizeHint() const { return QSize(410, 300); }

void ControllerDiagram::setProfile(ConsoleProfile profile) {
    profile_ = profile;
    selectedTarget_ = -1;
    pressedMask_ = 0;
    update();
}

void ControllerDiagram::setTargets(const QVector<int>& targets) {
    targets_.clear();
    for (int target : targets)
        targets_.insert(target);
    update();
}

void ControllerDiagram::setSelectedTarget(int target) {
    selectedTarget_ = target;
    update();
}

void ControllerDiagram::setPressedMask(unsigned mask) {
    if (pressedMask_ != mask) {
        pressedMask_ = mask;
        update();
    }
}

QTransform ControllerDiagram::drawingTransform() const {
    const qreal scale = qMin(width() / 440.0, height() / 300.0);
    QTransform transform;
    transform.translate((width() - 440.0 * scale) / 2.0,
                        (height() - 300.0 * scale) / 2.0);
    transform.scale(scale, scale);
    return transform;
}

QColor ControllerDiagram::faceFor(int target, const QColor& normal) const {
    return target >= 0 && target < 16 && (pressedMask_ & (1u << target))
        ? accent : normal;
}

void ControllerDiagram::addHit(int target, const QRectF& area) {
    if (targets_.contains(target))
        hitAreas_.append({target, area});
}

void ControllerDiagram::drawButton(QPainter& painter, int target, QPointF center,
                                   qreal radius, const QString& label,
                                   const QColor& face) {
    if (!targets_.contains(target))
        return;
    const QRectF outer(center.x() - radius, center.y() - radius,
                       radius * 2, radius * 2);
    painter.setPen(QPen(target == selectedTarget_ ? selected : ink,
                        target == selectedTarget_ ? 4 : 2));
    painter.setBrush(faceFor(target, face));
    painter.drawEllipse(outer);
    painter.setPen(faceFor(target, face).lightness() < 125
                       ? QColor(245, 246, 249) : ink);
    QFont labelFont = painter.font();
    labelFont.setBold(true);
    labelFont.setPointSizeF(10);
    painter.setFont(labelFont);
    painter.drawText(outer, Qt::AlignCenter, label);
    addHit(target, outer.adjusted(-5, -5, 5, 5));
}

void ControllerDiagram::drawSmallButton(QPainter& painter, int target,
                                        const QRectF& area, const QString& label) {
    if (!targets_.contains(target))
        return;
    painter.setPen(QPen(target == selectedTarget_ ? selected : QColor(24, 27, 32),
                        target == selectedTarget_ ? 3 : 1.5));
    painter.setBrush(faceFor(target, QColor(70, 75, 82)));
    painter.drawRoundedRect(area, area.height() / 2, area.height() / 2);
    painter.setPen(QColor(246, 246, 247));
    QFont font = painter.font();
    font.setPointSizeF(8.5);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(area, Qt::AlignCenter, label);
    addHit(target, area.adjusted(-5, -5, 5, 5));
}

void ControllerDiagram::drawDpad(QPainter& painter, QPointF center, qreal size) {
    const qreal arm = size * 0.38;
    const QRectF up(center.x() - arm / 2, center.y() - size,
                    arm, size - arm / 2);
    const QRectF down(center.x() - arm / 2, center.y() + arm / 2,
                      arm, size - arm / 2);
    const QRectF left(center.x() - size, center.y() - arm / 2,
                      size - arm / 2, arm);
    const QRectF right(center.x() + arm / 2, center.y() - arm / 2,
                       size - arm / 2, arm);
    const QPair<int, QRectF> pieces[] = {
        {Up, up}, {Down, down}, {Left, left}, {Right, right}
    };
    for (const auto& piece : pieces) {
        if (!targets_.contains(piece.first))
            continue;
        painter.setPen(QPen(piece.first == selectedTarget_ ? selected : ink,
                            piece.first == selectedTarget_ ? 3 : 1));
        painter.setBrush(faceFor(piece.first, QColor(63, 68, 74)));
        painter.drawRoundedRect(piece.second, 3, 3);
        addHit(piece.first, piece.second.adjusted(-5, -5, 5, 5));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(48, 53, 59));
    painter.drawEllipse(QRectF(center.x() - 11, center.y() - 11, 22, 22));
    painter.setPen(QPen(QColor(160, 166, 172), 1.5));
    painter.drawLine(QPointF(center.x(), center.y() - size + 8),
                     QPointF(center.x(), center.y() - size + 18));
    painter.drawLine(QPointF(center.x(), center.y() + size - 8),
                     QPointF(center.x(), center.y() + size - 18));
    painter.drawLine(QPointF(center.x() - size + 8, center.y()),
                     QPointF(center.x() - size + 18, center.y()));
    painter.drawLine(QPointF(center.x() + size - 8, center.y()),
                     QPointF(center.x() + size - 18, center.y()));
}

void ControllerDiagram::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    painter.setTransform(drawingTransform());
    hitAreas_.clear();

    QFont title = painter.font();
    title.setPointSizeF(11);
    title.setBold(true);
    painter.setFont(title);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(QRectF(12, 4, 416, 28), Qt::AlignCenter,
                     InputManager::profileName(profile_));

    if (profile_ == ConsoleProfile::MegaDrive) {
        QPainterPath shell;
        shell.moveTo(33, 166);
        shell.cubicTo(21, 104, 68, 75, 132, 74);
        shell.cubicTo(193, 54, 272, 57, 332, 75);
        shell.cubicTo(403, 74, 429, 122, 408, 189);
        shell.cubicTo(396, 238, 363, 252, 321, 233);
        shell.cubicTo(270, 214, 174, 214, 122, 235);
        shell.cubicTo(72, 253, 42, 223, 33, 166);
        painter.setPen(QPen(QColor(19, 21, 25), 3));
        painter.setBrush(QColor(55, 58, 64));
        painter.drawPath(shell);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(90, 93, 99));
        painter.drawRoundedRect(QRectF(155, 86, 124, 20), 10, 10);
        drawDpad(painter, QPointF(117, 165), 35);
        drawSmallButton(painter, 2, QRectF(181, 113, 41, 17),
                        QStringLiteral("MODE"));
        drawSmallButton(painter, 3, QRectF(232, 113, 43, 17),
                        QStringLiteral("START"));
        const QColor red(165, 54, 60);
        const QColor slate(77, 81, 90);
        drawButton(painter, 10, QPointF(284, 145), 17, QStringLiteral("X"), slate);
        drawButton(painter, 9, QPointF(332, 133), 17, QStringLiteral("Y"), slate);
        drawButton(painter, 11, QPointF(380, 121), 17, QStringLiteral("Z"), slate);
        drawButton(painter, 1, QPointF(277, 191), 19, QStringLiteral("A"), red);
        drawButton(painter, 0, QPointF(328, 179), 19, QStringLiteral("B"), red);
        drawButton(painter, 8, QPointF(379, 167), 19, QStringLiteral("C"), red);
    } else if (profile_ == ConsoleProfile::MasterSystem) {
        painter.setPen(QPen(QColor(97, 99, 101), 2));
        painter.setBrush(cream);
        painter.drawRoundedRect(QRectF(27, 72, 386, 160), 16, 16);
        painter.setPen(QPen(QColor(165, 164, 150), 1));
        painter.drawRoundedRect(QRectF(39, 84, 362, 136), 12, 12);
        drawDpad(painter, QPointF(125, 153), 37);
        drawButton(painter, 0, QPointF(294, 167), 23, QStringLiteral("1"),
                   QColor(175, 62, 65));
        drawButton(painter, 8, QPointF(359, 145), 23, QStringLiteral("2"),
                   QColor(175, 62, 65));
        drawSmallButton(painter, 3, QRectF(177, 44, 55, 20),
                        QStringLiteral("PAUSE"));
        painter.setPen(QColor(88, 91, 96));
        QFont note = painter.font();
        note.setPointSizeF(8);
        note.setBold(false);
        painter.setFont(note);
        painter.drawText(QRectF(237, 44, 80, 20), Qt::AlignVCenter,
                         QStringLiteral("на консоли"));
    } else if (profile_ == ConsoleProfile::GameGear) {
        painter.setPen(QPen(QColor(24, 26, 30), 2));
        painter.setBrush(QColor(57, 61, 68));
        painter.drawRoundedRect(QRectF(18, 55, 404, 199), 47, 47);
        painter.setPen(QPen(QColor(19, 21, 25), 2));
        painter.setBrush(QColor(23, 30, 36));
        painter.drawRoundedRect(QRectF(155, 80, 145, 142), 13, 13);
        painter.setBrush(QColor(61, 113, 121));
        painter.drawRoundedRect(QRectF(169, 94, 117, 114), 3, 3);
        drawDpad(painter, QPointF(92, 155), 32);
        drawButton(painter, 0, QPointF(345, 165), 20, QStringLiteral("1"),
                   QColor(145, 50, 56));
        drawButton(painter, 8, QPointF(382, 130), 20, QStringLiteral("2"),
                   QColor(145, 50, 56));
        drawSmallButton(painter, 3, QRectF(202, 228, 53, 17),
                        QStringLiteral("START"));
    } else {
        painter.setPen(QPen(QColor(88, 90, 91), 2));
        painter.setBrush(QColor(205, 207, 199));
        painter.drawRoundedRect(QRectF(28, 70, 384, 166), 25, 25);
        painter.setPen(QPen(QColor(150, 152, 149), 2));
        painter.setBrush(QColor(83, 89, 93));
        painter.drawEllipse(QRectF(74, 93, 128, 128));
        painter.setPen(QPen(QColor(37, 41, 45), 6));
        painter.drawLine(QPointF(138, 157), QPointF(138, 120));
        painter.setPen(QPen(QColor(20, 22, 24), 2));
        painter.setBrush(QColor(48, 52, 57));
        painter.drawEllipse(QRectF(121, 99, 34, 34));
        // Invisible directional sectors over the joystick are still clickable.
        addHit(Up, QRectF(113, 93, 50, 42));
        addHit(Down, QRectF(113, 179, 50, 42));
        addHit(Left, QRectF(74, 133, 47, 48));
        addHit(Right, QRectF(155, 133, 47, 48));
        const QPair<int, QPointF> arrows[] = {
            {Up, QPointF(138, 83)}, {Down, QPointF(138, 228)},
            {Left, QPointF(64, 157)}, {Right, QPointF(212, 157)}
        };
        for (const auto& arrow : arrows) {
            if (!targets_.contains(arrow.first))
                continue;
            painter.setPen(arrow.first == selectedTarget_ ? selected
                           : (pressedMask_ & (1u << arrow.first)) ? accent : ink);
            QFont f = painter.font();
            f.setPointSizeF(14);
            f.setBold(true);
            painter.setFont(f);
            const QRectF arrowArea(arrow.second.x() - 14, arrow.second.y() - 14,
                                   28, 28);
            painter.drawText(arrowArea, Qt::AlignCenter,
                             arrow.first == Up ? QStringLiteral("▲")
                             : arrow.first == Down ? QStringLiteral("▼")
                             : arrow.first == Left ? QStringLiteral("◀")
                                                   : QStringLiteral("▶"));
            addHit(arrow.first, arrowArea.adjusted(-3, -3, 3, 3));
        }
        drawButton(painter, 0, QPointF(291, 163), 24, QStringLiteral("1"),
                   QColor(159, 59, 60));
        drawButton(painter, 8, QPointF(359, 139), 24, QStringLiteral("2"),
                   QColor(159, 59, 60));
    }

    painter.resetTransform();
    QColor hintColor = palette().color(QPalette::Text);
    hintColor.setAlpha(165);
    painter.setPen(hintColor);
    QFont hint = painter.font();
    hint.setPointSizeF(9);
    painter.setFont(hint);
    painter.drawText(QRect(8, height() - 24, width() - 16, 18),
                     Qt::AlignCenter, QStringLiteral("Нажмите кнопку на рисунке для выбора"));
}

void ControllerDiagram::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    bool invertible = false;
    const QPointF point = drawingTransform().inverted(&invertible)
                              .map(event->position());
    if (invertible) {
        for (int index = hitAreas_.size() - 1; index >= 0; --index) {
            if (hitAreas_[index].second.contains(point)) {
                emit targetClicked(hitAreas_[index].first);
                event->accept();
                return;
            }
        }
    }
    QWidget::mousePressEvent(event);
}
