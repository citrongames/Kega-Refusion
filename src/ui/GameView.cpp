#include "i18n/Localization.h"
#include "GameView.h"
#include "InputDisplayPainter.h"
#include "WaylandMouseCapture.h"
#include <QCursor>
#include <QApplication>
#include <QEnterEvent>
#include <QKeyEvent>
#include "../core/VideoFrameGeometry.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QFocusEvent>
#include <QPainter>
#include <QGuiApplication>
#include <QOpenGLWidget>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QShowEvent>
#include <QScreen>
#include <QSurfaceFormat>
#include <memory>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <iterator>

namespace {
quint64 movieSeconds(quint64 frame, double fps) {
    return quint64(double(frame) / fps);
}
QString movieTime(quint64 frame, double fps) {
    const quint64 seconds = movieSeconds(frame, fps);
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString runTimerTime(quint64 microseconds) {
    const quint64 milliseconds = microseconds / 1000;
    const quint64 seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(seconds / 3600)
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'))
        .arg(milliseconds % 1000, 3, 10, QLatin1Char('0'));
}

QString formatRunTimer(GameView::RunTimerMode mode, quint64 frames, quint64 microseconds) {
    if (mode == GameView::RunTimerMode::Time) return runTimerTime(microseconds);
    if (mode == GameView::RunTimerMode::Frames)
        return QString::number(frames);
    return {};
}

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
        setMouseTracking(true);
        installEventFilter(owner_);
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
    QImage captureGameImage(const QSize& native) {
        if (!pipeline_ || !isValid()) return {};
        makeCurrent();
        QImage image;
        {
            QOpenGLFramebufferObject target(native);
            QString error;
            if (target.isValid() && pipeline_->render(owner_->frame_, target.handle(), native,
                    QRect(QPoint(), native), owner_->frameCount_, owner_->rewinding_ ? -1 : 1, error,
                    owner_->spriteOwnership_, owner_->aspectRatio_, presentationRect().size()))
                image = target.toImage();
            glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        }
        doneCurrent();
        return image;
    }
    void resetFrameHistory() {
        if (pipeline_) pipeline_->resetFrameHistory();
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
            owner_->paintOverlays(painter);
            return;
        }
        const qreal dpr = devicePixelRatioF();
        // Invalidate Qt's cached GL state around native drawing; otherwise its
        // font atlas can be mistaken for the video texture on subsequent paints.
        QPainter painter(this);
        painter.beginNativePainting();
        QString error;
        if (!pipeline_->render(owner_->frame_, defaultFramebufferObject(),
                               QSize(qRound(width() * dpr), qRound(height() * dpr)),
                               presentationRect(), owner_->frameCount_, owner_->rewinding_ ? -1 : 1, error, owner_->spriteOwnership_, owner_->aspectRatio_)) {
            failed(error);
        }
        painter.endNativePainting();
        owner_->paintOverlays(painter);
    }
private:
    QRect presentationRect() const {
        const qreal dpr = devicePixelRatioF();
        const QRectF logical = owner_->gameRect();
        // Round edges, rather than independently rounding position and dimensions.
        return QRect(QPoint(qRound(logical.left() * dpr), qRound(logical.top() * dpr)),
                           QPoint(qRound((logical.left() + logical.width()) * dpr) - 1,
                                  qRound((logical.top() + logical.height()) * dpr) - 1));
    }
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
#ifdef Q_OS_WIN
    // Present the OpenGL child on the game area's own HWND. Qt can then keep
    // the top-level window and its native menu on the desktop compositor.
    if (parent) setAttribute(Qt::WA_NativeWindow);
#endif
    waylandMouse_ = new WaylandMouseCapture(this);
    connect(waylandMouse_, &WaylandMouseCapture::motion, this, &GameView::capturedMouseMotion);
    connect(waylandMouse_, &WaylandMouseCapture::lost, this, [this] { releaseMouseCapture(); });
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    cursorIdleTimer_.setSingleShot(true);
    cursorIdleTimer_.setTimerType(Qt::PreciseTimer);
    cursorIdleTimer_.setInterval(1000);
    connect(&cursorIdleTimer_, &QTimer::timeout, this, [this] {
        if (!idleCursorAllowed() || cursor().shape() == Qt::BlankCursor) return;
        cursorBeforeIdle_ = cursor();
        cursorWasExplicit_ = testAttribute(Qt::WA_SetCursor);
        cursorIdleHidden_ = true;
        const QScopedValueRollback<bool> changing(changingIdleCursor_, true);
        setCursor(Qt::BlankCursor);
    });
    setMinimumSize(400, 280);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/osd.ttf"));
    if (fontId >= 0) {
        const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
        if (!families.isEmpty()) osdFamily_ = families.first();
    }
    if (osdFamily_.isEmpty())
        osdFamily_ = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    achievementFallbackBadge_ = QImage(QStringLiteral(":/icons/app.png"))
        .scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    osdClock_.start();
    osdTimer_.setInterval(90);
    connect(&osdTimer_, &QTimer::timeout, this, &GameView::expireOsd);
}

QImage GameView::captureImage() {
    QImage image;
    {
        // Captures omit the aiming overlay while leaving on-screen reticles intact.
        const QScopedValueRollback<bool> capture(capturingReticles_, true);
        // QWidget::grab does not include a composited QOpenGLWidget on every driver.
        image = shaderView_ && shaderView_->isVisible() && shaderView_->isValid()
            ? shaderView_->grabFramebuffer() : grab().toImage();
    }
    if (inputDisplayEnabled_ || std::any_of(std::begin(guns_), std::end(guns_),
                    [](const GunState& gun) { return gun.enabled; })) repaintView();
    return image;
}

QImage GameView::captureGameImage() {
    if (frame_.isNull()) return {};
    const QSize native = VideoFrameGeometry::nativeSize(frame_);
    if (shaderView_ && shaderView_->isVisible() && shaderView_->isValid() && !shaderPreset_.path.isEmpty())
        return shaderView_->captureGameImage(native);
    // Debug masks and backdrop are already applied in the core. A core video
    // filter can produce more samples than the console raster (e.g. Blargg).
    return frame_.size() == native ? frame_ : frame_.scaled(native, Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

void GameView::setFrame(const QImage& frame, double aspectRatio, const QImage& spriteOwnership) {
    const bool hadFrame = !frame_.isNull();
    const QSize previousNative = VideoFrameGeometry::nativeSize(frame_);
    const double previousAspect = aspectRatio_;
    frame_ = frame;
    spriteOwnership_ = shaderPreset_.usesSpriteOwnership() && spriteOwnership.size() == frame.size()
        && spriteOwnership.format() == QImage::Format_Grayscale8 ? spriteOwnership : QImage();
    if (frame_.isNull()) { clearMouseGun(); clearAnalogMouse(); }
    ++frameCount_;
    if (qIsFinite(aspectRatio) && aspectRatio > 0.25 && aspectRatio < 5.0)
        aspectRatio_ = aspectRatio;
    if (previousNative != VideoFrameGeometry::nativeSize(frame_) ||
        previousAspect != aspectRatio_) resetAnalogPointer();
    // A frame arrival must not continually postpone the mouse idle deadline.
    if (hadFrame != !frame_.isNull()) resetIdleCursor();
    repaintView();
}

void GameView::clearGame() {
    inputDisplay_ = {};
    setLightgunInputEnabled(false);
    setAnalogMousePlayer(-1, ControllerType::Pad);
    for (unsigned slot = 0; slot < kGunSlots; ++slot) setLightgunEnabled(slot, false);
    notices_.clear(); stateSlots_.expiresAt = 0; clearAchievementNotices(); osdTimer_.stop();
    movieMode_ = 0; movieFrame_ = movieLength_ = 0;
    videoRecording_ = false;
    runTimerReady_ = false;
    runTimerFrames_ = runTimerMicroseconds_ = 0;
    runTimerText_.clear();
    metricsReady_ = gameFpsAvailable_ = false;
    rebuildPerformanceMetrics();
    setPausedStatus(false); setFastForwardStatus(false); setRewindStatus(false);
    setFrame({}, 4.0 / 3.0);
    frameCount_ = 0;
    resetShaderHistory();
}

void GameView::resetShaderHistory() {
    if (shaderView_) shaderView_->resetFrameHistory();
}

void GameView::setLoading(bool loading, const QString& fileName) {
    const QString name = loading ? fileName.left(220) : QString();
    if (loading_ == loading && loadingFileName_ == name) return;
    loading_ = loading;
    if (loading_) { clearMouseGun(); clearAnalogMouse(); clearAchievementNotices(); }
    loadingFileName_ = name;
    resetIdleCursor();
    repaintView();
    if (loading && osdEnabled_ && isVisible()) {
        // One immediate presentation also precedes a synchronous archive scan.
        // Do not process input/events recursively or start an animation timer.
        if (shaderView_ && shaderView_->isVisible()) shaderView_->repaint();
        QWidget::repaint();
    }
}

void GameView::setAspectMode(AspectMode mode) {
    aspectMode_ = mode;
    resetAnalogPointer();
    repaintView();
}

void GameView::setIntegerScaling(bool enabled) {
    integerScaling_ = enabled;
    resetAnalogPointer();
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

void GameView::showAchievementNotice(const QString& heading, const QString& title,
                                     const QString& description, const QString& badgeUrl,
                                     bool sound) {
    if (!osdEnabled_ || loading_ || title.trimmed().isEmpty() || achievementQueue_.size() >= 8)
        return;
    const bool first = achievementQueue_.isEmpty();
    achievementQueue_.append({heading.trimmed().left(80), title.trimmed().left(120),
                              description.trimmed().left(240), badgeUrl.left(2048), {}, sound});
    if (first) startAchievementPopup();
    if (!badgeUrl.isEmpty()) emit achievementBadgeRequested(badgeUrl);
}

void GameView::setAchievementBadge(const QString& url, const QImage& image) {
    if (url.isEmpty() || image.isNull() || achievementQueue_.isEmpty()) return;
    const QImage badge = image.width() > 256 || image.height() > 256
        ? image.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation) : image;
    bool activeChanged = false;
    for (int i = 0; i < achievementQueue_.size(); ++i) {
        if (achievementQueue_[i].badgeUrl != url) continue;
        achievementQueue_[i].badge = badge;
        if (i == 0) activeChanged = true;
    }
    if (activeChanged) {
        rebuildAchievementPixmap();
        repaintView();
    }
}

void GameView::clearAchievementNotices() {
    if (achievementQueue_.isEmpty()) return;
    achievementQueue_.clear();
    achievementExpiresAt_ = 0;
    achievementPixmap_ = {};
    achievementCardSize_ = {};
    if (notices_.isEmpty() && !stateSlots_.expiresAt) osdTimer_.stop();
    repaintView();
}

void GameView::setAchievementPosition(int position) {
    const int next = position >= 0 && position <= 5 ? position : 0;
    if (achievementPosition_ == next) return;
    achievementPosition_ = next;
    if (!achievementQueue_.isEmpty()) repaintView();
}

QRect GameView::achievementPopupRect(const QSize& canvas) const {
    if (achievementQueue_.isEmpty() || achievementCardSize_.isEmpty()) return {};
    const int margin = 12;
    const int column = achievementPosition_ % 3;
    const int x = column == 0 ? margin
        : column == 1 ? (canvas.width() - achievementCardSize_.width()) / 2
                      : canvas.width() - margin - achievementCardSize_.width();
    const int y = achievementPosition_ < 3 ? margin
        : canvas.height() - margin - achievementCardSize_.height();
    return QRect(QPoint(x, y), achievementCardSize_);
}

void GameView::startAchievementPopup() {
    if (achievementQueue_.isEmpty()) return;
    achievementExpiresAt_ = osdClock_.elapsed() + 4000;
    rebuildAchievementPixmap();
    if (!osdTimer_.isActive()) osdTimer_.start();
    repaintView();
    emit achievementPopupStarted(achievementQueue_.first().sound);
}

void GameView::rebuildAchievementPixmap() {
    achievementPixmap_ = {};
    achievementCardSize_ = {};
    if (achievementQueue_.isEmpty() || width() <= 0 || height() <= 0) return;

    const qreal scale = osdScale();
    const int cardWidth = qMin(qFloor(width() / scale) - 24, 460);
    if (cardWidth <= 0) return;
    const int pad = 12, badgeSize = 84, gap = 10;
    const int textWidth = qMax(1, cardWidth - 2 * pad - badgeSize - gap);
    QFont headingFont = font();
    headingFont.setPixelSize(15);
    headingFont.setWeight(QFont::DemiBold);
    QFont titleFont = font();
    titleFont.setPixelSize(20);
    titleFont.setWeight(QFont::Bold);
    QFont detailFont = font();
    detailFont.setPixelSize(13);
    const QFontMetrics headingMetrics(headingFont);
    const QFontMetrics titleMetrics(titleFont);
    const QFontMetrics detailMetrics(detailFont);
    const AchievementPopup& popup = achievementQueue_.first();
    const QString heading = headingMetrics.elidedText(popup.heading, Qt::ElideRight, textWidth);
    const QStringList titleLines = wrapOsd(popup.title, titleMetrics, textWidth, 2);
    const QStringList details = popup.description.isEmpty()
        ? QStringList{} : wrapOsd(popup.description, detailMetrics, textWidth, 2);
    const int textHeight = headingMetrics.height() + 3
        + titleLines.size() * (titleMetrics.height() + 1)
        + (details.isEmpty() ? 0 : 3 + details.size() * (detailMetrics.height() + 1));
    const int cardHeight = qMax(badgeSize, textHeight) + 2 * pad;
    achievementCardSize_ = QSize(cardWidth, cardHeight);

    const qreal dpr = devicePixelRatioF() * scale;
    achievementPixmap_ = QPixmap(qMax(1, qCeil(cardWidth * dpr)),
                                  qMax(1, qCeil(cardHeight * dpr)));
    achievementPixmap_.setDevicePixelRatio(dpr);
    achievementPixmap_.fill(Qt::transparent);
    QPainter painter(&achievementPixmap_);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QRect card(0, 0, cardWidth, cardHeight);
    painter.fillRect(card, QColor(17, 23, 30, 245));
    painter.setPen(QPen(QColor(126, 224, 233), 2));
    painter.drawRect(card.adjusted(1, 1, -2, -2));
    const QRect badgeBox(pad, (cardHeight - badgeSize) / 2, badgeSize, badgeSize);
    painter.fillRect(badgeBox, QColor(44, 53, 65));
    painter.setPen(QColor(96, 111, 127));
    painter.drawRect(badgeBox.adjusted(0, 0, -1, -1));
    const QImage& badge = popup.badge.isNull()
        ? achievementFallbackBadge_ : popup.badge;
    if (!badge.isNull()) {
        const QSize scaled = badge.size().scaled(badgeBox.size() - QSize(8, 8),
                                                  Qt::KeepAspectRatio);
        const QRect target(QPoint(badgeBox.center().x() - scaled.width() / 2,
                                  badgeBox.center().y() - scaled.height() / 2), scaled);
        painter.drawImage(target, badge);
    }
    const int textX = pad + badgeSize + gap;
    int textY = pad;
    painter.setFont(headingFont);
    painter.setPen(QColor(126, 224, 233));
    painter.drawText(textX, textY + headingMetrics.ascent(), heading);
    textY += headingMetrics.height() + 3;
    painter.setFont(titleFont);
    painter.setPen(QColor(250, 251, 252));
    for (const QString& line : titleLines) {
        painter.drawText(textX, textY + titleMetrics.ascent(), line);
        textY += titleMetrics.height() + 1;
    }
    textY += 2;
    painter.setFont(detailFont);
    painter.setPen(QColor(195, 207, 219));
    for (const QString& line : details) {
        painter.drawText(textX, textY + detailMetrics.ascent(), line);
        textY += detailMetrics.height() + 1;
    }
}

void GameView::setOsdEnabled(bool enabled) {
    osdEnabled_ = enabled;
    if (!enabled) {
        runTimerReady_ = false;
        runTimerText_.clear();
    }
    notices_.clear();
    stateSlots_.expiresAt = 0;
    clearAchievementNotices();
    osdTimer_.stop();
    repaintView();
}

void GameView::setInputDisplay(const InputDisplayState& state) {
    if (inputDisplay_ == state) return;
    const bool visibleChange = !(InputDisplayPainter::visualState(inputDisplay_) ==
                                 InputDisplayPainter::visualState(state));
    inputDisplay_ = state;
    if (osdEnabled_ && inputDisplayEnabled_ && visibleChange) repaintView();
}

void GameView::setInputDisplayEnabled(bool enabled) {
    if (inputDisplayEnabled_ == enabled) return;
    inputDisplayEnabled_ = enabled;
    if (!enabled) {
        inputDisplayRaster_ = {};
        inputDisplayRasterValid_ = false;
    }
    repaintView();
}

void GameView::setPerformanceMetricsEnabled(bool enabled) {
    if (performanceMetricsEnabled_ == enabled) return;
    performanceMetricsEnabled_ = enabled;
    if (enabled) metricsReady_ = false;
    rebuildPerformanceMetrics();
    repaintView();
}

void GameView::setGameFpsEnabled(bool enabled) {
    if (gameFpsEnabled_ == enabled) return;
    gameFpsEnabled_ = enabled;
    if (enabled) gameFpsAvailable_ = false;
    rebuildPerformanceMetrics();
    repaintView();
}

void GameView::setPerformanceMetrics(double emulatedFps, double /* speedPercent */,
                                     double gameFps, bool gameFpsAvailable) {
    emulatedFps_ = emulatedFps;
    gameFps_ = gameFps;
    gameFpsAvailable_ = gameFpsAvailable;
    metricsReady_ = true;
    if (performanceMetricsEnabled_ || gameFpsEnabled_) {
        rebuildPerformanceMetrics();
        repaintView();
    }
}

void GameView::rebuildPerformanceMetrics() {
    metricsLines_.clear();
    if (performanceMetricsEnabled_)
        metricsLines_.append(metricsReady_ ? QString::number(emulatedFps_, 'f', 1) : QStringLiteral("—"));
    if (gameFpsEnabled_)
        metricsLines_.append(gameFpsAvailable_ ? QStringLiteral("~") + QString::number(gameFps_, 'f', 1)
                                             : QStringLiteral("~—"));
}

void GameView::setMutedStatus(bool muted) {
    if (muted_ == muted) return;
    muted_ = muted;
    if (osdEnabled_) repaintView();
}

void GameView::setPausedStatus(bool paused) {
    if (paused_ == paused) return;
    paused_ = paused;
    if (osdEnabled_) repaintView();
}

void GameView::setFastForwardStatus(bool enabled) {
    if (fastForward_ == enabled) return;
    fastForward_ = enabled;
    if (osdEnabled_) repaintView();
}

void GameView::setFastForwardFactor(int factor) {
    factor = qMax(0, factor);
    if (fastForwardFactor_ == factor) return;
    fastForwardFactor_ = factor;
    if (osdEnabled_ && fastForward_) repaintView();
}

void GameView::setConnectionWaiting(bool waiting) {
    if(connectionWaiting_==waiting)return;
    connectionWaiting_=waiting;
    if(osdEnabled_)repaintView();
}

void GameView::setReadinessWaiting(bool waiting) {
    if(readinessWaiting_==waiting)return;
    readinessWaiting_=waiting;
    if(osdEnabled_)repaintView();
}

void GameView::setRewindStatus(bool enabled) {
    if (rewinding_ == enabled) return;
    rewinding_ = enabled;
    if (osdEnabled_) repaintView();
}

void GameView::setMovieCounterAsTime(bool enabled) {
    if (movieCounterAsTime_ == enabled) return;
    movieCounterAsTime_ = enabled;
    if (osdEnabled_ && movieMode_ == 2) repaintView();
}

void GameView::setMovieStatus(int mode, quint64 frame, quint64 length, double fps) {
    if (!std::isfinite(fps) || fps <= 0.0 || fps > 1000.0) fps = 60.0;
    const bool changed = mode != movieMode_ || (mode == 2 && (movieCounterAsTime_
        ? movieSeconds(frame, fps) != movieSeconds(movieFrame_, movieFps_) ||
          movieSeconds(length, fps) != movieSeconds(movieLength_, movieFps_)
        : frame != movieFrame_ || length != movieLength_));
    movieMode_ = mode;
    movieFrame_ = frame;
    movieLength_ = length;
    movieFps_ = fps;
    if (changed && osdEnabled_) repaintView();
}

void GameView::setVideoRecordingStatus(bool recording) {
    if (videoRecording_ == recording) return;
    videoRecording_ = recording;
    if (osdEnabled_) repaintView();
}

void GameView::setRunTimerMode(RunTimerMode mode) {
    if (runTimerMode_ == mode) return;
    runTimerMode_ = mode;
    if (mode == RunTimerMode::Off) runTimerReady_ = false;
    const QString text = runTimerReady_
        ? formatRunTimer(mode, runTimerFrames_, runTimerMicroseconds_) : QString();
    if (runTimerText_ == text) return;
    runTimerText_ = text;
    if (osdEnabled_ && !frame_.isNull() && !loading_) repaintView();
}

void GameView::setRunTimer(quint64 frames, quint64 microseconds) {
    if (!osdEnabled_ || runTimerMode_ == RunTimerMode::Off) return;
    runTimerFrames_ = frames;
    runTimerMicroseconds_ = microseconds;
    runTimerReady_ = true;
    const QString text = formatRunTimer(runTimerMode_, frames, microseconds);
    if (runTimerText_ == text) return;
    runTimerText_ = text;
    if (osdEnabled_ && !frame_.isNull() && !loading_) repaintView();
}

void GameView::setRewindProgress(qint64 remainingMs, qint64 totalMs) {
    auto time = [](qint64 ms) {
        const qint64 tenths = qMax<qint64>(0, ms) / 100;
        return QStringLiteral("%1:%2.%3").arg(tenths / 600, 2, 10, QLatin1Char('0'))
            .arg(tenths / 10 % 60, 2, 10, QLatin1Char('0')).arg(tenths % 10);
    };
    // The origin stays fixed; remaining history counts down to zero.
    const QString text = QStringLiteral("%1 / %2").arg(time(remainingMs), time(totalMs));
    if (rewindTime_ == text) return;
    rewindTime_ = text;
    if (rewinding_ && osdEnabled_) repaintView();
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
    if (!achievementQueue_.isEmpty() && achievementExpiresAt_ <= now) {
        achievementQueue_.removeFirst();
        achievementExpiresAt_ = 0;
        achievementPixmap_ = {};
        achievementCardSize_ = {};
        if (achievementQueue_.isEmpty()) repaintView();
        else startAchievementPopup();
    }
    if (notices_.isEmpty() && !stateSlots_.expiresAt && achievementQueue_.isEmpty())
        osdTimer_.stop();
}

qreal GameView::osdScale() const {
    // Opening the menu changes the viewport height. Keep HUD sizes tied to
    // the whole window (or monitor in fullscreen), including on HiDPI screens.
    const QWidget* top = window();
    const int referenceHeight = top->isFullScreen() && top->screen()
        ? top->screen()->geometry().height() : top->height();
    return qMax(0.5, referenceHeight / 720.0);
}

void GameView::paintOverlays(QPainter& painter) {
    if (osdEnabled_) {
        const bool status = !notices_.isEmpty() || stateSlots_.expiresAt ||
            paused_ || connectionWaiting_ || readinessWaiting_ || fastForward_ || rewinding_ || muted_ ||
            movieMode_ == 1 || movieMode_ == 2 || videoRecording_ ||
            (!frame_.isNull() && !loading_ && !runTimerText_.isEmpty());
        const bool metrics = !frame_.isNull() && (performanceMetricsEnabled_ || gameFpsEnabled_);
        const bool input = inputDisplayEnabled_ && !frame_.isNull() && !loading_ && !capturingReticles_;
        if (status || metrics || input || !achievementPixmap_.isNull() || loading_) {
            const qreal scale = osdScale();
            const QSize canvas(qRound(width() / scale), qRound(height() / scale));
            painter.save();
            painter.scale(scale, scale);
            if (input) paintInputDisplay(painter, canvas);
            if (status || metrics) paintOsd(painter, canvas);
            // Achievement notifications cover the usual indicators until they expire.
            if (!achievementPixmap_.isNull())
                painter.drawPixmap(achievementPopupRect(canvas).topLeft(), achievementPixmap_);
            if (loading_) paintLoading(painter, canvas);
            painter.restore();
        }
    }
    paintCrosshairs(painter);
}

void GameView::paintInputDisplay(QPainter& painter, const QSize& canvas) {
    const InputDisplayState visual = InputDisplayPainter::visualState(inputDisplay_);
    const auto row = InputDisplayPainter::geometry(visual, QRect(QPoint(), canvas));
    if (row.scale <= 0) return;
    const qreal dpr = painter.device()->devicePixelRatioF() * painter.worldTransform().m11();
    const QSize pixels(qMax(1, qCeil(row.size.width() * dpr)),
                       qMax(1, qCeil(row.size.height() * dpr)));
    if (!inputDisplayRasterValid_ || !(inputDisplayRasterState_ == visual) ||
        inputDisplayRasterPixels_ != pixels || inputDisplayRasterScale_ != row.scale ||
        inputDisplayRasterDpr_ != dpr) {
        QImage raster(pixels, QImage::Format_ARGB32_Premultiplied);
        raster.fill(Qt::transparent);
        QPainter rowPainter(&raster);
        InputDisplayPainter::paintRow(rowPainter, visual, row.scale * dpr);
        rowPainter.end();
        inputDisplayRaster_ = std::move(raster);
        inputDisplayRasterState_ = visual;
        inputDisplayRasterPixels_ = pixels;
        inputDisplayRasterScale_ = row.scale;
        inputDisplayRasterDpr_ = dpr;
        inputDisplayRasterValid_ = true;
    }
    const QRectF target(row.topLeft, QSizeF(pixels) / dpr);
    painter.drawImage(target, inputDisplayRaster_);
}

void GameView::paintOsd(QPainter& painter, const QSize& canvas) {
    const int margin = 12;
    QFont osd(osdFamily_);
    osd.setPixelSize(16);
    osd.setStyleStrategy(QFont::NoAntialias);
    painter.setFont(osd);
    painter.setRenderHint(QPainter::TextAntialiasing, false);
    const QFontMetrics fm(osd);
    const int pad = 7;
    const int lineHeight = fm.height() + 2;
    const int maxTextWidth = qMax(60, canvas.width() - 2 * margin - 2 * pad);
    auto drawPanel = [&](const QStringList& lines, int y, bool accent, Qt::AlignmentFlag align = Qt::AlignLeft) {
        int textWidth = 0;
        for (const QString& line : lines)
            textWidth = qMax(textWidth, fm.horizontalAdvance(line));
        const int panelWidth = qMin(canvas.width() - 2 * margin, textWidth + 2 * pad);
        const int panelHeight = lines.size() * lineHeight + 2 * pad;
        const int x = align == Qt::AlignRight ? canvas.width() - margin - panelWidth : margin;
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

    int rightY = margin;
    if (!frame_.isNull() && !metricsLines_.isEmpty())
        rightY += drawPanel(metricsLines_, rightY, true, Qt::AlignRight) + 4;
    if (!frame_.isNull() && !loading_ && !runTimerText_.isEmpty())
        drawPanel({runTimerText_}, rightY, true, Qt::AlignRight);

    int y = margin;
    if (movieMode_ == 1 || videoRecording_) {
        QStringList sources;
        if (movieMode_ == 1) sources << Localization::text(QStringLiteral("ВВОД"));
        if (videoRecording_) sources << Localization::text(QStringLiteral("ВИДЕО"));
        const QString label = sources.join(QStringLiteral(" + "));
        const int dotSize = qMax(7, fm.height() / 2);
        const int panelWidth = qMin(canvas.width() - 2 * margin,
                                    fm.horizontalAdvance(label) + dotSize + 3 * pad);
        const int panelHeight = lineHeight + 2 * pad;
        const QRect box(margin, y, panelWidth, panelHeight);
        painter.save();
        painter.fillRect(box, QColor(0, 0, 0, 218));
        painter.setPen(QColor(126, 224, 233));
        painter.drawRect(box.adjusted(0, 0, -1, -1));
        painter.setPen(Qt::NoPen);
        painter.setBrush((osdClock_.elapsed() / 650) % 2 ? QColor(149, 43, 48) : QColor(232, 56, 62));
        painter.drawEllipse(QRect(margin + pad, y + (panelHeight - dotSize) / 2,
                                  dotSize, dotSize));
        const int textX = margin + 2 * pad + dotSize;
        const int baseline = y + pad + fm.ascent();
        painter.setPen(Qt::black);
        painter.drawText(textX + 1, baseline + 1, label);
        painter.setPen(QColor(244, 249, 250));
        painter.drawText(textX, baseline, label);
        painter.restore();
        y += panelHeight + 4;
    }
    if (movieMode_ == 2) {
        const auto counter = [this](quint64 frame) {
            return movieCounterAsTime_ ? movieTime(frame, movieFps_) : QString::number(frame);
        };
        const QString unit = movieCounterAsTime_ ? Localization::text(QStringLiteral("ВРЕМЯ"))
                                                 : Localization::text(QStringLiteral("КАДР"));
        y += drawPanel({Localization::text(QStringLiteral("ВОСПРОИЗВЕДЕНИЕ")),
                        QStringLiteral("%1 %2 / %3").arg(unit, counter(movieFrame_), counter(movieLength_))},
                       y, true) + 4;
    }
    if (connectionWaiting_)
        y += drawPanel({Localization::text(QStringLiteral("СВЯЗЬ ПОТЕРЯНА"))}, y, true) + 4;
    else if (readinessWaiting_)
        y += drawPanel({Localization::text(QStringLiteral("ОЖИДАНИЕ ИГРОКА")),
                        Localization::text(QStringLiteral("Загрузка игры или синхронизация"))}, y, true) + 4;
    if (paused_)
        y += drawPanel({Localization::text(QStringLiteral("ПАУЗА"))}, y, true) + 4;
    if (fastForward_)
        y += drawPanel({fastForwardFactor_ ? Localization::text(QStringLiteral("УСКОРЕНИЕ ×%1")).arg(fastForwardFactor_) : Localization::text(QStringLiteral("УСКОРЕНИЕ МАКС."))},
                       y, true) + 4;
    if (rewinding_)
        y += drawPanel({Localization::text(QStringLiteral("ПЕРЕМОТКА НАЗАД")), rewindTime_}, y, true) + 4;
    if (muted_)
        y += drawPanel({Localization::text(QStringLiteral("ЗВУК ВЫКЛЮЧЕН"))}, y, true) + 4;
    if (stateSlots_.expiresAt) {
        // Ten compact cells, as in FCEUX: filled occupied slots and a selected outline.
        // Keep the row inside narrow windows even when their height requests a large font.
        QFont slotFont(osd);
        const int gap = qMax(2, pad / 2);
        const int available = canvas.width() - 2 * margin - 2 * pad;
        int cellWidth = 0;
        for (;;) {
            const QFontMetrics metrics(slotFont);
            cellWidth = metrics.horizontalAdvance(QLatin1Char('0')) + 2 * pad;
            if (10 * cellWidth + 9 * gap <= available || slotFont.pixelSize() <= 8) break;
            slotFont.setPixelSize(qMax(8, slotFont.pixelSize() - 8));
        }
        // A tall, narrow window may also need less horizontal padding per cell.
        cellWidth = qMin(cellWidth, (available - 9 * gap) / 10);
        const QFontMetrics slotFm(slotFont);
        const int cellHeight = slotFm.height() + 2 * pad;
        const int labelHeight = slotFm.height() + 2;
        const int rowWidth = 10 * cellWidth + 9 * gap;
        const QString label = Localization::text(QStringLiteral("СЛОТ %1 · %2"))
                                  .arg(stateSlots_.selected)
                                  .arg(stateSlots_.occupiedMask & (1U << stateSlots_.selected)
                                           ? Localization::text(QStringLiteral("ЗАНЯТ")) : Localization::text(QStringLiteral("ПУСТ")));
        const int panelWidth = qMax(rowWidth, slotFm.horizontalAdvance(label)) + 2 * pad;
        const int panelHeight = labelHeight + cellHeight + 3 * pad;
        if (cellWidth > 0 && y + panelHeight <= canvas.height() - margin) {
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
                QPen outline(slot == stateSlots_.selected ? QColor(126, 224, 233)
                                                          : QColor(129, 142, 159));
                // Keep a full device pixel of contrast even in a small window.
                outline.setCosmetic(painter.deviceTransform().m11() < 1.0);
                painter.setPen(outline);
                painter.drawRect(cell.adjusted(0, 0, -1, -1));
                painter.setPen(occupied ? QColor(244, 249, 250) : QColor(129, 142, 159));
                painter.drawText(cell, Qt::AlignCenter, QString::number(slot));
            }
            painter.restore();
        }
    }
    for (const Notice& notice : notices_) {
        QStringList lines;
        if (notice.category == QLatin1String("recordingCompatibility")) {
            for (const QString& part : notice.text.split(QLatin1Char('\n'))) {
                for (const QString& line : wrapOsd(part, fm, maxTextWidth, 2)) {
                    if (lines.size() == 6) break;
                    lines.append(line);
                }
                if (lines.size() == 6) break;
            }
        } else {
            lines = wrapOsd(notice.text, fm, maxTextWidth, 2);
        }
        const int panelHeight = lines.size() * lineHeight + 2 * pad;
        if (y + panelHeight > canvas.height() - margin) break;
        y += drawPanel(lines, y, false) + 4;
    }

}

void GameView::paintLoading(QPainter& painter, const QSize& canvas) {
    painter.save();
    painter.fillRect(QRect(QPoint(), canvas), QColor(0, 0, 0, 148));
    const int panelWidth = qMin(560, canvas.width() - 40);
    const int panelHeight = loadingFileName_.isEmpty() ? 76 : 108;
    const QRect panel((canvas.width() - panelWidth) / 2, (canvas.height() - panelHeight) / 2,
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
                     Localization::text(QStringLiteral("Загрузка…")));
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

void GameView::setLightgunEnabled(unsigned player, bool enabled, unsigned gunNumber) {
    if (player >= kGunSlots) return;
    gunNumber = qBound(1U, gunNumber, 2U);
    auto& gun = guns_[player];
    if (gun.enabled == enabled && gun.number == gunNumber) return;
    if (!enabled && mouseGunPlayer_ == int(player)) clearMouseGun();
    gun.enabled = enabled;
    gun.number = gunNumber;
    if (!enabled) gun.hasAim = false;
    repaintView();
}

void GameView::setCrosshairVisible(unsigned player, bool visible) {
    if (player >= kGunSlots || guns_[player].crosshairVisible == visible) return;
    guns_[player].crosshairVisible = visible;
    repaintView();
}

void GameView::setLightgunAim(unsigned player, QPointF normalized, bool offscreen) {
    if (player >= kGunSlots) return;
    auto& gun = guns_[player];
    const bool valid = qIsFinite(normalized.x()) && qIsFinite(normalized.y());
    const QPointF aim = valid ? QPointF(qBound(0.0, normalized.x(), 1.0),
                                        qBound(0.0, normalized.y(), 1.0)) : gun.aim;
    offscreen = offscreen || !valid;
    if (gun.hasAim && gun.aim == aim && gun.offscreen == offscreen) return;
    gun.aim = aim;
    gun.hasAim = valid;
    gun.offscreen = offscreen;
    repaintView();
}

void GameView::setMouseGunPlayer(int player) {
    if (player < -1 || player >= int(kGunSlots) || mouseGunPlayer_ == player) return;
    releaseMouseCapture();
    clearMouseGun();
    mouseGunPlayer_ = player;
}

void GameView::setAnalogMousePlayer(int player, ControllerType type) {
    if (player < -1 || player >= int(kGunSlots) ||
        (player >= 0 && !isAnalogController(type))) return;
    if (analogMousePlayer_ == player && analogMouseType_ == type) return;
    releaseMouseCapture();
    clearAnalogMouse();
    analogMousePlayer_ = player;
    analogMouseType_ = player >= 0 ? type : ControllerType::Pad;
}

void GameView::setMouseCaptureEnabled(bool enabled) {
    if (mouseCaptureEnabled_ == enabled) return;
    releaseMouseCapture();
    mouseCaptureEnabled_ = enabled;
    clearMouseGun(); clearAnalogMouse();
}

void GameView::setCursorIdleTimeout(int seconds) {
    const int interval = qBound(0, seconds, 60) * 1000;
    if (cursorIdleTimer_.interval() == interval) return;
    cursorIdleTimer_.setInterval(interval);
    resetIdleCursor();
}

bool GameView::beginMouseCapture() {
    if (mouseCaptured_) return true;
    if (!mouseCaptureEnabled_ || (!analogMouseAvailable() && !mouseGunAvailable())) return false;
    resetIdleCursor(false);
    if (mouseGunAvailable()) capturedPointer_ = guns_[mouseGunPlayer_].aim;
    else if (analogMouseType_ == ControllerType::Paddle)
        capturedPointer_ = QPointF((analogPaddlePosition_ + 32768.0) / 65535.0, 0.5);
    else capturedPointer_ = QPointF(0.5, 0.5);
    resetAnalogPointer();
    setFocus(Qt::MouseFocusReason);
    const bool wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    if (wayland) {
        if (!waylandMouse_->begin(this)) {
            resetIdleCursor();
            showOsd(Localization::text(QStringLiteral("Захват мыши недоступен.")));
            return false;
        }
        setCursor(Qt::BlankCursor);
    } else {
        captureReturnPosition_ = QCursor::pos();
        captureAnchor_ = mapToGlobal(gameRect().center().toPoint());
        grabMouse(Qt::BlankCursor);
        if (QWidget::mouseGrabber() != this) {
            resetIdleCursor();
            return false;
        }
    }
    mouseCaptured_ = true;
    if (!wayland) QCursor::setPos(captureAnchor_);
    showOsd(Localization::text(QStringLiteral("Мышь захвачена · Esc — освободить")), 1800, QStringLiteral("mouse-capture"));
    return true;
}

bool GameView::releaseMouseCapture() {
    if (!mouseCaptured_) return false;
    mouseCaptured_ = false;
    if (waylandMouse_->active()) {
        waylandMouse_->end();
        unsetCursor();
    } else {
        if (QWidget::mouseGrabber() == this) releaseMouse();
        // Do not move the pointer on another application after focus loss.
        if (isActiveWindow() && isVisible() &&
            !QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
            QCursor::setPos(captureReturnPosition_);
        unsetCursor();
    }
    clearMouseGun(); clearAnalogMouse();
    notices_.erase(std::remove_if(notices_.begin(), notices_.end(), [](const Notice& notice) {
        return notice.category == QLatin1String("mouse-capture");
    }), notices_.end());
    repaintView();
    resetIdleCursor();
    return true;
}

void GameView::capturedMouseMotion(double dx, double dy) {
    if (!mouseCaptured_ || (!analogMouseAvailable() && !mouseGunAvailable())) return;
    const QRectF game = gameRect();
    if (game.isEmpty() || (dx == 0.0 && dy == 0.0)) return;
    if (mouseGunAvailable()) {
        // A small offscreen margin retains lightgun offscreen/reload behavior.
        capturedPointer_.setX(std::clamp(capturedPointer_.x() + dx / game.width(), -0.05, 1.05));
        capturedPointer_.setY(std::clamp(capturedPointer_.y() + dy / game.height(), -0.05, 1.05));
        updateMouseAim(QPointF());
    } else if (analogMouseType_ == ControllerType::Paddle) {
        capturedPointer_.setX(std::clamp(capturedPointer_.x() + dx / game.width(), 0.0, 1.0));
        updateAnalogPointer(QPointF());
    } else {
        const QSize native = VideoFrameGeometry::nativeSize(frame_);
        emit analogMouseMotion(unsigned(analogMousePlayer_),
                               dx * native.width() / game.width(),
                               dy * native.height() / game.height());
    }
}

void GameView::resetAnalogPointer() {
    analogBaselineValid_ = false;
    analogPaddlePositionValid_ = false;
    emit analogPointerReset();
}

void GameView::setLightgunInputEnabled(bool enabled) {
    if (lightgunInputEnabled_ == enabled) return;
    lightgunInputEnabled_ = enabled;
    if (!enabled) { releaseMouseCapture(); clearMouseGun(); clearAnalogMouse(); }
}

bool GameView::analogMouseAvailable() const {
    return lightgunInputEnabled_ && analogMousePlayer_ >= 0 &&
           analogMousePlayer_ < int(kGunSlots) && isAnalogController(analogMouseType_) &&
           !frame_.isNull() && !loading_;
}

void GameView::updateAnalogPointer(const QPointF& eventPosition) {
    if (!analogMouseAvailable()) return;
    const QRectF game = gameRect();
    const QPointF position = mouseCaptured_
        ? QPointF(game.left()+capturedPointer_.x()*game.width(), game.top()+capturedPointer_.y()*game.height()) : eventPosition;
    if (game.isEmpty() || !game.contains(position)) {
        if (analogBaselineValid_ || analogPaddlePositionValid_)
            resetAnalogPointer();
        return;
    }
    const unsigned player = unsigned(analogMousePlayer_);
    if (analogMouseType_ == ControllerType::Paddle) {
        const double normalized = (position.x() - game.left()) / game.width();
        const int value = std::clamp(int(std::lround(normalized * 65535.0 - 32768.0)),
                                     -32768, 32767);
        if (!analogPaddlePositionValid_ || analogPaddlePosition_ != value) {
            analogPaddlePosition_ = value;
            analogPaddlePositionValid_ = true;
            emit analogPaddleMoved(player, value);
        }
        return;
    }
    if (mouseCaptured_ || mouseCaptureEnabled_) return;
    if (analogBaselineValid_) {
        const QSize native = VideoFrameGeometry::nativeSize(frame_);
        const QPointF delta = position - analogLastPosition_;
        const double dx = delta.x() * native.width() / game.width();
        const double dy = delta.y() * native.height() / game.height();
        if (dx != 0.0 || dy != 0.0) emit analogMouseMotion(unsigned(analogMousePlayer_), dx, dy);
    }
    analogLastPosition_ = position;
    analogBaselineValid_ = true;

}

void GameView::clearAnalogMouse() {
    if (analogMousePlayer_ >= 0 && analogMousePlayer_ < int(kGunSlots) &&
        analogMouseButtons_) {
        analogMouseButtons_ = 0;
        emit gunButtonsChanged(unsigned(analogMousePlayer_), 0);
    }
    resetAnalogPointer();
}

bool GameView::mouseGunAvailable() const {
    return lightgunInputEnabled_ && mouseGunPlayer_ >= 0 && mouseGunPlayer_ < int(kGunSlots) &&
           guns_[mouseGunPlayer_].enabled && !frame_.isNull() && !loading_;
}

void GameView::updateMouseAim(const QPointF& eventPosition) {
    if (!mouseGunAvailable()) return;
    const QRectF game = gameRect();
    if (game.isEmpty()) return;
    const QPointF position = mouseCaptured_
        ? QPointF(game.left()+capturedPointer_.x()*game.width(), game.top()+capturedPointer_.y()*game.height()) : eventPosition;
    const bool offscreen = !game.contains(position);
    const QPointF aim(qBound(0.0, (position.x() - game.left()) / game.width(), 1.0),
                      qBound(0.0, (position.y() - game.top()) / game.height(), 1.0));
    auto& gun = guns_[mouseGunPlayer_];
    if (gun.hasAim && gun.aim == aim && gun.offscreen == offscreen) return;
    gun.aim = aim;
    gun.hasAim = true;
    gun.offscreen = offscreen;
    emit gunAimChanged(unsigned(mouseGunPlayer_), aim.x(), aim.y(), offscreen);
    repaintView();
}

void GameView::clearMouseGun() {
    if (mouseGunPlayer_ < 0 || mouseGunPlayer_ >= int(kGunSlots)) return;
    const unsigned player = unsigned(mouseGunPlayer_);
    if (mouseGunButtons_) {
        mouseGunButtons_ = 0;
        emit gunButtonsChanged(player, 0);
    }
    auto& gun = guns_[player];
    if (!gun.offscreen) {
        gun.offscreen = true;
        emit gunAimChanged(player, gun.aim.x(), gun.aim.y(), true);
        repaintView();
    }
}

void GameView::mousePressEvent(QMouseEvent* event) {
    if (!mouseCaptured_) pointerActivity(event->position());
    if (!loading_ && frame_.isNull() && event->button() == Qt::LeftButton &&
        openArea().contains(event->position().toPoint())) {
        emit openGameRequested();
        event->accept();
        return;
    }
    const unsigned bit = unsigned(event->button()) & unsigned(Qt::LeftButton | Qt::RightButton |
        Qt::MiddleButton | Qt::BackButton | Qt::ForwardButton);
    if (mouseCaptureEnabled_ && !mouseCaptured_ && bit &&
        (mouseGunAvailable() || analogMouseAvailable())) {
        // The activation click belongs to the window, not to the emulated device.
        if (event->button() == Qt::LeftButton && gameRect().contains(event->position())) beginMouseCapture();
        event->accept(); return;
    }
    if (mouseGunAvailable() && bit) {
        updateMouseAim(event->position());
        if (!(mouseGunButtons_ & bit)) {
            mouseGunButtons_ |= bit;
            emit gunButtonsChanged(unsigned(mouseGunPlayer_), mouseGunButtons_);
        }
        event->accept();
        return;
    }
    if (analogMouseAvailable() && bit && (mouseCaptured_ || gameRect().contains(event->position()))) {
        updateAnalogPointer(event->position());
        if (!(analogMouseButtons_ & bit)) {
            analogMouseButtons_ |= bit;
            emit gunButtonsChanged(unsigned(analogMousePlayer_), analogMouseButtons_);
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void GameView::mouseReleaseEvent(QMouseEvent* event) {
    if (!mouseCaptured_) pointerActivity(event->position());
    const unsigned bit = unsigned(event->button()) & unsigned(Qt::LeftButton | Qt::RightButton |
        Qt::MiddleButton | Qt::BackButton | Qt::ForwardButton);
    if (mouseGunPlayer_ >= 0 && mouseGunPlayer_ < int(kGunSlots) && bit && (mouseGunButtons_ & bit)) {
        mouseGunButtons_ &= ~bit;
        if (mouseGunAvailable()) updateMouseAim(event->position());
        emit gunButtonsChanged(unsigned(mouseGunPlayer_), mouseGunButtons_);
        event->accept();
        return;
    }
    if (analogMousePlayer_ >= 0 && analogMousePlayer_ < int(kGunSlots) &&
        bit && (analogMouseButtons_ & bit)) {
        analogMouseButtons_ &= ~bit;
        if (analogMouseAvailable()) updateAnalogPointer(event->position());
        emit gunButtonsChanged(unsigned(analogMousePlayer_), analogMouseButtons_);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void GameView::mouseMoveEvent(QMouseEvent* event) {
    if (mouseCaptured_) {
        if (!waylandMouse_->active()) {
            const QPointF delta = event->globalPosition() - QPointF(captureAnchor_);
            capturedMouseMotion(delta.x(), delta.y());
            if (!delta.isNull()) QCursor::setPos(captureAnchor_);
        }
        event->accept();
        return;
    }
    pointerActivity(event->position());
    if (!mouseCaptureEnabled_) {
        if (mouseGunAvailable()) updateMouseAim(event->position());
        if (analogMouseAvailable()) updateAnalogPointer(event->position());
    }
    QWidget::mouseMoveEvent(event);
}

void GameView::leaveEvent(QEvent* event) {
    cursorInside_ = false;
    resetIdleCursor(false);
    if (mouseCaptured_) { QWidget::leaveEvent(event); return; }
    clearMouseGun();
    clearAnalogMouse();
    QWidget::leaveEvent(event);
}

void GameView::focusOutEvent(QFocusEvent* event) {
    releaseMouseCapture();
    clearMouseGun();
    clearAnalogMouse();
    resetIdleCursor(false);
    QWidget::focusOutEvent(event);
}

void GameView::paintCrosshairs(QPainter& painter) {
    if (frame_.isNull() || loading_ || capturingReticles_) return;
    const bool visible = std::any_of(std::begin(guns_), std::end(guns_),
        [](const GunState& gun) { return gun.enabled && gun.crosshairVisible && gun.hasAim && !gun.offscreen; });
    if (!visible) return;
    const QRectF game = gameRect();
    if (game.isEmpty()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(game, Qt::IntersectClip);
    unsigned drawn = 0;
    for (unsigned player = 0; player < kGunSlots && drawn < 2; ++player) {
        const auto& gun = guns_[player];
        if (!gun.enabled || !gun.crosshairVisible || !gun.hasAim || gun.offscreen) continue;
        ++drawn;
        const QPointF point(game.left() + gun.aim.x() * game.width(),
                            game.top() + gun.aim.y() * game.height());
        const QColor color = gun.number == 1 ? QColor(66, 228, 238) : QColor(255, 135, 112);
        const auto drawMarks = [&painter, &point] {
            painter.drawEllipse(point, 8, 8);
            painter.drawLine(QPointF(point.x() - 17, point.y()), QPointF(point.x() - 11, point.y()));
            painter.drawLine(QPointF(point.x() + 11, point.y()), QPointF(point.x() + 17, point.y()));
            painter.drawLine(QPointF(point.x(), point.y() - 17), QPointF(point.x(), point.y() - 11));
            painter.drawLine(QPointF(point.x(), point.y() + 11), QPointF(point.x(), point.y() + 17));
        };
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 235), 5, Qt::SolidLine, Qt::RoundCap));
        drawMarks();
        painter.setPen(QPen(color, 2, Qt::SolidLine, Qt::RoundCap));
        drawMarks();

    }
    painter.restore();
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
                         Localization::text(QStringLiteral("Нажмите + или Ctrl+O · перетащите игру сюда")));
        painter.drawText(QRect(24, center.y() + 51, width() - 48, 26),
                         Qt::AlignCenter,
                         Localization::text(QStringLiteral("Поддержка: MD, GEN, BIN, SMD, SMS, GG, SG, 68K, CUE, ISO, CHD, M3U")));
    } else {
        const QRectF target = gameRect();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(target, frame_);
    }
}


GameView::~GameView() {
    cursorInside_ = false;
    resetIdleCursor(false);
    releaseMouseCapture();
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
    paintOverlays(painter);
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

bool GameView::idleCursorAllowed() const {
    if (cursorIdleTimer_.interval() <= 0 || mouseCaptured_ || !cursorInside_ || !isVisible() || !isEnabled() || !isActiveWindow()
        || loading_ || QApplication::activePopupWidget() || QApplication::activeModalWidget()) return false;
    if (window()->isFullScreen()) return rect().contains(cursorPosition_.toPoint());
    return !frame_.isNull() && gameRect().contains(cursorPosition_);
}

void GameView::restoreIdleCursor() {
    if (!cursorIdleHidden_) return;
    cursorIdleHidden_ = false;
    const QScopedValueRollback<bool> changing(changingIdleCursor_, true);
    if (cursorWasExplicit_) setCursor(cursorBeforeIdle_);
    else unsetCursor();
}

void GameView::resetIdleCursor(bool arm) {
    cursorIdleTimer_.stop();
    restoreIdleCursor();
    if (arm && idleCursorAllowed() && cursor().shape() != Qt::BlankCursor)
        cursorIdleTimer_.start();
}

void GameView::pointerActivity(const QPointF& position) {
    cursorPosition_ = position;
    cursorInside_ = rect().contains(position.toPoint());
    resetIdleCursor();
}

bool GameView::eventFilter(QObject* watched, QEvent* event) {
    // The transparent GPU child normally sends pointer input to GameView. Also
    // handle direct child enter/move events without changing controller input.
    if (watched == shaderView_) {
        if (event->type() == QEvent::MouseMove && !mouseCaptured_)
            pointerActivity(shaderView_->mapTo(this, static_cast<QMouseEvent*>(event)->position().toPoint()));
        else if (event->type() == QEvent::Enter && !mouseCaptured_) {
            const auto* enter = dynamic_cast<QEnterEvent*>(event);
            pointerActivity(enter ? shaderView_->mapTo(this, enter->position().toPoint())
                                  : mapFromGlobal(QCursor::pos()));
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool GameView::event(QEvent* event) {
    if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate) {
        releaseMouseCapture();
        resetIdleCursor(false);
        if (event->type() == QEvent::Hide) cursorInside_ = false;
    }
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::WindowActivate)
        resetIdleCursor();
    if (event->type() == QEvent::CursorChange && cursorIdleHidden_ && !changingIdleCursor_) {
        // An explicit controller cursor change takes ownership from idle hiding.
        cursorIdleHidden_ = false;
        cursorIdleTimer_.stop();
    }
    if (event->type() == QEvent::UngrabMouse && mouseCaptured_ && !waylandMouse_->active())
        releaseMouseCapture();
    if (mouseCaptured_ && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
            if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) releaseMouseCapture();
            event->accept(); return true;
        }
    }
    if (event->type() == QEvent::Enter) {
        resetAnalogPointer();
        const auto* enter = dynamic_cast<QEnterEvent*>(event);
        pointerActivity(enter ? enter->position() : QPointF(mapFromGlobal(QCursor::pos())));
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (event->type() == QEvent::DevicePixelRatioChange) {
#else
    // Qt 6.4 forwards screen changes to every child widget through this event.
    if (event->type() == QEvent::ScreenChangeInternal) {
#endif
        resetAnalogPointer();
        if (!achievementQueue_.isEmpty()) rebuildAchievementPixmap();
    }
    return QWidget::event(event);
}

void GameView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!achievementQueue_.isEmpty()) rebuildAchievementPixmap();
    ensureGpu();
}

void GameView::resizeEvent(QResizeEvent* event) {
    releaseMouseCapture();
    QWidget::resizeEvent(event);
    resetIdleCursor();
    resetAnalogPointer();
    if (!achievementQueue_.isEmpty()) rebuildAchievementPixmap();
    if (shaderView_) shaderView_->setGeometry(rect());
}

void GameView::gpuFailed(const QString& error) {
    const bool hadShader = !shaderPreset_.path.isEmpty();
    shaderPreset_ = {};
    spriteOwnership_ = {};
    emit spriteOwnershipRequested(false);
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
    spriteOwnership_ = {};
    emit spriteOwnershipRequested(shaderPreset_.usesSpriteOwnership());
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
