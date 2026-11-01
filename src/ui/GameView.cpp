#include "GameView.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QtMath>
#include <algorithm>

namespace {
QStringList wrapOsd(const QString& text, const QFontMetrics& metrics,
                    int maxWidth, int maxLines) {
    QStringList lines;
    QString line;
    const QStringList words = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& word : words) {
        const QString candidate = line.isEmpty() ? word : line + QLatin1Char(' ') + word;
        if (metrics.horizontalAdvance(candidate) <= maxWidth) {
            line = candidate;
            continue;
        }
        if (!line.isEmpty()) {
            lines.append(line);
            line.clear();
        }
        if (lines.size() >= maxLines) break;
        if (metrics.horizontalAdvance(word) > maxWidth) {
            line = metrics.elidedText(word, Qt::ElideRight, maxWidth);
        } else {
            line = word;
        }
    }
    if (!line.isEmpty() && lines.size() < maxLines) lines.append(line);
    if (lines.isEmpty()) lines.append(metrics.elidedText(text, Qt::ElideRight, maxWidth));
    if (lines.size() == maxLines && !words.isEmpty()) {
        const QString joined = lines.join(QLatin1Char(' '));
        if (joined.size() < text.simplified().size())
            lines.last() = metrics.elidedText(lines.last() + QStringLiteral("…"),
                                              Qt::ElideRight, maxWidth);
    }
    return lines;
}
} // namespace

GameView::GameView(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(400, 280);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/osd.ttf"));
    if (fontId >= 0) {
        const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
        if (!families.isEmpty()) osdFamily_ = families.first();
    }
    if (osdFamily_.isEmpty())
        osdFamily_ = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    osdClock_.start();
    osdTimer_.setInterval(90);
    connect(&osdTimer_, &QTimer::timeout, this, &GameView::expireOsd);
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

void GameView::showOsd(const QString& message, int durationMs,
                       const QString& category) {
    if (!osdEnabled_ || message.trimmed().isEmpty()) return;
    const qint64 deadline = osdClock_.elapsed() + qBound(500, durationMs, 12000);
    for (int i = notices_.size() - 1; i >= 0; --i) {
        if ((!category.isEmpty() && notices_[i].category == category) ||
            (category.isEmpty() && notices_[i].text == message))
            notices_.removeAt(i);
    }
    notices_.append({message.left(220), category, deadline});
    while (notices_.size() > 1) notices_.removeFirst();
    if (!osdTimer_.isActive()) osdTimer_.start();
    update();
}

void GameView::setOsdEnabled(bool enabled) {
    osdEnabled_ = enabled;
    notices_.clear();
    osdTimer_.stop();
    update();
}

void GameView::setPausedStatus(bool paused) {
    if (paused_ == paused) return;
    paused_ = paused;
    update();
}

void GameView::setFastForwardStatus(bool enabled) {
    if (fastForward_ == enabled) return;
    fastForward_ = enabled;
    update();
}

void GameView::expireOsd() {
    const qint64 now = osdClock_.elapsed();
    const auto end = std::remove_if(notices_.begin(), notices_.end(),
                                    [now](const Notice& n) { return n.expiresAt <= now; });
    if (end != notices_.end()) {
        notices_.erase(end, notices_.end());
        update();
    }
    if (notices_.isEmpty()) osdTimer_.stop();
}

void GameView::paintOsd(QPainter& painter) {
    if (!osdEnabled_ || (notices_.isEmpty() && !paused_ && !fastForward_)) return;
    QFont osd(osdFamily_);
    osd.setPixelSize(height() >= 1080 ? 24 : height() >= 520 ? 16 : 8);
    osd.setStyleStrategy(QFont::NoAntialias);
    painter.setFont(osd);
    painter.setRenderHint(QPainter::TextAntialiasing, false);
    const QFontMetrics fm(osd);
    const int margin = qBound(6, width() / 65, 16);
    const int pad = qBound(5, height() / 95, 9);
    const int lineHeight = fm.height() + 2;
    const int maxTextWidth = qMax(60, width() - 2 * margin - 2 * pad);
    auto drawPanel = [&](const QStringList& lines, int x, int y, bool accent) {
        int textWidth = 0;
        for (const QString& line : lines)
            textWidth = qMax(textWidth, fm.horizontalAdvance(line));
        const int panelWidth = qMin(width() - 2 * margin, textWidth + 2 * pad);
        const int panelHeight = lines.size() * lineHeight + 2 * pad;
        const QRect box(x, y, panelWidth, panelHeight);
        painter.fillRect(box, QColor(0, 0, 0, 218));
        painter.setPen(accent ? QColor(126, 224, 233) : QColor(129, 142, 159));
        painter.drawRect(box.adjusted(0, 0, -1, -1));
        int baseline = y + pad + fm.ascent();
        for (const QString& line : lines) {
            painter.setPen(Qt::black);
            painter.drawText(x + pad + 1, baseline + 1, line);
            painter.setPen(QColor(244, 249, 250));
            painter.drawText(x + pad, baseline, line);
            baseline += lineHeight;
        }
        return panelHeight;
    };

    int y = margin;
    if (paused_)
        y += drawPanel({QStringLiteral("ПАУЗА")}, margin, y, true) + 4;
    if (fastForward_)
        y += drawPanel({QStringLiteral("УСКОРЕНИЕ ×4")}, margin, y, true) + 4;
    for (const Notice& notice : notices_) {
        const QStringList lines = wrapOsd(notice.text, fm, maxTextWidth, 2);
        const int panelHeight = lines.size() * lineHeight + 2 * pad;
        if (y + panelHeight > height() - margin) break;
        y += drawPanel(lines, margin, y, false) + 4;
    }
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
    } else {
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
    paintOsd(painter);
}
