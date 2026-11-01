#include "GameView.h"
#include "../core/VideoFrameGeometry.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QGuiApplication>
#include <QOpenGLWidget>
#include <QOpenGLContext>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QShowEvent>
#include <QSurfaceFormat>
#include <memory>
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

// Kept as a child so the raster path remains usable on offscreen platforms and
// machines without desktop OpenGL. Only emulated video enters the shader chain.
class ShaderView final : public QOpenGLWidget, protected QOpenGLFunctions {
public:
    explicit ShaderView(GameView* owner) : QOpenGLWidget(owner), owner_(owner) {
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(2, 1);
        format.setProfile(QSurfaceFormat::NoProfile);
        format.setSwapInterval(1);
        format.setDepthBufferSize(0);
        format.setStencilBufferSize(0);
        setFormat(format);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setObjectName(QStringLiteral("gpuVideo"));
    }
    ~ShaderView() override {
        if (context()) {
            disconnect(context(), nullptr, this, nullptr);
            makeCurrent();
            pipeline_.reset();
            doneCurrent();
        }
    }
    bool load(const ShaderPreset& preset, QString& error) {
        if (!initialized_) return true; // initializeGL reads the owner's pending preset.
        if (!context() || !context()->isValid())
            return false;
        makeCurrent();
        auto next = std::make_unique<ShaderPipeline>();
        const bool okay = next->initialize(preset, error);
        if (okay) pipeline_ = std::move(next);
        next.reset();
        doneCurrent();
        if (okay) update();
        return okay;
    }
    void parameter(const QString& id, float value) {
        if (pipeline_) pipeline_->setParameter(id, value);
        update();
    }
protected:
    void initializeGL() override {
        initialized_ = true;
        initializeOpenGLFunctions();
        connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
            makeCurrent();
            pipeline_.reset();
            initialized_ = false;
            doneCurrent();
        }, Qt::DirectConnection);
        if (context()->isOpenGLES()) {
            failed(QStringLiteral("Desktop OpenGL 2.1 or newer is required for GLSL video shaders."));
            return;
        }
        QString error;
        auto next = std::make_unique<ShaderPipeline>();
        if (!next->initialize(owner_->shaderPreset_, error)) {
            next.reset();
            failed(error);
            return;
        }
        pipeline_ = std::move(next);
    }
    void paintGL() override {
        if (owner_->frame_.isNull() || !pipeline_) {
            QPainter painter(this);
            owner_->paintRaster(painter);
            owner_->paintOsd(painter);
            owner_->paintLoading(painter);
            return;
        }
        const qreal dpr = devicePixelRatioF();
        const QRectF logical = owner_->gameRect();
        // Round edges, rather than independently rounding position and dimensions.
        const QRect target(QPoint(qRound(logical.left() * dpr), qRound(logical.top() * dpr)),
                           QPoint(qRound((logical.left() + logical.width()) * dpr) - 1,
                                  qRound((logical.top() + logical.height()) * dpr) - 1));
        // Invalidate Qt's cached GL state around native drawing; otherwise its
        // font atlas can be mistaken for the video texture on subsequent paints.
        QPainter painter(this);
        painter.beginNativePainting();
        QString error;
        if (!pipeline_->render(owner_->frame_, defaultFramebufferObject(),
                               QSize(qRound(width() * dpr), qRound(height() * dpr)),
                               target, owner_->frameCount_, owner_->rewinding_ ? -1 : 1, error)) {
            failed(error);
        }
        painter.endNativePainting();
        owner_->paintOsd(painter); // Native-resolution OSD is never filtered.
        owner_->paintLoading(painter);
    }
private:
    void failed(const QString& error) {
        if (failurePending_) return;
        failurePending_ = true;
        QTimer::singleShot(0, owner_, [owner = owner_, error] { owner->gpuFailed(error); });
    }
    GameView* owner_;
    std::unique_ptr<ShaderPipeline> pipeline_;
    bool initialized_ = false;
    bool failurePending_ = false;
};

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

QImage GameView::captureImage() {
    // QWidget::grab does not include a composited QOpenGLWidget on every driver.
    if (shaderView_ && shaderView_->isVisible() && shaderView_->isValid())
        return shaderView_->grabFramebuffer();
    return grab().toImage();
}

QImage GameView::captureGameImage() {
    if (frame_.isNull() || width() <= 0 || height() <= 0) return {};
    QImage image;
    {
        // Grab the rendered shader/raster output without changing OSD settings,
        // notices or persistent status. The next presentation restores overlays.
        const QScopedValueRollback<bool> capture(capturingGame_, true);
        image = captureImage();
    }
    repaintView();
    if (image.isNull()) return {};
    const QRectF game = gameRect();
    const qreal scaleX = qreal(image.width()) / width();
    const qreal scaleY = qreal(image.height()) / height();
    const QRect crop(QPoint(qRound(game.left() * scaleX), qRound(game.top() * scaleY)),
                     QPoint(qRound((game.left() + game.width()) * scaleX) - 1,
                            qRound((game.top() + game.height()) * scaleY) - 1));
    image = image.copy(crop.intersected(image.rect()));
    image.setDevicePixelRatio(1.0);
    return image;
}

void GameView::setFrame(const QImage& frame, double aspectRatio) {
    frame_ = frame;
    ++frameCount_;
    if (qIsFinite(aspectRatio) && aspectRatio > 0.25 && aspectRatio < 5.0)
        aspectRatio_ = aspectRatio;
    repaintView();
}

void GameView::setLoading(bool loading, const QString& fileName) {
    const QString name = loading ? fileName.left(220) : QString();
    if (loading_ == loading && loadingFileName_ == name) return;
    loading_ = loading;
    loadingFileName_ = name;
    repaintView();
    if (loading && isVisible()) {
        // One immediate presentation also precedes a synchronous archive scan.
        // Do not process input/events recursively or start an animation timer.
        if (shaderView_ && shaderView_->isVisible()) shaderView_->repaint();
        QWidget::repaint();
    }
}

void GameView::setAspectMode(AspectMode mode) {
    aspectMode_ = mode;
    repaintView();
}

void GameView::setIntegerScaling(bool enabled) {
    integerScaling_ = enabled;
    repaintView();
}

void GameView::showOsd(const QString& message, int durationMs,
                       const QString& category) {
    if (!osdEnabled_ || message.trimmed().isEmpty()) return;
    stateSlots_.expiresAt = 0;
    const qint64 deadline = osdClock_.elapsed() + qBound(500, durationMs, 12000);
    for (int i = notices_.size() - 1; i >= 0; --i) {
        if ((!category.isEmpty() && notices_[i].category == category) ||
            (category.isEmpty() && notices_[i].text == message))
            notices_.removeAt(i);
    }
    notices_.append({message.left(220), category, deadline});
    while (notices_.size() > 1) notices_.removeFirst();
    if (!osdTimer_.isActive()) osdTimer_.start();
    repaintView();
}

void GameView::showStateSlots(int selected, unsigned occupiedMask, int durationMs) {
    if (!osdEnabled_) return;
    notices_.clear();
    stateSlots_ = {qBound(0, selected, 9), occupiedMask & 0x3ffU,
                   osdClock_.elapsed() + qBound(500, durationMs, 12000)};
    if (!osdTimer_.isActive()) osdTimer_.start();
    repaintView();
}

void GameView::setOsdEnabled(bool enabled) {
    osdEnabled_ = enabled;
    notices_.clear();
    stateSlots_.expiresAt = 0;
    osdTimer_.stop();
    repaintView();
}

void GameView::setMutedStatus(bool muted) {
    if (muted_ == muted) return;
    muted_ = muted;
    repaintView();
}

void GameView::setPausedStatus(bool paused) {
    if (paused_ == paused) return;
    paused_ = paused;
    repaintView();
}

void GameView::setFastForwardStatus(bool enabled) {
    if (fastForward_ == enabled) return;
    fastForward_ = enabled;
    repaintView();
}

void GameView::setFastForwardFactor(int factor) {
    factor = qMax(0, factor);
    if (fastForwardFactor_ == factor) return;
    fastForwardFactor_ = factor;
    if (fastForward_) repaintView();
}

void GameView::setRewindStatus(bool enabled) {
    if (rewinding_ == enabled) return;
    rewinding_ = enabled;
    repaintView();
}

void GameView::expireOsd() {
    const qint64 now = osdClock_.elapsed();
    const auto end = std::remove_if(notices_.begin(), notices_.end(),
                                    [now](const Notice& n) { return n.expiresAt <= now; });
    if (end != notices_.end()) {
        notices_.erase(end, notices_.end());
        repaintView();
    }
    if (stateSlots_.expiresAt && stateSlots_.expiresAt <= now) {
        stateSlots_.expiresAt = 0;
        repaintView();
    }
    if (notices_.isEmpty() && !stateSlots_.expiresAt) osdTimer_.stop();
}

void GameView::paintOsd(QPainter& painter) {
    if (capturingGame_ || !osdEnabled_ || (notices_.isEmpty() && !stateSlots_.expiresAt &&
                         !paused_ && !fastForward_ && !rewinding_ && !muted_)) return;
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
        y += drawPanel({fastForwardFactor_ ? QStringLiteral("УСКОРЕНИЕ ×%1").arg(fastForwardFactor_) : QStringLiteral("УСКОРЕНИЕ МАКС.")},
                       margin, y, true) + 4;
    if (rewinding_)
        y += drawPanel({QStringLiteral("ПЕРЕМОТКА НАЗАД")}, margin, y, true) + 4;
    if (muted_)
        y += drawPanel({QStringLiteral("ЗВУК ВЫКЛЮЧЕН")}, margin, y, true) + 4;
    if (stateSlots_.expiresAt) {
        // Ten compact cells, as in FCEUX: filled occupied slots and a selected outline.
        // Keep the row inside narrow windows even when their height requests a large font.
        QFont slotFont(osd);
        const int gap = qMax(2, pad / 2);
        const int available = width() - 2 * margin - 2 * pad;
        int cellWidth = 0;
        for (;;) {
            const QFontMetrics metrics(slotFont);
            cellWidth = metrics.horizontalAdvance(QLatin1Char('0')) + 2 * pad;
            if (10 * cellWidth + 9 * gap <= available || slotFont.pixelSize() <= 8) break;
            slotFont.setPixelSize(qMax(8, slotFont.pixelSize() - 8));
        }
        const QFontMetrics slotFm(slotFont);
        const int cellHeight = slotFm.height() + 2 * pad;
        const int labelHeight = slotFm.height() + 2;
        const int rowWidth = 10 * cellWidth + 9 * gap;
        const QString label = QStringLiteral("СЛОТ %1 · %2")
                                  .arg(stateSlots_.selected)
                                  .arg(stateSlots_.occupiedMask & (1U << stateSlots_.selected)
                                           ? QStringLiteral("ЗАНЯТ") : QStringLiteral("ПУСТ"));
        const int panelWidth = qMax(rowWidth, slotFm.horizontalAdvance(label)) + 2 * pad;
        const int panelHeight = labelHeight + cellHeight + 3 * pad;
        if (y + panelHeight <= height() - margin) {
            painter.save();
            painter.setFont(slotFont);
            const QRect panel(margin, y, panelWidth, panelHeight);
            painter.fillRect(panel, QColor(0, 0, 0, 218));
            painter.setPen(QColor(129, 142, 159));
            painter.drawRect(panel.adjusted(0, 0, -1, -1));
            painter.setPen(QColor(244, 249, 250));
            painter.drawText(margin + pad, y + pad + slotFm.ascent(), label);
            const int rowY = y + 2 * pad + labelHeight;
            for (int slot = 0; slot < 10; ++slot) {
                const bool occupied = stateSlots_.occupiedMask & (1U << slot);
                const QRect cell(margin + pad + slot * (cellWidth + gap), rowY,
                                 cellWidth, cellHeight);
                painter.fillRect(cell, occupied ? QColor(72, 84, 96) : QColor(0, 0, 0));
                painter.setPen(slot == stateSlots_.selected ? QColor(126, 224, 233)
                                                           : QColor(129, 142, 159));
                painter.drawRect(cell.adjusted(0, 0, -1, -1));
                painter.setPen(occupied ? QColor(244, 249, 250) : QColor(129, 142, 159));
                painter.drawText(cell, Qt::AlignCenter, QString::number(slot));
            }
            painter.restore();
        }
    }
    for (const Notice& notice : notices_) {
        const QStringList lines = wrapOsd(notice.text, fm, maxTextWidth, 2);
        const int panelHeight = lines.size() * lineHeight + 2 * pad;
        if (y + panelHeight > height() - margin) break;
        y += drawPanel(lines, margin, y, false) + 4;
    }
}

void GameView::paintLoading(QPainter& painter) {
    if (!loading_ || capturingGame_) return;
    painter.save();
    painter.fillRect(rect(), QColor(0, 0, 0, 148));
    const int panelWidth = qMin(560, width() - 40);
    const int panelHeight = loadingFileName_.isEmpty() ? 76 : 108;
    const QRect panel((width() - panelWidth) / 2, (height() - panelHeight) / 2,
                      panelWidth, panelHeight);
    painter.fillRect(panel, QColor(17, 20, 27, 242));
    painter.setPen(QColor(126, 224, 233));
    painter.drawRect(panel.adjusted(0, 0, -1, -1));
    QFont title = font();
    title.setPixelSize(20);
    title.setBold(true);
    painter.setFont(title);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(QColor(244, 249, 250));
    painter.drawText(panel.adjusted(18, 16, -18, -16), Qt::AlignHCenter | Qt::AlignTop,
                     QStringLiteral("Загрузка…"));
    if (!loadingFileName_.isEmpty()) {
        QFont detail = font();
        detail.setPixelSize(14);
        painter.setFont(detail);
        const QString text = QFontMetrics(detail).elidedText(loadingFileName_, Qt::ElideMiddle, panelWidth - 36);
        painter.setPen(QColor(188, 196, 211));
        painter.drawText(panel.adjusted(18, 58, -18, -16), Qt::AlignHCenter | Qt::AlignTop, text);
    }
    painter.restore();
}

QRect GameView::openArea() const {
    // The visible plus is 54x54. Keep a larger target around it without
    // making the entire placeholder clickable.
    const QPoint plusCenter = rect().center() + QPoint(0, -59);
    return QRect(plusCenter.x() - 152, plusCenter.y() - 124, 304, 248);
}

void GameView::mousePressEvent(QMouseEvent* event) {
    if (!loading_ && frame_.isNull() && event->button() == Qt::LeftButton &&
        openArea().contains(event->position().toPoint())) {
        emit openGameRequested();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void GameView::paintRaster(QPainter& painter) {
    painter.fillRect(rect(), frame_.isNull() ? QColor(17, 20, 27) : QColor(Qt::black));

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
                         QStringLiteral("Нажмите + или Ctrl+O · перетащите игру сюда"));
        painter.drawText(QRect(24, center.y() + 51, width() - 48, 26),
                         Qt::AlignCenter,
                         QStringLiteral("Поддержка: MD, GEN, BIN, SMD, SMS, GG, SG, 68K, CUE, ISO, CHD, M3U"));
    } else {
        const QRectF target = gameRect();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(target, frame_);
    }
}


GameView::~GameView() {
    delete shaderView_;
}

QRectF GameView::gameRect() const {
    if (frame_.isNull()) return {};
        const QSize native = VideoFrameGeometry::nativeSize(frame_);
        const double desiredAspect =
            aspectMode_ == AspectMode::Stretch ? double(width()) / qMax(1, height()) :
            aspectMode_ == AspectMode::SquarePixels
                ? double(native.width()) / qMax(1, native.height()) : aspectRatio_;

        double targetWidth = width();
        double targetHeight = targetWidth / desiredAspect;
        if (targetHeight > height()) {
            targetHeight = height();
            targetWidth = targetHeight * desiredAspect;
        }

        if (integerScaling_ && aspectMode_ != AspectMode::Stretch) {
            const double baseWidth = native.height() * desiredAspect;
            const double baseHeight = native.height();
            const int factor = qFloor(qMin(double(width()) / baseWidth,
                                           double(height()) / baseHeight));
            if (factor >= 1) {
                targetWidth = baseWidth * factor;
                targetHeight = baseHeight * factor;
            }
        }

        return QRectF((width() - targetWidth) / 2.0,
                            (height() - targetHeight) / 2.0,
                            targetWidth, targetHeight);

}

void GameView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    if (shaderView_ && shaderView_->isVisible()) {
        painter.fillRect(rect(), Qt::black);
        return;
    }
    paintRaster(painter);
    paintOsd(painter);
    paintLoading(painter);
}

void GameView::repaintView() {
    QWidget::update();
    if (shaderView_) shaderView_->update();
}

void GameView::ensureGpu() {
    if (shaderView_ || gpuFailed_ || gpuCreating_) return;
    const QString platform = QGuiApplication::platformName();
    if (platform == QStringLiteral("offscreen") || platform == QStringLiteral("minimal")) return;
    // Adding the first QOpenGLWidget recreates some native top-level windows and
    // reenters showEvent before the constructor returns.
    gpuCreating_ = true;
    shaderView_ = new ShaderView(this);
    shaderView_->setGeometry(rect());
    shaderView_->show();
    shaderView_->raise();
    gpuCreating_ = false;
    // A failed context may prevent initializeGL from being called altogether.
    QTimer::singleShot(0, this, [this] {
        if (shaderView_ && isVisible() && !shaderView_->isValid())
            gpuFailed(QStringLiteral("Cannot create an OpenGL video context. Raster rendering remains available."));
    });
}

void GameView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    ensureGpu();
}

void GameView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (shaderView_) shaderView_->setGeometry(rect());
}

void GameView::gpuFailed(const QString& error) {
    const bool hadShader = !shaderPreset_.path.isEmpty();
    shaderPreset_ = {};
    gpuFailed_ = true;
    if (shaderView_) {
        shaderView_->hide();
        shaderView_->deleteLater();
        shaderView_ = nullptr;
    }
    repaintView();
    if (hadShader) emit shaderFailed(error);
}

bool GameView::setShaderPreset(const QString& path, QString* error) {
    QString failure;
    ShaderPreset next;
    if (!path.isEmpty() && !ShaderPreset::load(path, next, failure)) {
        if (error) *error = failure;
        return false;
    }
    const QString platform = QGuiApplication::platformName();
    if (!path.isEmpty() && (platform == QStringLiteral("offscreen") || platform == QStringLiteral("minimal"))) {
        if (error) *error = QStringLiteral("GLSL shaders require a native OpenGL window; this platform only supports raster rendering.");
        return false;
    }
    if (shaderView_ && !shaderView_->load(next, failure)) {
        if (error) *error = failure;
        return false;
    }
    shaderPreset_ = std::move(next);
    if (!path.isEmpty() && !shaderView_) {
        gpuFailed_ = false;
        if (isVisible()) ensureGpu();
    }
    if (error) error->clear();
    repaintView();
    return true;
}

bool GameView::setShaderParameter(const QString& id, float value) {
    if (!qIsFinite(value)) return false;
    for (auto& parameter : shaderPreset_.parameters) {
        if (parameter.id != id) continue;
        parameter.value = qBound(parameter.minimum, value, parameter.maximum);
        if (shaderView_) shaderView_->parameter(id, parameter.value);
        repaintView();
        return true;
    }
    return false;
}
