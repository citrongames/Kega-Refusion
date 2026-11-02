#pragma once

#include "ShaderPipeline.h"

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QString>
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
    void setOsdEnabled(bool enabled);
    void setPausedStatus(bool paused);
    void setMutedStatus(bool muted);
    void setFastForwardStatus(bool enabled);
    void setFastForwardFactor(int factor);
    void setRewindStatus(bool enabled);
    void setRewindProgress(qint64 remainingMs, qint64 totalMs);
    void setLightgunEnabled(unsigned player, bool enabled, unsigned gunNumber = 1);
    // Aim coordinates are normalized to [0, 1] within the displayed gameRect.
    void setLightgunAim(unsigned player, QPointF normalized, bool offscreen = false);
    void setMouseGunPlayer(int player); // -1 disables mouse aim; 0/1 select console ports; 2 selects the chained second Justifier.
    void setLightgunInputEnabled(bool enabled);

signals:
    void shaderFailed(const QString& error);
    void openGameRequested();
    void gunAimChanged(unsigned player, double x, double y, bool offscreen);
    // Raw Qt::MouseButton mask; the input layer maps buttons for each gun device.
    void gunButtonsChanged(unsigned player, unsigned qtButtons);

protected:
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
    struct StateSlots {
        int selected = 0;
        unsigned occupiedMask = 0;
        qint64 expiresAt = 0;
    };
    void expireOsd();
    void paintOsd(QPainter& painter);
    void paintLoading(QPainter& painter);

    QImage frame_;
    double aspectRatio_ = 4.0 / 3.0;
    AspectMode aspectMode_ = AspectMode::Console;
    bool integerScaling_ = false;
    bool osdEnabled_ = true;
    bool capturingGame_ = false;
    bool capturingReticles_ = false;
    struct GunState {
        QPointF aim{0.5, 0.5};
        bool enabled = false;
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
    QString rewindTime_;
    StateSlots stateSlots_;
    QVector<Notice> notices_;
    QElapsedTimer osdClock_;
    QTimer osdTimer_;
    QString osdFamily_;
};
