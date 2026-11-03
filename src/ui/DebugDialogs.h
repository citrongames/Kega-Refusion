#pragma once

#include <QColor>
#include <QDialog>
#include <QtGlobal>
#include <array>

class QCheckBox;
class QColorDialog;
class QGroupBox;
class QHideEvent;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;

class DebugGraphicsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit DebugGraphicsDialog(QWidget* parent = nullptr);
    void setControls(quint32 available, quint32 disabled, bool hardcore,
                     bool ready, QColor backdrop);

signals:
    void maskChanged(quint32 disabled);
    void backdropChanged(quint32 rgb);

protected:
    void hideEvent(QHideEvent* event) override;

private:
    void updateBackdropIcon();
    void showColorPicker();

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

signals:
    void maskChanged(quint64 disabled);
    void gainChanged(unsigned channel, unsigned percent);

private:
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
