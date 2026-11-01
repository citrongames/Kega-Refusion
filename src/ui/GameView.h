#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QPainter;

class GameView final : public QWidget {
public:
    enum class AspectMode { Console, SquarePixels, Stretch };

    explicit GameView(QWidget* parent = nullptr);
    void setFrame(const QImage& frame, double aspectRatio);
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

protected:
    void paintEvent(QPaintEvent* event) override;

private:
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
