#pragma once

#include "ShaderPipeline.h"

#include <QElapsedTimer>
#include <QImage>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QPainter;
class QMouseEvent;
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
    QImage captureImage();
    void setAspectMode(AspectMode mode);
    void setIntegerScaling(bool enabled);
    void showOsd(const QString& message, int durationMs = 2600,
                 const QString& category = {});
    void showStateSlots(int selected, unsigned occupiedMask, int durationMs = 2000);
    void setOsdEnabled(bool enabled);
    void setPausedStatus(bool paused);
    void setFastForwardStatus(bool enabled);
    void setFastForwardFactor(int factor);
    void setRewindStatus(bool enabled);

signals:
    void shaderFailed(const QString& error);
    void openGameRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    friend class ShaderView;
    void repaintView();
    void ensureGpu();
    void gpuFailed(const QString& error);
    QRectF gameRect() const;
    QRect openArea() const;
    void paintRaster(QPainter& painter);
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

    QImage frame_;
    double aspectRatio_ = 4.0 / 3.0;
    AspectMode aspectMode_ = AspectMode::Console;
    bool integerScaling_ = false;
    bool osdEnabled_ = true;
    bool paused_ = false;
    bool fastForward_ = false;
    int fastForwardFactor_ = 4;
    bool rewinding_ = false;
    StateSlots stateSlots_;
    QVector<Notice> notices_;
    QElapsedTimer osdClock_;
    QTimer osdTimer_;
    QString osdFamily_;
};
