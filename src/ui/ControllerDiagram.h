#pragma once

#include "../input/InputManager.h"

#include <QPainterPath>
#include <QPixmap>
#include <QRectF>
#include <QSet>
#include <QTransform>
#include <QVector>
#include <QWidget>

class QPainter;

class ControllerDiagram final : public QWidget {
    Q_OBJECT
public:
    explicit ControllerDiagram(QWidget* parent = nullptr);
    QSize sizeHint() const override;
    void setProfile(ConsoleProfile profile);
    void setSixButton(bool sixButton);
    void setControllerType(ControllerType type);
    void setTargets(const QVector<int>& targets);
    void setSelectedTarget(int target, bool awaitingInput);
    void setPressedMask(unsigned mask);

signals:
    void targetClicked(int target);
    void targetDoubleClicked(int target);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    struct TargetArea {
        int target;
        QPainterPath shape;
        QPainterPath hit;
    };
    void updateIllustration();
    void rebuildTargets();
    void addArea(int target, const QPainterPath& shape, qreal padding = 24);
    void addCircle(int target, QPointF center, qreal radius);
    void addEllipse(int target, QPointF center, qreal rx, qreal ry);
    void addDpad(QPointF center, qreal reach);
    void drawHighlight(QPainter& painter, int target, const QPainterPath& shape) const;
    QRectF illustrationArea() const;
    QRectF consolePauseArea() const;
    QRectF extraButtonsArea() const;
    QRectF extraButtonArea(int target) const;
    QRectF reloadArea() const;
    QTransform drawingTransform() const;
    int targetAt(const QPointF& point) const;

    ConsoleProfile profile_ = ConsoleProfile::MegaDrive;
    bool sixButton_ = true;
    ControllerType controllerType_ = ControllerType::Pad;
    QString illustrationPath_;
    QPixmap illustration_;
    QSet<int> targets_;
    int selectedTarget_ = -1;
    bool awaitingInput_ = false;
    unsigned pressedMask_ = 0;
    QVector<TargetArea> areas_;
};
