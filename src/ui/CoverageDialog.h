#pragma once

#include "core/DebugCoverage.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QHideEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableView;
class CoveragePageModel;

class CoverageDialog final : public QDialog {
    Q_OBJECT
public:
    explicit CoverageDialog(QWidget* parent = nullptr);
    DebugCoverageCommand refreshCommand() const;
    void setSnapshot(const DebugCoverageSnapshot& snapshot);

signals:
    void commandRequested(const DebugCoverageCommand& command);
    void navigateRequested(quint32 instance, quint32 domain, quint32 offset);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    const DebugCoverageSource* selectedSource() const;
    DebugCoverageConfig configuration() const;
    bool canNavigate() const;
    void updateControls();
    void updatePage();
    void updateColumns();
    void changeOffset(quint32 address);
    void navigate();
    void send(DebugCoverageCommand::Kind kind, const QString& path = {});
    void exportCapture();
    void showFeedback(const QString& message);

    QComboBox* instance_ = nullptr;
    QComboBox* cpu_ = nullptr;
    QCheckBox* execution_ = nullptr;
    QCheckBox* reads_ = nullptr;
    QPushButton* start_ = nullptr;
    QPushButton* stop_ = nullptr;
    QPushButton* clear_ = nullptr;
    QPushButton* export_ = nullptr;
    QComboBox* source_ = nullptr;
    QLineEdit* offset_ = nullptr;
    QPushButton* previous_ = nullptr;
    QPushButton* next_ = nullptr;
    QPushButton* openMemory_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* feedback_ = nullptr;
    QTableView* table_ = nullptr;
    CoveragePageModel* model_ = nullptr;
    DebugCoverageSnapshot snapshot_;
    quint32 instanceId_ = 0;
    quint32 cpuId_ = 0;
    quint32 sourceId_ = 0;
    quint32 pageOffset_ = 0;
    quint32 selectedOffset_ = 0;
    bool wantExecution_ = true;
    bool wantReads_ = true;
    bool commandPending_ = false;
};
