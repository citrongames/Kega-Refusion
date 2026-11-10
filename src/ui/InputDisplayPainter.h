#pragma once

#include "core/InputDisplay.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <algorithm>
#include <cmath>

// All coordinates below are in a small, fixed card. The complete row is scaled
// once to the available width, so every connected input slot remains visible.
namespace InputDisplayPainter {
namespace detail {
constexpr qreal cardWidth = 150.0;
constexpr qreal cardHeight = 86.0;
constexpr qreal gap = 6.0;

inline bool down(const InputDisplaySlot& slot, unsigned bit) {
    return (slot.buttons & (1u << bit)) != 0;
}

inline QColor active() { return QColor(108, 230, 239); }
inline QColor idle() { return QColor(50, 62, 76); }
inline QColor rim() { return QColor(136, 153, 170); }
inline QColor textColor() { return QColor(246, 250, 252); }

inline void disc(QPainter& p, qreal x, qreal y, qreal r,
                 const QString& label, bool pressed) {
    p.setPen(QPen(pressed ? QColor(220, 255, 255) : rim(), 1));
    p.setBrush(pressed ? active() : idle());
    p.drawEllipse(QPointF(x, y), r, r);
    if (!label.isEmpty()) {
        QFont font = p.font();
        font.setPixelSize(r < 8 ? 8 : 9);
        font.setWeight(QFont::DemiBold);
        p.setFont(font);
        p.setPen(pressed ? QColor(8, 30, 37) : textColor());
        p.drawText(QRectF(x - r, y - r, 2 * r, 2 * r), Qt::AlignCenter, label);
    }
}

inline void pill(QPainter& p, const QRectF& rect, const QString& label, bool pressed) {
    p.setPen(QPen(pressed ? QColor(220, 255, 255) : rim(), 1));
    p.setBrush(pressed ? active() : idle());
    p.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);
    QFont font = p.font();
    font.setPixelSize(8);
    font.setWeight(QFont::DemiBold);
    p.setFont(font);
    p.setPen(pressed ? QColor(8, 30, 37) : textColor());
    p.drawText(rect, Qt::AlignCenter, label);
}

inline void dpad(QPainter& p, const InputDisplaySlot& slot, qreal x, qreal y) {
    const QRectF arms[] = {
        {x - 6, y - 20, 12, 14}, {x - 6, y + 6, 12, 14},
        {x - 20, y - 6, 14, 12}, {x + 6, y - 6, 14, 12}
    };
    const unsigned bits[] = {4, 5, 6, 7};
    for (int i = 0; i < 4; ++i) {
        const bool pressed = down(slot, bits[i]);
        p.setPen(QPen(pressed ? QColor(220, 255, 255) : rim(), 0.8));
        p.setBrush(pressed ? active() : idle());
        p.drawRoundedRect(arms[i], 2, 2);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(idle());
    p.drawRect(QRectF(x - 6, y - 6, 12, 12));
    p.setBrush(QColor(22, 30, 40));
    p.drawEllipse(QPointF(x, y), 2.4, 2.4);
}

inline void mdBody(QPainter& p, bool six) {
    p.setPen(QPen(rim(), 1.2));
    p.setBrush(QColor(27, 35, 47, 242));
    QPainterPath path;
    if (six) {
        path.moveTo(16, 37);
        path.cubicTo(23, 23, 44, 25, 57, 28);
        path.cubicTo(75, 32, 92, 28, 109, 26);
        path.cubicTo(129, 22, 141, 34, 143, 49);
        path.cubicTo(146, 66, 133, 81, 120, 82);
        path.cubicTo(105, 83, 94, 74, 74, 74);
        path.cubicTo(53, 74, 42, 83, 27, 82);
        path.cubicTo(9, 80, 3, 56, 16, 37);
    } else {
        path.moveTo(19, 29);
        path.cubicTo(38, 16, 108, 16, 130, 29);
        path.cubicTo(148, 40, 146, 70, 129, 78);
        path.cubicTo(112, 85, 99, 75, 75, 75);
        path.cubicTo(51, 75, 37, 85, 21, 78);
        path.cubicTo(3, 69, 2, 40, 19, 29);
    }
    path.closeSubpath();
    p.drawPath(path);
}

inline void mdPad(QPainter& p, const InputDisplaySlot& s, bool six) {
    mdBody(p, six);
    dpad(p, s, 35, 54);
    const qreal bottom = six ? 67 : 59;
    const qreal top = 45;
    disc(p, 92, bottom, 8.5, QStringLiteral("A"), down(s, 1));
    disc(p, 112, bottom, 8.5, QStringLiteral("B"), down(s, 0));
    disc(p, 132, bottom, 8.5, QStringLiteral("C"), down(s, 8));
    if (six) {
        disc(p, 92, top, 7.2, QStringLiteral("X"), down(s, 10));
        disc(p, 112, top, 7.2, QStringLiteral("Y"), down(s, 9));
        disc(p, 132, top, 7.2, QStringLiteral("Z"), down(s, 11));
        pill(p, QRectF(54, 66, 28, 10), QStringLiteral("MODE"), down(s, 2));
    } else {
        pill(p, QRectF(62, 38, 29, 11), QStringLiteral("START"), down(s, 3));
    }
    if (six) pill(p, QRectF(54, 51, 28, 10), QStringLiteral("START"), down(s, 3));
}

inline void smsPad(QPainter& p, const InputDisplaySlot& s) {
    p.setPen(QPen(rim(), 1.2));
    p.setBrush(QColor(26, 35, 46, 245));
    p.drawRoundedRect(QRectF(9, 32, 132, 49), 7, 7);
    // Pause is on the Master System console, separate from the two-button pad.
    p.setBrush(QColor(32, 44, 58, 245));
    p.drawRoundedRect(QRectF(61, 20, 48, 13), 2, 2);
    pill(p, QRectF(70, 21, 30, 11), QStringLiteral("PAUSE"), down(s, 3));
    dpad(p, s, 36, 57);
    disc(p, 101, 59, 10, QStringLiteral("1"), down(s, 0));
    disc(p, 127, 59, 10, QStringLiteral("2"), down(s, 8));
}

inline void sgPad(QPainter& p, const InputDisplaySlot& s) {
    p.setPen(QPen(QColor(185, 194, 193), 1.1));
    p.setBrush(QColor(129, 139, 140, 245));
    p.drawRoundedRect(QRectF(10, 26, 130, 55), 4, 4);
    p.setPen(QPen(QColor(42, 48, 49), 1));
    p.setBrush(QColor(34, 40, 43));
    p.drawEllipse(QPointF(44, 55), 21, 21);
    const qreal dx = (down(s, 7) ? 7.0 : 0.0) - (down(s, 6) ? 7.0 : 0.0);
    const qreal dy = (down(s, 5) ? 7.0 : 0.0) - (down(s, 4) ? 7.0 : 0.0);
    p.setPen(QPen(QColor(208, 218, 216), 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(44, 56), QPointF(44 + dx, 47 + dy));
    p.setPen(QPen((dx || dy) ? QColor(214, 255, 252) : QColor(157, 172, 174), 1));
    p.setBrush((dx || dy) ? active() : QColor(25, 31, 34));
    p.drawEllipse(QPointF(44 + dx, 45 + dy), 8, 8);
    p.setPen(QColor(76, 29, 35));
    p.setBrush(QColor(113, 40, 47));
    p.drawRoundedRect(QRectF(77, 39, 53, 34), 5, 5);
    disc(p, 91, 56, 9, QStringLiteral("1"), down(s, 0));
    disc(p, 116, 56, 9, QStringLiteral("2"), down(s, 8));
}

inline void gear(QPainter& p, const InputDisplaySlot& s) {
    p.setPen(QPen(rim(), 1.2));
    p.setBrush(QColor(28, 40, 56, 245));
    p.drawRoundedRect(QRectF(7, 24, 136, 59), 22, 22);
    p.setBrush(QColor(13, 24, 34));
    p.drawRoundedRect(QRectF(55, 30, 43, 34), 4, 4);
    p.setPen(QColor(92, 113, 132));
    p.drawRoundedRect(QRectF(60, 34, 33, 25), 2, 2);
    dpad(p, s, 35, 55);
    disc(p, 111, 49, 8, QStringLiteral("1"), down(s, 0));
    disc(p, 127, 66, 8, QStringLiteral("2"), down(s, 8));
    pill(p, QRectF(65, 68, 24, 10), QStringLiteral("START"), down(s, 3));
}

inline void gun(QPainter& p, const InputDisplaySlot& s, InputDisplayDevice device) {
    p.setPen(QPen(rim(), 1.1));
    QPainterPath shape;
    if (device == InputDisplayDevice::LightPhaser) {
        p.setBrush(QColor(30, 35, 42, 245));
        shape.moveTo(17, 36); shape.lineTo(109, 32); shape.lineTo(131, 38);
        shape.lineTo(135, 46); shape.lineTo(75, 49); shape.lineTo(65, 56);
        shape.lineTo(57, 79); shape.lineTo(37, 79); shape.lineTo(43, 52);
        shape.lineTo(18, 49);
    } else if (device == InputDisplayDevice::Menacer) {
        p.setBrush(QColor(104, 113, 119, 245));
        shape.moveTo(14, 36); shape.lineTo(31, 29); shape.lineTo(95, 29);
        shape.lineTo(101, 24); shape.lineTo(121, 24); shape.lineTo(121, 31);
        shape.lineTo(136, 37); shape.lineTo(136, 46); shape.lineTo(76, 48);
        shape.lineTo(67, 55); shape.lineTo(62, 78); shape.lineTo(39, 78);
        shape.lineTo(41, 53); shape.lineTo(16, 51);
    } else {
        p.setBrush(QColor(31, 88, 163, 245));
        shape.moveTo(15, 39); shape.lineTo(95, 33); shape.lineTo(111, 33);
        shape.lineTo(128, 39); shape.lineTo(136, 41); shape.lineTo(136, 48);
        shape.lineTo(91, 49); shape.lineTo(79, 57); shape.lineTo(68, 77);
        shape.lineTo(45, 79); shape.lineTo(46, 56); shape.lineTo(18, 52);
    }
    shape.closeSubpath();
    p.drawPath(shape);
    p.setBrush(device == InputDisplayDevice::Justifier ? QColor(193, 208, 220)
                                                     : QColor(69, 78, 84));
    p.drawRect(QRectF(123, 38, 12, 8));
    if (device == InputDisplayDevice::Justifier) {
        p.setPen(QPen(QColor(202, 218, 231), 1));
        p.setBrush(QColor(27, 66, 119));
        p.drawEllipse(QPointF(80, 46), 11, 10); // Revolver cylinder.
    } else if (device == InputDisplayDevice::Menacer) {
        p.setPen(QPen(QColor(51, 60, 65), 2));
        p.drawLine(QPointF(89, 29), QPointF(113, 29)); // Sight module.
    }
    // The core treats Reload as an offscreen trigger press.
    disc(p, 63, 54, 6, QStringLiteral("T"), down(s, 1) || down(s, 2));
    if (device == InputDisplayDevice::Menacer) {
        disc(p, 92, 40, 6, QStringLiteral("2"), down(s, 0));
        disc(p, 108, 40, 6, QStringLiteral("3"), down(s, 8));
        pill(p, QRectF(70, 62, 31, 10), QStringLiteral("START"), down(s, 3));
    } else if (device == InputDisplayDevice::Justifier) {
        pill(p, QRectF(70, 62, 31, 10), QStringLiteral("START"), down(s, 3));
    }
    pill(p, QRectF(108, 62, 34, 11), QStringLiteral("RELOAD"), down(s, 2));
    if (s.offscreen) {
        p.setPen(QColor(255, 185, 103));
        QFont font = p.font();
        font.setPixelSize(8);
        font.setWeight(QFont::Bold);
        p.setFont(font);
        p.drawText(QRectF(99, 76, 38, 8), Qt::AlignCenter, QStringLiteral("OFF"));
    }
}

inline void mouse(QPainter& p, const InputDisplaySlot& s) {
    p.setPen(QPen(rim(), 1.2));
    p.setBrush(QColor(32, 43, 57, 245));
    p.drawRoundedRect(QRectF(43, 24, 64, 59), 28, 27);
    p.setPen(QColor(110, 128, 143));
    p.drawLine(QPointF(75, 26), QPointF(75, 50));
    // Show the emulated Mega Mouse's own A/B/C/Start control names.
    disc(p, 59, 42, 9, QStringLiteral("A"), down(s, 1));
    disc(p, 91, 42, 9, QStringLiteral("B"), down(s, 0));
    disc(p, 75, 62, 8, QStringLiteral("C"), down(s, 8));
    const int dx = qBound(-1, int(s.x), 1);
    const int dy = qBound(-1, int(s.y), 1);
    const QPointF center(75, 77);
    p.setPen(QPen(rim(), 0.8));
    p.setBrush(QColor(12, 24, 35));
    p.drawEllipse(center, 5, 5);
    if (dx || dy) {
        const qreal length = std::sqrt(qreal(dx * dx + dy * dy));
        const QPointF tip(center.x() + 4.0 * dx / length,
                          center.y() + 4.0 * dy / length);
        const QPointF flank(-dy / length, dx / length);
        QPainterPath arrow;
        arrow.moveTo(tip);
        arrow.lineTo(center + flank * 2.1);
        arrow.lineTo(center - flank * 2.1);
        arrow.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(active());
        p.drawPath(arrow);
    }
    pill(p, QRectF(109, 60, 32, 11), QStringLiteral("START"), down(s, 3));
}

inline void paddle(QPainter& p, const InputDisplaySlot& s) {
    p.setPen(QPen(rim(), 1.2));
    p.setBrush(QColor(35, 43, 52, 245));
    p.drawRoundedRect(QRectF(10, 26, 130, 55), 6, 6);
    p.setPen(QPen(rim(), 1.2));
    p.setBrush(QColor(18, 29, 42));
    p.drawEllipse(QPointF(61, 54), 24, 24);
    p.setPen(QPen(active(), 2.3, Qt::SolidLine, Qt::RoundCap));
    const qreal fraction = qBound<qreal>(0, (qreal(s.x) + 32768.0) / 65535.0, 1);
    const qreal angle = (225.0 - 270.0 * fraction) * 3.141592653589793 / 180.0;
    p.drawLine(QPointF(61, 54), QPointF(61 + 18 * std::cos(angle), 54 - 18 * std::sin(angle)));
    disc(p, 119, 56, 11, QStringLiteral("BTN"), down(s, 0));
}

inline const QString& name(InputDisplayDevice device) {
    static const QString names[] = {
        QStringLiteral(""), QStringLiteral("MD 3"), QStringLiteral("MD 6"),
        QStringLiteral("MASTER SYSTEM"), QStringLiteral("GAME GEAR"),
        QStringLiteral("SG-1000"), QStringLiteral("LIGHT PHASER"),
        QStringLiteral("MENACER"), QStringLiteral("JUSTIFIER"),
        QStringLiteral("MD MOUSE"), QStringLiteral("PADDLE")
    };
    const unsigned index = unsigned(device);
    return names[index < sizeof(names) / sizeof(names[0]) ? index : 0];
}

inline void card(QPainter& p, const InputDisplaySlot& s, unsigned player) {
    p.setPen(QPen(QColor(113, 132, 151, 220), 1));
    p.setBrush(QColor(5, 11, 19, 222));
    p.drawRoundedRect(QRectF(0.5, 0.5, cardWidth - 1, cardHeight - 1), 6, 6);
    p.setPen(active());
    QFont font = p.font();
    font.setPixelSize(10);
    font.setWeight(QFont::Bold);
    p.setFont(font);
    p.drawText(QRectF(7, 3, 23, 15), Qt::AlignVCenter, QStringLiteral("P%1").arg(player + 1));
    p.setPen(QColor(214, 225, 234));
    font.setPixelSize(9);
    font.setWeight(QFont::DemiBold);
    p.setFont(font);
    p.drawText(QRectF(33, 3, 111, 15), Qt::AlignVCenter, name(s.device));
    switch (s.device) {
    case InputDisplayDevice::MegaDrive3: mdPad(p, s, false); break;
    case InputDisplayDevice::MegaDrive6: mdPad(p, s, true); break;
    case InputDisplayDevice::MasterSystem: smsPad(p, s); break;
    case InputDisplayDevice::SG1000: sgPad(p, s); break;
    case InputDisplayDevice::GameGear: gear(p, s); break;
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
    const qreal margin = qBound<qreal>(6, bounds.width() / 65.0, 16);
    const qreal available = bounds.width() - 2 * margin;
    const qreal rowWidth = count * detail::cardWidth + (count - 1) * detail::gap;
    const qreal scale = qMin<qreal>(1.0, available / rowWidth);
    if (scale <= 0) return {};
    return {{bounds.left() + margin,
             bounds.bottom() + 1 - margin - detail::cardHeight * scale},
            {rowWidth * scale, detail::cardHeight * scale}, scale};
}

inline void paintRow(QPainter& painter, const InputDisplayState& state, qreal scale) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.scale(scale, scale);
    unsigned drawn = 0;
    for (unsigned player = 0; player < state.players.size(); ++player) {
        const auto& slot = state.players[player];
        if (slot.device == InputDisplayDevice::None) continue;
        painter.save();
        painter.translate(drawn * (detail::cardWidth + detail::gap), 0);
        detail::card(painter, slot, player);
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
