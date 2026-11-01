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
    void setPausedStatus(bool paused);
    void setFastForwardStatus(bool enabled);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Notice {
        QString text;
        QString category;
        qint64 expiresAt = 0;
    };
    void expireOsd();
    void paintOsd(QPainter& painter);

    QImage frame_;
    double aspectRatio_ = 4.0 / 3.0;
    AspectMode aspectMode_ = AspectMode::Console;
    bool integerScaling_ = false;
    bool paused_ = false;
    bool fastForward_ = false;
    QVector<Notice> notices_;
    QElapsedTimer osdClock_;
    QTimer osdTimer_;
    QString osdFamily_;
};
