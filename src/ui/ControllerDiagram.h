#pragma once

#include "../input/InputManager.h"

#include <QRectF>
#include <QSet>
#include <QVector>
#include <QWidget>

class ControllerDiagram final : public QWidget {
    Q_OBJECT
public:
    explicit ControllerDiagram(QWidget* parent = nullptr);
    QSize sizeHint() const override;
    void setProfile(ConsoleProfile profile);
    void setTargets(const QVector<int>& targets);
    void setSelectedTarget(int target);
    void setPressedMask(unsigned mask);

signals:
    void targetClicked(int target);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void addHit(int target, const QRectF& area);
    void drawDpad(QPainter& painter, QPointF center, qreal size);
    void drawButton(QPainter& painter, int target, QPointF center, qreal radius,
                    const QString& label, const QColor& face);
    void drawSmallButton(QPainter& painter, int target, const QRectF& area,
                         const QString& label);
    QColor faceFor(int target, const QColor& normal) const;
    QTransform drawingTransform() const;

    ConsoleProfile profile_ = ConsoleProfile::MegaDrive;
    QSet<int> targets_;
    int selectedTarget_ = -1;
    unsigned pressedMask_ = 0;
    QVector<QPair<int, QRectF>> hitAreas_;
};
