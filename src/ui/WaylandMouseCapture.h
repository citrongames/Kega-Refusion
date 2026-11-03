#pragma once

#include <QObject>
#include <memory>

class QWidget;

// Relative pointer capture for a visible Qt Wayland widget. The public API is
// available on every platform; unsupported platforms simply return false.
class WaylandMouseCapture final : public QObject {
    Q_OBJECT
public:
    explicit WaylandMouseCapture(QObject* parent = nullptr);
    ~WaylandMouseCapture() override;
    bool supported() const;
    bool begin(QWidget* target);
    void end();
    bool active() const;
signals:
    void motion(double dx, double dy);
    void lost();
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
