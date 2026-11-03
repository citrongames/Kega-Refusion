#include "i18n/Localization.h"
#include "ControllerDiagram.h"

#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QtMath>

namespace {
constexpr int Up = 4;
constexpr int Down = 5;
constexpr int Left = 6;
constexpr int Right = 7;
constexpr qreal ImageWidth = 1536;
constexpr qreal ImageHeight = 1024;
const QColor accent(64, 180, 198);
const QColor selected(239, 184, 73);

QPainterPath ellipse(QPointF center, qreal rx, qreal ry) {
    QPainterPath path;
    path.addEllipse(QRectF(center.x() - rx, center.y() - ry, 2 * rx, 2 * ry));
    return path;
}

// Expanded click shapes share no area: bisectors divide nearby buttons while
// leaving their actual faces intact. A tiny gap also avoids boundary ties.
QPainterPath nearerHalfPlane(QPointF center, QPointF other) {
    const QPointF normal = (other - center) / QLineF(center, other).length();
    const QPointF tangent(-normal.y(), normal.x());
    const QPointF middle = (center + other) / 2 - normal;
    QPainterPath path;
    path.moveTo(middle + tangent * 4096);
    path.lineTo(middle - tangent * 4096);
    path.lineTo(middle - tangent * 4096 - normal * 8192);
    path.lineTo(middle + tangent * 4096 - normal * 8192);
    path.closeSubpath();
    return path;
}

QPainterPath diskSector(QPointF center, qreal angle) {
    constexpr qreal inner = 65, outer = 195, halfAngle = 30;
    const auto point = [center](qreal radius, qreal degrees) {
        const qreal radians = qDegreesToRadians(degrees);
        return center + QPointF(radius * qCos(radians), -radius * qSin(radians));
    };
    QPainterPath path;
    path.moveTo(point(inner, angle - halfAngle));
    path.lineTo(point(outer, angle - halfAngle));
    path.arcTo(QRectF(center.x() - outer, center.y() - outer, 2 * outer, 2 * outer),
               angle - halfAngle, 2 * halfAngle);
    path.lineTo(point(inner, angle + halfAngle));
    path.arcTo(QRectF(center.x() - inner, center.y() - inner, 2 * inner, 2 * inner),
               angle + halfAngle, -2 * halfAngle);
    path.closeSubpath();
    return path;
}
}

ControllerDiagram::ControllerDiagram(QWidget* parent) : QWidget(parent) {
    setMinimumSize(290, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::PointingHandCursor);
    updateIllustration();
}

QSize ControllerDiagram::sizeHint() const {
    return QSize(410, controllerType_ == ControllerType::Menacer ? 440 :
                      controllerType_ != ControllerType::Pad ? 360 : 300);
}

void ControllerDiagram::setProfile(ConsoleProfile profile) {
    if (profile_ == profile) {
        // ControlsPage also reapplies the profile when switching players.
        // Clear the previous player's indicators without reloading the image.
        if (selectedTarget_ != -1 || pressedMask_) {
            selectedTarget_ = -1;
            pressedMask_ = 0;
            update();
        }
        return;
    }
    profile_ = profile;
    selectedTarget_ = -1;
    pressedMask_ = 0;
    updateIllustration();
}

void ControllerDiagram::setSixButton(bool sixButton) {
    if (sixButton_ == sixButton)
        return;
    sixButton_ = sixButton;
    if (profile_ == ConsoleProfile::MegaDrive && controllerType_ == ControllerType::Pad)
        updateIllustration();
}

void ControllerDiagram::setControllerType(ControllerType type) {
    if (controllerType_ == type) return;
    controllerType_ = type;
    setMinimumHeight(type == ControllerType::Menacer ? 340 : type != ControllerType::Pad ? 260 : 220);
    updateGeometry();
    selectedTarget_ = -1;
    pressedMask_ = 0;
    updateIllustration();
}

void ControllerDiagram::updateIllustration() {
    QString path;
    switch (controllerType_) {
    case ControllerType::LightPhaser: path = QStringLiteral(":/controllers/phaser.png"); break;
    case ControllerType::Menacer: path = QStringLiteral(":/controllers/menacer.png"); break;
    case ControllerType::Justifier: path = QStringLiteral(":/controllers/justifier.png"); break;
    case ControllerType::Mouse: path = QStringLiteral(":/controllers/mouse.png"); break;
    case ControllerType::Paddle: path = QStringLiteral(":/controllers/paddle.png"); break;
    case ControllerType::Pad: break;
    }
    if (controllerType_ == ControllerType::Pad) switch (profile_) {
    case ConsoleProfile::MegaDrive:
        path = sixButton_ ? QStringLiteral(":/controllers/md6.png")
                          : QStringLiteral(":/controllers/md3.png");
        break;
    case ConsoleProfile::MasterSystem: path = QStringLiteral(":/controllers/sms.png"); break;
    case ConsoleProfile::GameGear: path = QStringLiteral(":/controllers/gg.png"); break;
    case ConsoleProfile::SG1000: path = QStringLiteral(":/controllers/sg1000.png"); break;
    }
    // Retain only the selected illustration. Painting and pressed-mask updates
    // never load a resource or allocate a resized copy of the bitmap.
    if (illustrationPath_ != path) {
        illustrationPath_ = path;
        illustration_.load(path);
    }
    rebuildTargets();
    update();
}

void ControllerDiagram::setTargets(const QVector<int>& targets) {
    QSet<int> next;
    for (int target : targets)
        next.insert(target);
    if (targets_ == next)
        return;
    targets_.swap(next);
    if (!targets_.contains(selectedTarget_))
        selectedTarget_ = -1;
    rebuildTargets();
    update();
}

void ControllerDiagram::setSelectedTarget(int target) {
    if (selectedTarget_ != target) {
        selectedTarget_ = target;
        update();
    }
}

void ControllerDiagram::setPressedMask(unsigned mask) {
    if (pressedMask_ != mask) {
        pressedMask_ = mask;
        update();
    }
}

void ControllerDiagram::addArea(int target, const QPainterPath& shape, qreal padding) {
    if (!targets_.contains(target))
        return;
    QPainterPathStroker stroke;
    stroke.setWidth(2 * padding);
    stroke.setJoinStyle(Qt::RoundJoin);
    stroke.setCapStyle(Qt::RoundCap);
    areas_.append({target, shape, shape.united(stroke.createStroke(shape))});
}

void ControllerDiagram::addCircle(int target, QPointF center, qreal radius) {
    addEllipse(target, center, radius, radius);
}

void ControllerDiagram::addEllipse(int target, QPointF center, qreal rx, qreal ry) {
    addArea(target, ellipse(center, rx, ry));
}

void ControllerDiagram::addDpad(QPointF center, qreal reach) {
    const qreal halfWidth = reach * 0.325;
    const QPair<int, QRectF> pieces[] = {
        {Up, QRectF(center.x() - halfWidth, center.y() - reach,
                    2 * halfWidth, reach - halfWidth)},
        {Down, QRectF(center.x() - halfWidth, center.y() + halfWidth,
                      2 * halfWidth, reach - halfWidth)},
        {Left, QRectF(center.x() - reach, center.y() - halfWidth,
                      reach - halfWidth, 2 * halfWidth)},
        {Right, QRectF(center.x() + halfWidth, center.y() - halfWidth,
                       reach - halfWidth, 2 * halfWidth)}
    };
    for (const auto& piece : pieces) {
        QPainterPath shape;
        shape.addRoundedRect(piece.second, reach * 0.06, reach * 0.06);
        addArea(piece.first, shape);
    }
}

void ControllerDiagram::rebuildTargets() {
    areas_.clear();
    // These regions follow the button faces in the supplied 1536x1024 rasters.
    // Other Menacer controls are shown separately below the side-view artwork.
    switch (controllerType_) {
    case ControllerType::LightPhaser: {
        QPainterPath trigger;
        trigger.addRoundedRect(QRectF(555, 446, 119, 122), 22, 22);
        addArea(1, trigger);
        break;
    }
    case ControllerType::Menacer: {
        QPainterPath trigger;
        trigger.moveTo(887, 450);
        trigger.quadTo(932, 456, 930, 486);
        trigger.quadTo(925, 523, 891, 538);
        trigger.lineTo(866, 523);
        trigger.quadTo(896, 499, 887, 450);
        trigger.closeSubpath();
        addArea(1, trigger);
        break;
    }
    case ControllerType::Justifier: {
        addCircle(3, QPointF(1152, 347), 35);
        QPainterPath trigger;
        trigger.addRoundedRect(QRectF(981, 500, 61, 145), 20, 20);
        addArea(1, trigger);
        break;
    }
    case ControllerType::Mouse:
        // Button faces in the 1536x1024 MD Mouse image.
        addEllipse(1, QPointF(598, 320), 65, 108);
        addEllipse(0, QPointF(768, 297), 66, 107);
        addEllipse(8, QPointF(937, 323), 66, 108);
        addEllipse(3, QPointF(765, 769), 78, 44);
        break;
    case ControllerType::Paddle:
        // Both physical buttons are electrically the same action.
        addEllipse(0, QPointF(1237, 674), 92, 94);
        addEllipse(0, QPointF(1261, 245), 90, 46);
        break;
    case ControllerType::Pad: break;
    }
    if (controllerType_ == ControllerType::Pad) switch (profile_) {
    case ConsoleProfile::MegaDrive:
        if (sixButton_) {
            addDpad(QPointF(355, 512), 150);
            addEllipse(3, QPointF(770, 418), 72, 25);
            addEllipse(2, QPointF(1230, 197), 52, 22);
            addCircle(10, QPointF(1029, 463), 59);
            addCircle(9, QPointF(1173, 423), 59);
            addCircle(11, QPointF(1318, 381), 59);
            addCircle(1, QPointF(1056, 621), 73);
            addCircle(0, QPointF(1220, 579), 73);
            addCircle(8, QPointF(1381, 540), 73);
        } else {
            addDpad(QPointF(390, 516), 112);
            QPainterPath start;
            start.addRoundedRect(QRectF(-70, -16, 140, 32), 16, 16);
            QTransform angle;
            angle.translate(1095, 327.5);
            angle.rotate(-18);
            addArea(3, angle.map(start));
            addCircle(1, QPointF(1023, 615), 70);
            addCircle(0, QPointF(1172, 537), 68);
            addCircle(8, QPointF(1324, 479), 66);
        }
        break;
    case ConsoleProfile::MasterSystem:
        addDpad(QPointF(322, 525), 145);
        addCircle(0, QPointF(1045, 575), 80);
        addCircle(8, QPointF(1295, 575), 80);
        break;
    case ConsoleProfile::GameGear:
        addDpad(QPointF(211, 485), 100);
        addEllipse(3, QPointF(1317, 355), 45, 20);
        addCircle(0, QPointF(1265, 525), 51);
        addCircle(8, QPointF(1399, 460), 51);
        break;
    case ConsoleProfile::SG1000: {
        const QPointF center(394, 523);
        addArea(Up, diskSector(center, 90));
        addArea(Down, diskSector(center, 270));
        addArea(Left, diskSector(center, 180));
        addArea(Right, diskSector(center, 0));
        QPainterPath first, second;
        first.addRoundedRect(QRectF(947, 498, 148, 222), 74, 74);
        second.addRoundedRect(QRectF(1206, 498, 148, 222), 74, 74);
        addArea(0, first);
        addArea(8, second);
        break;
    }
    }
    for (qsizetype i = 0; i < areas_.size(); ++i) {
        for (qsizetype j = i + 1; j < areas_.size(); ++j) {
            auto& first = areas_[i];
            auto& second = areas_[j];
            if (!first.hit.boundingRect().intersects(second.hit.boundingRect()))
                continue;
            const QPointF a = first.shape.boundingRect().center();
            const QPointF b = second.shape.boundingRect().center();
            if (QLineF(a, b).length() < 1)
                continue;
            first.hit = first.hit.intersected(nearerHalfPlane(a, b));
            second.hit = second.hit.intersected(nearerHalfPlane(b, a));
        }
    }
}

QRectF ControllerDiagram::illustrationArea() const {
    const qreal footer = controllerType_ == ControllerType::Menacer ? 138 :
        (controllerType_ != ControllerType::Pad || profile_ == ConsoleProfile::MasterSystem ? 72 : 28);
    return QRectF(8, 34, qMax(1, width() - 16), qMax(qreal(1), height() - 34 - footer));
}

QRectF ControllerDiagram::consolePauseArea() const {
    return QRectF((width() - 196) / 2.0, height() - 66, 196, 32);
}

QRectF ControllerDiagram::extraButtonsArea() const {
    const qreal panelWidth = qMin(qreal(320), qreal(width() - 16));
    return QRectF((width() - panelWidth) / 2, height() - 111, panelWidth, 42);
}

QRectF ControllerDiagram::extraButtonArea(int target) const {
    const int index = target == 0 ? 0 : target == 8 ? 1 : target == 3 ? 2 : -1;
    if (index < 0 || !targets_.contains(target)) return {};
    const QRectF panel = extraButtonsArea();
    const qreal buttonWidth = (panel.width() - 16) / 3;
    return QRectF(panel.left() + 4 + index * (buttonWidth + 4), panel.top() + 4,
                  buttonWidth, panel.height() - 8);
}

QRectF ControllerDiagram::reloadArea() const {
    if (controllerType_ == ControllerType::Pad || !targets_.contains(2)) return {};
    const qreal buttonWidth = qMin(qreal(220), qreal(width() - 16));
    return QRectF((width() - buttonWidth) / 2, height() - 63, buttonWidth, 32);
}

QTransform ControllerDiagram::drawingTransform() const {
    const QRectF area = illustrationArea();
    const qreal scale = qMin(area.width() / ImageWidth, area.height() / ImageHeight);
    QTransform transform;
    transform.translate(area.center().x() - ImageWidth * scale / 2,
                        area.center().y() - ImageHeight * scale / 2);
    transform.scale(scale, scale);
    return transform;
}

void ControllerDiagram::drawHighlight(QPainter& painter, int target,
                                      const QPainterPath& shape) const {
    const bool pressed = target >= 0 && target < 16 && (pressedMask_ & (1u << target));
    if (pressed) {
        QColor fill = accent;
        fill.setAlpha(60);
        QPen pen(accent, 2);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.setBrush(fill);
        painter.drawPath(shape);
    }
    if (target == selectedTarget_) {
        QColor fill = selected;
        fill.setAlpha(pressed ? 0 : 38);
        QPen pen(selected, 2.5);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.setBrush(fill);
        painter.drawPath(shape);
    }
}

void ControllerDiagram::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), palette().color(QPalette::Base));

    QFont title = painter.font();
    title.setPointSizeF(11);
    title.setBold(true);
    painter.setFont(title);
    painter.setPen(palette().color(QPalette::Text));
    QString name = InputManager::profileName(profile_);
    switch (controllerType_) {
    case ControllerType::Pad:
        if (profile_ == ConsoleProfile::MegaDrive)
            name += sixButton_ ? Localization::text(QStringLiteral(" · 6 кнопок")) : Localization::text(QStringLiteral(" · 3 кнопки"));
        break;
    case ControllerType::LightPhaser: name += QStringLiteral(" · Light Phaser"); break;
    case ControllerType::Menacer: name += QStringLiteral(" · Menacer"); break;
    case ControllerType::Justifier: name += QStringLiteral(" · Justifier"); break;
    case ControllerType::Mouse: name += QStringLiteral(" · MD Mouse"); break;
    case ControllerType::Paddle: name += QStringLiteral(" · SMS Paddle"); break;
    }
    painter.drawText(QRectF(8, 4, width() - 16, 26), Qt::AlignCenter, name);

    if (illustration_.isNull()) {
        painter.drawText(illustrationArea(), Qt::AlignCenter,
                         Localization::text(QStringLiteral("Изображение недоступно")));
    } else {
        painter.save();
        painter.setTransform(drawingTransform());
        painter.drawPixmap(QRectF(0, 0, ImageWidth, ImageHeight), illustration_,
                           QRectF(illustration_.rect()));
        for (const auto& area : areas_)
            drawHighlight(painter, area.target, area.shape);
        // SJ-150's image has no printed numbers. These are mapping hints below
        // the controller, rather than invented markings on its button faces.
        if (profile_ == ConsoleProfile::SG1000 && controllerType_ == ControllerType::Pad) {
            QFont labels = painter.font();
            labels.setPixelSize(64);
            labels.setBold(true);
            painter.setFont(labels);
            painter.setPen(palette().color(QPalette::Text));
            if (targets_.contains(0))
                painter.drawText(QRectF(968, 826, 106, 76), Qt::AlignCenter, QStringLiteral("1"));
            if (targets_.contains(8))
                painter.drawText(QRectF(1227, 826, 106, 76), Qt::AlignCenter, QStringLiteral("2"));
        }
        painter.restore();
    }

    if (profile_ == ConsoleProfile::MasterSystem && controllerType_ == ControllerType::Pad && targets_.contains(3)) {
        const QRectF panel = consolePauseArea();
        const QRectF button(panel.right() - 87, panel.top() + 3, 83, panel.height() - 6);
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.setBrush(palette().color(QPalette::AlternateBase));
        painter.drawRoundedRect(panel, 6, 6);
        QFont font = painter.font();
        font.setPointSizeF(9);
        font.setBold(false);
        painter.setFont(font);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRectF(panel.left() + 9, panel.top(), 100, panel.height()),
                         Qt::AlignVCenter, Localization::text(QStringLiteral("Пауза консоли")));
        painter.setBrush(palette().color(QPalette::Button));
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.drawRoundedRect(button, 5, 5);
        QPainterPath outline;
        outline.addRoundedRect(button, 5, 5);
        drawHighlight(painter, 3, outline);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(palette().color(QPalette::ButtonText));
        painter.drawText(button, Qt::AlignCenter, QStringLiteral("PAUSE"));
    }

    if (controllerType_ == ControllerType::Menacer) {
        const QRectF panel = extraButtonsArea();
        QFont font = painter.font();
        font.setPointSizeF(9);
        font.setBold(false);
        painter.setFont(font);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRectF(panel.left(), panel.top() - 22, panel.width(), 18),
                         Qt::AlignCenter, Localization::text(QStringLiteral("Остальные кнопки")));
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.setBrush(palette().color(QPalette::AlternateBase));
        painter.drawRoundedRect(panel, 6, 6);
        for (int target : {0, 8, 3}) {
            const QRectF button = extraButtonArea(target);
            if (button.isEmpty()) continue;
            painter.setBrush(palette().color(QPalette::Button));
            painter.setPen(QPen(palette().color(QPalette::Mid), 1));
            painter.drawRoundedRect(button, 5, 5);
            QPainterPath outline;
            outline.addRoundedRect(button, 5, 5);
            drawHighlight(painter, target, outline);
            painter.setPen(palette().color(QPalette::ButtonText));
            painter.drawText(button, Qt::AlignCenter,
                             InputManager::targetName(profile_, target, controllerType_));
        }
    }

    const QRectF reload = reloadArea();
    if (!reload.isEmpty()) {
        // Reload is a frontend action (off-screen shot), not a button invented
        // on the physical gun illustration.
        QFont font = painter.font();
        font.setPointSizeF(9);
        font.setBold(false);
        painter.setFont(font);
        painter.setBrush(palette().color(QPalette::Button));
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.drawRoundedRect(reload, 5, 5);
        QPainterPath outline;
        outline.addRoundedRect(reload, 5, 5);
        drawHighlight(painter, 2, outline);
        painter.setPen(palette().color(QPalette::ButtonText));
        painter.drawText(reload, Qt::AlignCenter,
                         InputManager::targetName(profile_, 2, controllerType_));
    }

    QColor hintColor = palette().color(QPalette::Text);
    hintColor.setAlpha(165);
    painter.setPen(hintColor);
    QFont hint = painter.font();
    hint.setPointSizeF(9);
    hint.setBold(false);
    painter.setFont(hint);
    painter.drawText(QRect(8, height() - 24, width() - 16, 18), Qt::AlignCenter,
                     controllerType_ == ControllerType::Pad || !areas_.isEmpty()
                         ? Localization::text(QStringLiteral("Нажмите кнопку на рисунке для выбора"))
                         : Localization::text(QStringLiteral("Выберите кнопку в таблице")));
}

void ControllerDiagram::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (profile_ == ConsoleProfile::MasterSystem && controllerType_ == ControllerType::Pad && targets_.contains(3) &&
        consolePauseArea().contains(event->position())) {
        emit targetClicked(3);
        event->accept();
        return;
    }
    if (reloadArea().contains(event->position())) {
        emit targetClicked(2);
        event->accept();
        return;
    }
    if (controllerType_ == ControllerType::Menacer) {
        for (int target : {0, 8, 3}) {
            if (extraButtonArea(target).contains(event->position())) {
                emit targetClicked(target);
                event->accept();
                return;
            }
        }
    }
    bool invertible = false;
    const QPointF point = drawingTransform().inverted(&invertible).map(event->position());
    if (invertible && !illustration_.isNull()) {
        for (const auto& area : areas_) {
            if (area.hit.contains(point)) {
                emit targetClicked(area.target);
                event->accept();
                return;
            }
        }
    }
    QWidget::mousePressEvent(event);
}
