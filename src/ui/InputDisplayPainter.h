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

inline void md6(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.moveTo(2, 17);
    shape.cubicTo(3.5, 10, 8.5, 5, 16, 3);
    shape.cubicTo(24, 0.7, 34, 0.7, 41.5, 3);
    shape.cubicTo(49, 5, 54, 10, 55.7, 16);
    shape.cubicTo(58, 22.5, 55, 28.6, 50.6, 28.3);
    shape.cubicTo(47.6, 28.2, 45.6, 25.6, 42, 24.1);
    shape.cubicTo(34.5, 21, 23.5, 21, 16, 24.3);
    shape.cubicTo(12, 26, 10.5, 28.3, 7.2, 28.2);
    shape.cubicTo(2.4, 28.2, 0.3, 23.2, 2, 17);
    shape.closeSubpath();
    body(p, shape);

    p.save();
    p.translate(13.5, 15);
    p.setPen(QPen(idle(), 0.7));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(), 7.5, 7.5);
    p.scale(0.77, 0.77);
    dpad(p, s, 0, 0);
    p.restore();

    miniButton(p, QRectF(25, 11.8, 5.5, 2.2), down(s, 3)); // Start
    p.save();
    p.translate(42, 5.6);
    p.rotate(18);
    miniButton(p, QRectF(-2.5, -0.8, 5, 1.6), down(s, 2)); // Mode
    p.restore();

    button(p, 36.5, 12.5, 1.9, down(s, 10)); // X
    button(p, 42,   11,   1.9, down(s, 9));  // Y
    button(p, 47.5, 10,   1.9, down(s, 11)); // Z
    button(p, 38,   18.5, 2.5, down(s, 1));  // A
    button(p, 44,   17,   2.5, down(s, 0));  // B
    button(p, 50,   16,   2.5, down(s, 8));  // C
}

inline void md3(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.moveTo(1.6, 13.5);
    shape.cubicTo(2.8, 6.4, 15.8, 1, 29, 1);
    shape.cubicTo(42.2, 1, 55.2, 6.4, 56.4, 13.5);
    shape.cubicTo(57.4, 18.2, 54.3, 24.7, 50.5, 27.6);
    shape.cubicTo(47, 30.1, 43, 29, 41.1, 25.7);
    shape.cubicTo(38.8, 21.6, 35.4, 20.5, 29, 20.5);
    shape.cubicTo(22.6, 20.5, 19.2, 21.6, 16.9, 25.7);
    shape.cubicTo(15, 29, 11, 30.1, 7.5, 27.6);
    shape.cubicTo(3.7, 24.7, 0.6, 18.2, 1.6, 13.5);
    shape.closeSubpath();
    body(p, shape);

    p.save();
    p.translate(14, 14.5);
    p.setPen(QPen(idle(), 0.7));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(), 7.7, 7.7);
    p.scale(0.79, 0.79);
    dpad(p, s, 0, 0);
    p.restore();

    p.save();
    p.translate(41, 8);
    p.rotate(-26);
    miniButton(p, QRectF(-2.9, -1, 5.8, 2), down(s, 3)); // Start
    p.restore();

    button(p, 38.5, 18.1, 2.8, down(s, 1)); // A
    button(p, 44.5, 15.3, 2.8, down(s, 0)); // B
    button(p, 50.4, 13.1, 2.8, down(s, 8)); // C
}

inline void compactDpad(QPainter& p, const InputDisplaySlot& s,
                        QPointF center, qreal scale) {
    p.save();
    p.translate(center);
    p.scale(scale, scale);
    dpad(p, s, 0, 0);
    p.restore();
}

inline void sms(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.addRoundedRect(QRectF(1, 6, 56, 22), 1.7, 1.7);
    body(p, shape);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(166, 76, 73), 0.65));
    p.drawRoundedRect(QRectF(3, 8.5, 52, 17), 1, 1);
    p.setPen(Qt::NoPen);
    p.setBrush(idle().darker(150));
    p.drawRoundedRect(QRectF(5.5, 10, 14, 14), 1.2, 1.2);
    compactDpad(p, s, {12.5, 17}, 0.73);
    button(p, 39, 18.5, 3.4, down(s, 0));
    button(p, 49, 18.5, 3.4, down(s, 8));
    // Pause is on the console. Keep its indicator separate from the pad.
    miniButton(p, QRectF(25.5, 1.2, 7, 2.7), down(s, 3));
}

inline void sg(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.addRoundedRect(QRectF(1, 4, 56, 22), 3, 3);
    body(p, shape, QColor(182, 182, 163, 245));
    p.setPen(QPen(idle(), 0.75));
    p.setBrush(shell());
    p.drawEllipse(QPointF(14.5, 15), 7.4, 7.4);
    compactDpad(p, s, {14.5, 15}, 0.74);
    p.setPen(Qt::NoPen);
    p.setBrush(idle());
    p.drawRect(QRectF(34, 6.6, 19, 2));
    miniButton(p, QRectF(36, 15, 4.9, 7.2), down(s, 0));
    miniButton(p, QRectF(46, 15, 4.9, 7.2), down(s, 8));
}

inline void gear(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.moveTo(4, 5);
    shape.cubicTo(6, 2.2, 51, 2.2, 54, 5);
    shape.cubicTo(56, 7, 58, 23, 55, 25.5);
    shape.cubicTo(52, 28.7, 6, 28.7, 3, 25.5);
    shape.cubicTo(0, 23, 2, 7, 4, 5);
    shape.closeSubpath();
    body(p, shape);
    p.setPen(QPen(idle(), 1.3));
    p.setBrush(QColor(8, 20, 29));
    p.drawRoundedRect(QRectF(16, 7, 25, 16), 1.4, 1.4);
    compactDpad(p, s, {9, 15}, 0.64);
    miniButton(p, QRectF(46, 8.6, 4.5, 1.9), down(s, 3));
    button(p, 46, 17, 2.2, down(s, 0));
    button(p, 51.5, 14.5, 2.2, down(s, 8));
}

inline void muzzle(QPainter& p, const InputDisplaySlot& s, QRectF rect) {
    p.setPen(Qt::NoPen);
    p.setBrush((s.offscreen || down(s, 2)) ? QColor(255, 173, 82) : idle());
    p.drawRoundedRect(rect, 0.5, 0.5);
}

inline void phaser(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.moveTo(2, 5.5); shape.lineTo(5, 3.5); shape.lineTo(49, 3.5);
    shape.lineTo(50, 2); shape.lineTo(56, 4); shape.lineTo(55, 9);
    shape.lineTo(26, 11); shape.lineTo(22.5, 15); shape.lineTo(19, 15);
    shape.lineTo(17, 27.5); shape.lineTo(10.5, 27.5); shape.lineTo(12.5, 13);
    shape.lineTo(5, 10); shape.lineTo(2, 9);
    shape.closeSubpath();
    body(p, shape);
    miniButton(p, QRectF(19.3, 10.7, 3.2, 3), down(s, 1) || down(s, 2));
    muzzle(p, s, QRectF(52, 5, 2, 3));
}

inline void menacer(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.moveTo(3, 5.5); shape.lineTo(5, 3); shape.lineTo(17, 3);
    shape.lineTo(19, 5); shape.lineTo(38, 5); shape.lineTo(41, 2.5);
    shape.lineTo(43, 5); shape.lineTo(53, 5); shape.lineTo(55, 8);
    shape.lineTo(54, 12); shape.lineTo(44, 12); shape.lineTo(48, 23);
    shape.quadTo(51, 30, 44, 28); shape.lineTo(26, 24); shape.lineTo(13, 14);
    shape.lineTo(4, 13); shape.lineTo(2, 11); shape.lineTo(2, 7);
    shape.closeSubpath();
    // Open space between the forward support and the rear grip.
    shape.moveTo(20, 13.5); shape.lineTo(31.5, 13.5); shape.lineTo(34, 17.5);
    shape.lineTo(37, 17.5); shape.lineTo(41, 23.5); shape.lineTo(28, 21);
    shape.closeSubpath();
    body(p, shape, QColor(101, 111, 119, 245));
    muzzle(p, s, QRectF(3.3, 6.5, 2.6, 5));
    button(p, 34.8, 14.5, 1.65, down(s, 1) || down(s, 2));
    button(p, 23.5, 9.4, 1.7, down(s, 0));
    button(p, 29, 9.4, 1.7, down(s, 8));
    miniButton(p, QRectF(34, 8, 5, 2.5), down(s, 3));
}

inline void justifier(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.moveTo(2, 5); shape.lineTo(3, 3); shape.lineTo(26, 3);
    shape.lineTo(28, 2); shape.lineTo(38, 2); shape.lineTo(40, 5);
    shape.lineTo(43, 4); shape.lineTo(43, 7); shape.quadTo(52, 12, 55, 26);
    shape.quadTo(56, 28, 53, 28); shape.lineTo(46, 28);
    shape.quadTo(46, 18, 41, 15); shape.quadTo(38, 21, 33, 20);
    shape.quadTo(27, 20, 26, 13); shape.lineTo(24, 10); shape.lineTo(2, 10);
    shape.closeSubpath();
    shape.addEllipse(QRectF(29, 12.4, 9, 5.6));
    body(p, shape, QColor(31, 89, 166, 242));
    p.setPen(QPen(rim(), 0.6));
    p.setBrush(QColor(25, 64, 124));
    p.drawRoundedRect(QRectF(26, 4, 10.5, 5.5), 2, 2);
    button(p, 40, 8.7, 1.45, down(s, 3));
    miniButton(p, QRectF(34, 11.5, 2.3, 4.1), down(s, 1) || down(s, 2));
    muzzle(p, s, QRectF(3, 4.6, 2, 4));
}

inline void mouse(QPainter& p, const InputDisplaySlot& s) {
    QPainterPath shape;
    shape.moveTo(27, 2.5);
    shape.cubicTo(19, 3, 18, 9, 18.7, 16);
    shape.cubicTo(19.6, 24.5, 22.7, 28.5, 29, 28.5);
    shape.cubicTo(35.3, 28.5, 38.4, 24.5, 39.3, 16);
    shape.cubicTo(40, 9, 39, 3, 31, 2.5);
    shape.closeSubpath();
    body(p, shape);
    miniButton(p, QRectF(20.7, 6.7, 4.4, 7.3), down(s, 1)); // A, B, C
    miniButton(p, QRectF(26.8, 4.5, 4.4, 8.5), down(s, 0));
    miniButton(p, QRectF(32.9, 6.7, 4.4, 7.3), down(s, 8));
    miniButton(p, QRectF(26.4, 24, 5.2, 2), down(s, 3));
    const int dx = qBound(-1, int(s.x), 1);
    const int dy = qBound(-1, int(s.y), 1);
    const QPointF center(29, 18.5);
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
    shape.addRoundedRect(QRectF(2, 5, 54, 23), 4, 4);
    body(p, shape, QColor(193, 183, 160, 245));
    // The top and front buttons share one electrical contact.
    miniButton(p, QRectF(43, 2.8, 6, 2.8), down(s, 0));
    p.setPen(QPen(QColor(140, 160, 171), 0.8));
    p.setBrush(QColor(12, 23, 32));
    p.drawEllipse(QPointF(17, 16), 8.8, 8.8);
    const qreal fraction = qBound<qreal>(0, (qreal(s.x) + 32768.0) / 65535.0, 1);
    const qreal angle = (225.0 - 270.0 * fraction) * 3.141592653589793 / 180.0;
    p.setPen(QPen(pressed(), 1.8, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(17, 16), QPointF(17 + 7 * std::cos(angle), 16 - 7 * std::sin(angle)));
    button(p, 46, 18, 3.6, down(s, 0));
}

inline void device(QPainter& p, const InputDisplaySlot& s) {
    switch (s.device) {
    case InputDisplayDevice::MegaDrive3: md3(p, s); break;
    case InputDisplayDevice::MegaDrive6: md6(p, s); break;
    case InputDisplayDevice::MasterSystem: sms(p, s); break;
    case InputDisplayDevice::GameGear: gear(p, s); break;
    case InputDisplayDevice::SG1000: sg(p, s); break;
    case InputDisplayDevice::LightPhaser: phaser(p, s); break;
    case InputDisplayDevice::Menacer: menacer(p, s); break;
    case InputDisplayDevice::Justifier: justifier(p, s); break;
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
