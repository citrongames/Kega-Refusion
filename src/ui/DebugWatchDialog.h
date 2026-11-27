#pragma once

#include "core/DebugInspection.h"

#include <QDialog>

class DebugWatchModel;
class QHideEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QTableView;

class DebugWatchDialog final : public QDialog {
    Q_OBJECT
public:
    explicit DebugWatchDialog(QWidget* parent = nullptr);
    QVector<DebugWatchEntry> entries() const;
    QVector<DebugWatchEntry> scriptEntries() const;
    void setEntries(const QVector<DebugWatchEntry>& entries);
    void setScriptEntries(const QVector<DebugWatchEntry>& entries);
    void addWatch(const DebugWatchEntry& entry);
    void setSnapshot(const DebugInspectionSnapshot& snapshot);

signals:
    void inspectionChanged();
    void entriesChanged();
    void navigateRequested(quint32 instance, quint32 domain, quint32 offset);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void editWatch(int row);
    void removeWatch();
    void navigate(int row);
    bool canNavigate(int row) const;
    void showTableMenu(const QPoint& point);
    void updateActions();
    void resetSamples();
    quint32 unusedId();
    void showFeedback(const QString& message);

    QTableView* table_ = nullptr;
    DebugWatchModel* model_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* edit_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* memory_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* feedback_ = nullptr;
    QVector<DebugWatchDomain> domains_;
    QVector<DebugWatchEntry> scriptDeclarations_;
    QString gameKey_;
    quint64 generation_ = 0;
    quint32 activeInstance_ = 0;
    quint32 activeDomain_ = KR_MEM_RAM;
    quint32 nextId_ = 1;
};
