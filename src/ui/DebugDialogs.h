#pragma once

#include "core/DebugInspection.h"

#include <QColor>
#include <QDialog>
#include <QtGlobal>
#include <array>

class DebugStateInspector;
class DebugVideoInspector;
class QCheckBox;
class QColorDialog;
class QGroupBox;
class QHideEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QSlider;
class QSpinBox;
class QTabWidget;

class DebugGraphicsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit DebugGraphicsDialog(QWidget* parent = nullptr);
    void setControls(quint32 available, quint32 disabled, bool hardcore,
                     bool ready, QColor backdrop, bool firecore = false);
    DebugInspectionRequest request() const;
    void setSnapshot(const DebugInspectionSnapshot& snapshot);

signals:
    void maskChanged(quint32 disabled);
    void backdropChanged(quint32 rgb);
    void inspectionChanged();
    void memoryRequested(quint32 domain, quint32 offset);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void updateBackdropIcon();
    void showColorPicker();

    QTabWidget* tabs_ = nullptr;
    std::array<DebugVideoInspector*, 5> views_{};
    DebugStateInspector* registers_ = nullptr;
    std::array<QCheckBox*, 6> layers_{};
    QLabel* placeholder_ = nullptr;
    QPushButton* restore_ = nullptr;
    QPushButton* backdropButton_ = nullptr;
    QPushButton* resetBackdrop_ = nullptr;
    QColorDialog* colorDialog_ = nullptr;
    quint32 available_ = 0;
    quint32 disabled_ = 0;
    QColor backdrop_{255, 0, 255};
};

class DebugAudioDialog final : public QDialog {
    Q_OBJECT
public:
    explicit DebugAudioDialog(QWidget* parent = nullptr);
    void setControls(quint64 available, quint64 disabled, bool ready,
                     const std::array<unsigned, 38>& gains);
    DebugInspectionRequest request() const;
    void setSnapshot(const DebugInspectionSnapshot& snapshot);

signals:
    void maskChanged(quint64 disabled);
    void gainChanged(unsigned channel, unsigned percent);
    void inspectionChanged();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QTabWidget* tabs_ = nullptr;
    DebugStateInspector* state_ = nullptr;
    std::array<QGroupBox*, 8> groups_{};
    QLabel* placeholder_ = nullptr;
    std::array<QWidget*, 38> rows_{};
    std::array<QCheckBox*, 38> channels_{};
    std::array<QSlider*, 38> sliders_{};
    std::array<QSpinBox*, 38> percents_{};
    QPushButton* restore_ = nullptr;
    QPushButton* resetGains_ = nullptr;
    quint64 available_ = 0;
    quint64 disabled_ = 0;
    std::array<unsigned, 38> gains_{};
};
