#pragma once

#include <QImage>
#include <QWidget>

class GameView final : public QWidget {
public:
    enum class AspectMode { Console, SquarePixels, Stretch };

    explicit GameView(QWidget* parent = nullptr);
    void setFrame(const QImage& frame, double aspectRatio);
    void setAspectMode(AspectMode mode);
    void setIntegerScaling(bool enabled);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage frame_;
    double aspectRatio_ = 4.0 / 3.0;
    AspectMode aspectMode_ = AspectMode::Console;
    bool integerScaling_ = false;
};
