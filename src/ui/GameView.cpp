#include "GameView.h"

#include <QFontMetrics>
#include <QPainter>
#include <QtMath>

GameView::GameView(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(400, 280);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void GameView::setFrame(const QImage& frame, double aspectRatio) {
    frame_ = frame;
    if (qIsFinite(aspectRatio) && aspectRatio > 0.25 && aspectRatio < 5.0)
        aspectRatio_ = aspectRatio;
    update();
}

void GameView::setAspectMode(AspectMode mode) {
    aspectMode_ = mode;
    update();
}

void GameView::setIntegerScaling(bool enabled) {
    integerScaling_ = enabled;
    update();
}

void GameView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(17, 20, 27));

    if (frame_.isNull()) {
        painter.setRenderHint(QPainter::Antialiasing);
        const QPoint center = rect().center();
        const QColor accent(116, 190, 205);
        QPen pen(accent, 3);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const QRect emblem(center.x() - 27, center.y() - 86, 54, 54);
        painter.drawRoundedRect(emblem, 11, 11);
        painter.drawLine(emblem.left() + 12, emblem.center().y(),
                         emblem.right() - 12, emblem.center().y());
        painter.drawLine(emblem.center().x(), emblem.top() + 12,
                         emblem.center().x(), emblem.bottom() - 12);

        painter.setPen(QColor(239, 242, 247));
        QFont title = font();
        title.setPointSize(qMax(16, title.pointSize() + 7));
        title.setWeight(QFont::DemiBold);
        painter.setFont(title);
        painter.drawText(QRect(20, center.y() - 17, width() - 40, 34),
                         Qt::AlignCenter, QStringLiteral("Kega Refusion"));

        painter.setPen(QColor(166, 174, 189));
        QFont detail = font();
        detail.setPointSize(qMax(9, detail.pointSize()));
        painter.setFont(detail);
        painter.drawText(QRect(24, center.y() + 24, width() - 48, 26),
                         Qt::AlignCenter,
                         QStringLiteral("Ctrl+O — открыть игру · или перетащите ROM сюда"));
        painter.drawText(QRect(24, center.y() + 51, width() - 48, 26),
                         Qt::AlignCenter,
                         QStringLiteral("Поддержка: MD, GEN, BIN, SMD, SMS, GG, SG, 68K"));
        return;
    }

    const double desiredAspect =
        aspectMode_ == AspectMode::Stretch ? double(width()) / qMax(1, height()) :
        aspectMode_ == AspectMode::SquarePixels
            ? double(frame_.width()) / qMax(1, frame_.height()) : aspectRatio_;

    double targetWidth = width();
    double targetHeight = targetWidth / desiredAspect;
    if (targetHeight > height()) {
        targetHeight = height();
        targetWidth = targetHeight * desiredAspect;
    }

    if (integerScaling_ && aspectMode_ != AspectMode::Stretch) {
        const double baseWidth = frame_.height() * desiredAspect;
        const double baseHeight = frame_.height();
        const int factor = qFloor(qMin(double(width()) / baseWidth,
                                       double(height()) / baseHeight));
        if (factor >= 1) {
            targetWidth = baseWidth * factor;
            targetHeight = baseHeight * factor;
        }
    }

    const QRectF target((width() - targetWidth) / 2.0,
                        (height() - targetHeight) / 2.0,
                        targetWidth, targetHeight);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(target, frame_);
}
