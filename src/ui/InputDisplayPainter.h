#pragma once

#include "core/InputDisplay.h"

#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <algorithm>
#include <cmath>

// Small native controller silhouettes. All unused pixels remain transparent.
namespace InputDisplayPainter {
namespace detail {
constexpr qreal unitWidth = 58;
constexpr qreal unitHeight = 30;
constexpr qreal gap = 3;

inline bool down(const InputDisplaySlot& slot, unsigned bit) {
    return (slot.buttons & (1u << bit)) != 0;
}
inline QColor rim() { return QColor(208, 221, 229, 235); }
inline QColor shell() { return QColor(19, 27, 37, 225); }
inline QColor idle() { return QColor(53, 69, 82, 245); }
inline QColor pressed() { return QColor(105, 238, 244); }

inline void body(QPainter& p, const QPainterPath& path, const QColor& fill = shell()) {
    p.setPen(QPen(rim(), 1));
    p.setBrush(fill);
    p.drawPath(path);
}

inline void button(QPainter& p, qreal x, qreal y, qreal radius, bool held,
                   const QColor& released = idle()) {
    p.setPen(QPen(held ? QColor(222, 255, 255) : QColor(151, 174, 186), 0.75));
    p.setBrush(held ? pressed() : released);
    p.drawEllipse(QPointF(x, y), radius, radius);
}

inline void miniButton(QPainter& p, const QRectF& r, bool held) {
    p.setPen(QPen(held ? QColor(222, 255, 255) : QColor(151, 174, 186), 0.7));
    p.setBrush(held ? pressed() : idle());
    p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
}

inline void dpad(QPainter& p, const InputDisplaySlot& s, qreal x, qreal y) {
    const QRectF arms[] = {
        {x - 2, y - 8, 4, 6}, {x - 2, y + 2, 4, 6},
        {x - 8, y - 2, 6, 4}, {x + 2, y - 2, 6, 4}
    };
    const unsigned bits[] = {4, 5, 6, 7};
    for (int i = 0; i < 4; ++i) {
        const bool held = down(s, bits[i]);
        p.setPen(QPen(held ? QColor(226, 255, 255) : QColor(135, 154, 165), 0.5));
        p.setBrush(held ? pressed() : idle());
        p.drawRoundedRect(arms[i], 0.7, 0.7);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(idle());
    p.drawRect(QRectF(x - 2, y - 2, 4, 4));
}

inline void md(QPainter& p, const InputDisplaySlot& s, bool six) {
    QPainterPath shape;
    if (six) {
        shape.moveTo(5, 8); shape.cubicTo(10, 3, 18, 5, 26, 7);
        shape.cubicTo(36, 7, 46, 3, 53, 8);
        shape.cubicTo(60, 14, 56, 26, 49, 28);
        shape.cubicTo(43, 30, 38, 25, 29, 25);
        shape.cubicTo(19, 25, 15, 30, 9, 28);
        shape.cubicTo(1, 26, -1, 14, 5, 8);
    } else {
        shape.moveTo(7, 6); shape.cubicTo(17, 1, 42, 1, 51, 6);
        shape.cubicTo(59, 11, 58, 24, 51, 27);
        shape.cubicTo(44, 30, 38, 25, 29, 25);
        shape.cubicTo(19, 25, 14, 30, 7, 27);
        shape.cubicTo(0, 24, -1, 12, 7, 6);
    }
    shape.closeSubpath();
    body(p, shape);
    dpad(p, s, 13, 16);
    if (six) {
        const qreal xs[] = {37, 45, 53};
        const unsigned upper[] = {10, 9, 11};
        const unsigned lower[] = {1, 0, 8};
        for (int i = 0; i < 3; ++i) {
            button(p, xs[i], 11, 2.6, down(s, upper[i]));
            button(p, xs[i], 20, 3.1, down(s, lower[i]));
        }
        miniButton(p, QRectF(23, 12, 8, 3), down(s, 3));
        miniButton(p, QRectF(23, 20, 6, 2.7), down(s, 2));
    } else {
        button(p, 37, 17, 3.3, down(s, 1));
        button(p, 45, 17, 3.3, down(s, 0));
        button(p, 53, 17, 3.3, down(s, 8));
        miniButton(p, QRectF(25, 10, 7, 3.2), down(s, 3));
    }
}

inline void sms(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.addRoundedRect(QRectF(1, 5, 56, 23), 3, 3);
    body(p, shape);
    dpad(p, s, 14, 16);
    button(p, 42, 17, 3.7, down(s, 0));
    button(p, 52, 17, 3.7, down(s, 8));
    // Master System Pause sits on the console, outside the pad body.
    miniButton(p, QRectF(26, 1, 7, 2.7), down(s, 3));
}

inline void sg(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.addRoundedRect(QRectF(2, 3, 54, 25), 2, 2);
    body(p, shape, QColor(128, 140, 142, 245));
    p.setPen(QPen(QColor(31, 43, 47), 0.8));
    p.setBrush(QColor(36, 44, 49));
    p.drawEllipse(QPointF(16, 16), 10, 10);
    const qreal dx = (down(s, 7) ? 3 : 0) - (down(s, 6) ? 3 : 0);
    const qreal dy = (down(s, 5) ? 3 : 0) - (down(s, 4) ? 3 : 0);
    p.setPen(QPen(QColor(212, 227, 230), 2.4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(16, 18), QPointF(16 + dx, 14 + dy));
    button(p, 16 + dx, 13 + dy, 3.5, dx || dy, QColor(28, 35, 40));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(103, 40, 46));
    p.drawRoundedRect(QRectF(34, 8, 19, 16), 3, 3);
    button(p, 39, 16, 3.3, down(s, 0), QColor(125, 51, 55));
    button(p, 49, 16, 3.3, down(s, 8), QColor(125, 51, 55));
}

inline void gear(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.addRoundedRect(QRectF(1, 3, 56, 25), 10, 10);
    body(p, shape, QColor(28, 41, 57, 235));
    p.setPen(QPen(QColor(125, 149, 162), 0.8));
    p.setBrush(QColor(8, 20, 29));
    p.drawRoundedRect(QRectF(23, 7, 16, 15), 2, 2);
    dpad(p, s, 12, 16);
    button(p, 47, 12, 2.9, down(s, 0));
    button(p, 52, 20, 2.9, down(s, 8));
    miniButton(p, QRectF(27, 24, 7, 2.6), down(s, 3));
}

inline void gun(QPainter& p, const InputDisplaySlot& s, InputDisplayDevice device) {
    QPainterPath shape;
    QColor fill = shell();
    if (device == InputDisplayDevice::LightPhaser) {
        shape.moveTo(3, 11); shape.lineTo(46, 8); shape.lineTo(54, 10);
        shape.lineTo(56, 15); shape.lineTo(30, 17); shape.lineTo(26, 22);
        shape.lineTo(23, 29); shape.lineTo(14, 29); shape.lineTo(17, 18);
        shape.lineTo(3, 17);
    } else if (device == InputDisplayDevice::Menacer) {
        fill = QColor(102, 113, 120, 240);
        shape.moveTo(2, 10); shape.lineTo(35, 8); shape.lineTo(39, 4);
        shape.lineTo(51, 4); shape.lineTo(52, 9); shape.lineTo(57, 11);
        shape.lineTo(57, 17); shape.lineTo(30, 18); shape.lineTo(27, 29);
        shape.lineTo(16, 29); shape.lineTo(18, 19); shape.lineTo(2, 18);
    } else {
        fill = QColor(31, 89, 166, 242);
        shape.moveTo(2, 11); shape.lineTo(41, 8); shape.lineTo(52, 10);
        shape.lineTo(57, 13); shape.lineTo(57, 17); shape.lineTo(35, 18);
        shape.lineTo(29, 22); shape.lineTo(24, 29); shape.lineTo(14, 29);
        shape.lineTo(16, 20); shape.lineTo(2, 18);
    }
    shape.closeSubpath();
    body(p, shape, fill);
    if (device == InputDisplayDevice::Justifier) {
        p.setPen(QPen(QColor(211, 223, 230), 0.8));
        p.setBrush(QColor(25, 64, 124));
        p.drawEllipse(QPointF(35, 15), 5, 4); // Revolver cylinder.
    }
    if (device == InputDisplayDevice::Menacer) {
        button(p, 37, 13, 2, down(s, 0));
        button(p, 44, 13, 2, down(s, 8));
        miniButton(p, QRectF(30, 20, 5, 2.4), down(s, 3));
    } else if (device == InputDisplayDevice::Justifier) {
        miniButton(p, QRectF(29, 8, 5, 2.4), down(s, 3));
    }
    // Reload is the offscreen trigger action, not a separate physical button.
    button(p, 26, 20, 2.8, down(s, 1) || down(s, 2));
    p.setPen(Qt::NoPen);
    p.setBrush((s.offscreen || down(s, 2)) ? QColor(255, 173, 82) : QColor(76, 94, 103));
    p.drawRect(QRectF(54, 11, 3, 5));
}

inline void mouse(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.addRoundedRect(QRectF(13, 1, 32, 28), 12, 12);
    body(p, shape, QColor(27, 38, 51, 240));
    p.setPen(QPen(QColor(111, 131, 144), 0.7));
    p.drawLine(QPointF(29, 2), QPointF(29, 14));
    button(p, 20, 10, 3.6, down(s, 1));
    button(p, 38, 10, 3.6, down(s, 0));
    button(p, 21, 21, 2.4, down(s, 8));
    miniButton(p, QRectF(34, 19, 5, 3), down(s, 3));
    const int dx = qBound(-1, int(s.x), 1);
    const int dy = qBound(-1, int(s.y), 1);
    const QPointF center(29, 21);
    p.setPen(QPen(QColor(136, 158, 171), 0.6));
    p.setBrush(QColor(9, 24, 35));
    p.drawEllipse(center, 4.2, 4.2);
    if (dx || dy) {
        const qreal length = std::sqrt(qreal(dx * dx + dy * dy));
        const QPointF tip(center.x() + 3.7 * dx / length,
                          center.y() + 3.7 * dy / length);
        const QPointF flank(-dy / length, dx / length);
        QPainterPath arrow;
        arrow.moveTo(tip);
        arrow.lineTo(center + flank * 1.8);
        arrow.lineTo(center - flank * 1.8);
        arrow.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(pressed());
        p.drawPath(arrow);
    }
}

inline void paddle(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.addRoundedRect(QRectF(3, 3, 52, 25), 3, 3);
    body(p, shape);
    p.setPen(QPen(QColor(140, 160, 171), 0.8));
    p.setBrush(QColor(12, 23, 32));
    p.drawEllipse(QPointF(23, 15), 10, 10);
    const qreal fraction = qBound<qreal>(0, (qreal(s.x) + 32768.0) / 65535.0, 1);
    const qreal angle = (225.0 - 270.0 * fraction) * 3.141592653589793 / 180.0;
    p.setPen(QPen(pressed(), 1.8, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(23, 15), QPointF(23 + 8 * std::cos(angle), 15 - 8 * std::sin(angle)));
    button(p, 46, 16, 4.4, down(s, 0));
}

inline void device(QPainter& p, const InputDisplaySlot& s) {
    switch (s.device) {
    case InputDisplayDevice::MegaDrive3: md(p, s, false); break;
    case InputDisplayDevice::MegaDrive6: md(p, s, true); break;
    case InputDisplayDevice::MasterSystem: sms(p, s); break;
    case InputDisplayDevice::GameGear: gear(p, s); break;
    case InputDisplayDevice::SG1000: sg(p, s); break;
    case InputDisplayDevice::LightPhaser:
    case InputDisplayDevice::Menacer:
    case InputDisplayDevice::Justifier: gun(p, s, s.device); break;
    case InputDisplayDevice::Mouse: mouse(p, s); break;
    case InputDisplayDevice::Paddle: paddle(p, s); break;
    case InputDisplayDevice::None: break;
    }
}
} // namespace detail

struct RowGeometry {
    QPointF topLeft;
    QSizeF size;
    qreal scale = 0;
};

inline InputDisplayState visualState(InputDisplayState state) {
    for (auto& slot : state.players) {
        if (slot.device == InputDisplayDevice::Paddle) slot.y = 0;
        else if (slot.device != InputDisplayDevice::Mouse) slot.x = slot.y = 0;
        if (slot.device != InputDisplayDevice::LightPhaser &&
            slot.device != InputDisplayDevice::Menacer &&
            slot.device != InputDisplayDevice::Justifier) slot.offscreen = false;
    }
    return state;
}

inline RowGeometry geometry(const InputDisplayState& state, const QRect& bounds) {
    unsigned count = 0;
    for (const auto& slot : state.players)
        if (slot.device != InputDisplayDevice::None) ++count;
    if (!count) return {};
    constexpr qreal margin = 8;
    const qreal width = count * detail::unitWidth + (count - 1) * detail::gap;
    const qreal scale = qMin(2.0, (bounds.width() - 2 * margin) / width);
    if (scale <= 0) return {};
    return {{bounds.left() + margin,
             bounds.bottom() + 1 - margin - detail::unitHeight * scale},
            {width * scale, detail::unitHeight * scale}, scale};
}

inline void paintRow(QPainter& painter, const InputDisplayState& state, qreal scale) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(scale, scale);
    unsigned drawn = 0;
    for (const auto& slot : state.players) {
        if (slot.device == InputDisplayDevice::None) continue;
        painter.save();
        painter.translate(drawn * (detail::unitWidth + detail::gap), 0);
        detail::device(painter, slot);
        painter.restore();
        ++drawn;
    }
    painter.restore();
}

inline void paint(QPainter& painter, const InputDisplayState& state, const QRect& bounds) {
    const RowGeometry row = geometry(state, bounds);
    if (row.scale <= 0) return;
    painter.save();
    painter.translate(row.topLeft);
    paintRow(painter, state, row.scale);
    painter.restore();
}
} // namespace InputDisplayPainter
