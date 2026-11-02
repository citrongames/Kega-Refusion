#pragma once

#include "ShaderPipeline.h"

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QPainter;
class QMouseEvent;
class QFocusEvent;
class ShaderView;

class GameView final : public QWidget {
    Q_OBJECT
public:
    enum class AspectMode { Console, SquarePixels, Stretch };

    explicit GameView(QWidget* parent = nullptr);
    ~GameView() override;
    bool setShaderPreset(const QString& path, QString* error = nullptr);
    QString shaderPreset() const { return shaderPreset_.path; }
    QVector<ShaderParameter> shaderParameters() const { return shaderPreset_.parameters; }
    bool setShaderParameter(const QString& id, float value);
    void setFrame(const QImage& frame, double aspectRatio);
    void clearGame();
    void setLoading(bool loading, const QString& fileName = {});
    QImage captureImage();
    QImage captureGameImage();
    void setAspectMode(AspectMode mode);
    void setIntegerScaling(bool enabled);
    void showOsd(const QString& message, int durationMs = 2600,
                 const QString& category = {});
    void showStateSlots(int selected, unsigned occupiedMask, int durationMs = 2000);
    void showAchievementNotice(const QString& heading, const QString& title,
                               const QString& description, const QString& badgeUrl,
                               bool sound = false);
    void setAchievementBadge(const QString& url, const QImage& image);
    void clearAchievementNotices();
    void setAchievementPosition(int position); // 0..5: top L/C/R, bottom L/C/R.
    void setOsdEnabled(bool enabled);
    void setPerformanceMetricsEnabled(bool enabled);
    void setGameFpsEnabled(bool enabled);
    void setPerformanceMetrics(double emulatedFps, double speedPercent, double gameFps, bool gameFpsAvailable);
    void setPausedStatus(bool paused);
    void setMutedStatus(bool muted);
    void setFastForwardStatus(bool enabled);
    void setFastForwardFactor(int factor);
    void setRewindStatus(bool enabled);
    void setRewindProgress(qint64 remainingMs, qint64 totalMs);
    void setMovieCounterAsTime(bool enabled);
    void setMovieStatus(int mode, quint64 frame, quint64 length, double fps);
    void setLightgunEnabled(unsigned player, bool enabled, unsigned gunNumber = 1);
    // Aim coordinates are normalized to [0, 1] within the displayed gameRect.
    void setCrosshairVisible(unsigned player, bool visible);
    void setLightgunAim(unsigned player, QPointF normalized, bool offscreen = false);
    void setMouseGunPlayer(int player); // -1 disables mouse aim; 0/1 select console ports; 2 selects the chained second Justifier.
    void setLightgunInputEnabled(bool enabled);

signals:
    void shaderFailed(const QString& error);
    void openGameRequested();
    void achievementPopupStarted(bool sound);
    void achievementBadgeRequested(const QString& url);
    void gunAimChanged(unsigned player, double x, double y, bool offscreen);
    // Raw Qt::MouseButton mask; the input layer maps buttons for each gun device.
    void gunButtonsChanged(unsigned player, unsigned qtButtons);

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    friend class ShaderView;
    void repaintView();
    void ensureGpu();
    void gpuFailed(const QString& error);
    QRectF gameRect() const;
    QRect openArea() const;
    void paintRaster(QPainter& painter);
    void paintCrosshairs(QPainter& painter);
    bool mouseGunAvailable() const;
    void updateMouseAim(const QPointF& position);
    void clearMouseGun();
    ShaderView* shaderView_ = nullptr;
    ShaderPreset shaderPreset_;
    bool gpuFailed_ = false;
    bool gpuCreating_ = false;
    quint64 frameCount_ = 0;

    struct Notice {
        QString text;
        QString category;
        qint64 expiresAt = 0;
    };
    struct AchievementPopup {
        QString heading;
        QString title;
        QString description;
        QString badgeUrl;
        QImage badge;
        bool sound = false;
    };
    struct StateSlots {
        int selected = 0;
        unsigned occupiedMask = 0;
        qint64 expiresAt = 0;
    };
    void expireOsd();
    void startAchievementPopup();
    void rebuildAchievementPixmap();
    QRect achievementPopupRect() const;
    void paintOsd(QPainter& painter);
    void rebuildPerformanceMetrics();
    void paintLoading(QPainter& painter);

    QImage frame_;
    double aspectRatio_ = 4.0 / 3.0;
    AspectMode aspectMode_ = AspectMode::Console;
    bool integerScaling_ = false;
    bool osdEnabled_ = true;
    bool performanceMetricsEnabled_ = false;
    bool gameFpsEnabled_ = false;
    bool metricsReady_ = false;
    bool gameFpsAvailable_ = false;
    double emulatedFps_ = 0.0;
    double gameFps_ = 0.0;
    QStringList metricsLines_;
    bool capturingGame_ = false;
    bool capturingReticles_ = false;
    struct GunState {
        QPointF aim{0.5, 0.5};
        bool enabled = false;
        bool crosshairVisible = true;
        bool hasAim = false;
        bool offscreen = true;
        unsigned number = 1; // Display number is independent of the input slot.
    };
    static constexpr unsigned kGunSlots = 3;
    GunState guns_[kGunSlots];
    int mouseGunPlayer_ = -1;
    unsigned mouseGunButtons_ = 0;
    bool lightgunInputEnabled_ = false;
    bool loading_ = false;
    QString loadingFileName_;
    bool paused_ = false;
    bool muted_ = false;
    bool fastForward_ = false;
    int fastForwardFactor_ = 4;
    bool rewinding_ = false;
    int movieMode_ = 0;
    quint64 movieFrame_ = 0, movieLength_ = 0;
    double movieFps_ = 60.0;
    bool movieCounterAsTime_ = true;
    QString rewindTime_;
    StateSlots stateSlots_;
    QVector<Notice> notices_;
    QVector<AchievementPopup> achievementQueue_;
    QPixmap achievementPixmap_;
    QImage achievementFallbackBadge_;
    QSize achievementCardSize_;
    qint64 achievementExpiresAt_ = 0;
    int achievementPosition_ = 0;
    QElapsedTimer osdClock_;
    QTimer osdTimer_;
    QString osdFamily_;
};
