#pragma once

#include "core/DebugTrace.h"

#include <QDialog>
#include <array>

class QCheckBox;
class QComboBox;
class QHideEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QSpinBox;
class QTableView;
class TraceTableModel;

class TraceDialog final : public QDialog {
    Q_OBJECT
public:
    explicit TraceDialog(QWidget* parent = nullptr);
    void setSnapshot(const DebugTraceSnapshot& snapshot);
    bool hasRecords() const { return snapshot_.total != 0; }

signals:
    void commandRequested(const DebugTraceCommand& command);
    void navigateRequested(quint32 cpu, quint32 address);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void updateControls();
    void startCapture();
    void send(DebugTraceCommand::Kind kind);
    QString chooseNewFile(const QString& title, const QString& initial = {});
    void showFeedback(const QString& text);

    QLabel* status_ = nullptr;
    QLabel* feedback_ = nullptr;
    QComboBox* cpu_ = nullptr;
    std::array<QCheckBox*, 7> filters_{};
    QSpinBox* memory_ = nullptr;
    QCheckBox* captureFile_ = nullptr;
    QLineEdit* file_ = nullptr;
    QPushButton* filePick_ = nullptr;
    QPushButton* start_ = nullptr;
    QPushButton* stop_ = nullptr;
    QPushButton* clear_ = nullptr;
    QPushButton* export_ = nullptr;
    QTableView* table_ = nullptr;
    TraceTableModel* model_ = nullptr;

    DebugTraceSnapshot snapshot_;
    quint32 filterMask_ = DebugTraceConfig{}.filters;
    bool commandPending_ = false;
};
