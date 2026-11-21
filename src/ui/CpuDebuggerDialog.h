#pragma once

#include "core/DebugProject.h"

#include <QDialog>
#include <QHash>
#include <QMap>
#include <array>
#include <optional>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QHideEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTabWidget;

class CpuDebuggerDialog final : public QDialog {
    Q_OBJECT
public:
    explicit CpuDebuggerDialog(QWidget* parent = nullptr);
    DebuggerCommand refreshCommand() const;
    void setSnapshot(const DebuggerSnapshot& snapshot);
    void navigateTo(quint32 cpu, quint32 address);
    DebugProject exportProject() const;
    void applyProject(const DebugProject& project);
    void clearProject();
    void cancelProjectImport();
    bool projectPending() const;
    QString projectError() const;

signals:
    void command(const DebuggerCommand& command);
    void projectChanged();
    void projectStateChanged();
    void projectApplied(bool accepted);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct Symbol {
        QString label;
        QString comment;
        bool bookmarked = false;
        bool operator==(const Symbol& other) const {
            return label == other.label && comment == other.comment && bookmarked == other.bookmarked;
        }
    };

    struct PendingConfiguration {
        QVector<DebuggerBreakpoint> breakpoints;
        std::optional<QVector<DebugProjectSymbol>> importedSymbols;
        quint64 requestId = 0;
        quint32 editedId = 0;
        bool edited = false, resetHits = false, validateOnly = false;
    };

    void send(DebuggerCommand::Kind action);
    void goToInput();
    void followProgramCounter();
    void updateControls();
    void updateCpuChoices();
    void updateBreakEventAvailability();
    QString cpuName(quint32 cpu) const;
    int addressDigits(quint32 cpu) const;
    quint32 addressMask(quint32 cpu) const;
    void updateCode();
    void copyDisassembly();
    void updateState();
    void updateFlags(const kr_debug_cpu* state, const kr_debug_target* target);
    bool canEditRegister(const QString& name) const;
    void editRegister(int row);
    void editFlag(size_t flag, bool checked);
    void writeRegister(const QString& name, quint32 value, quint32 mask);
    void updateBreakpoints();
    void editBreakpoint(const DebuggerBreakpoint* breakpoint);
    void saveBreakpoint();
    void removeBreakpoint();
    void toggleExecuteBreakpoint(int row);
    void configure(QVector<DebuggerBreakpoint> breakpoints, quint32 editedId = 0);
    void sendPendingConfiguration();
    void reconcileConfiguration();
    void publishProjectState();
    void replaceSymbols(const QVector<DebugProjectSymbol>& symbols);
    qsizetype symbolCount() const;
    DebuggerBreakpoint displayedBreakpoint(const DebuggerBreakpoint& breakpoint) const;
    void importSymbols();
    void showCodeMenu(const QPoint& point);
    void updateBookmarks();

    QTabWidget* pages_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* feedback_ = nullptr;
    QLabel* cpuLabel_ = nullptr;
    QComboBox* cpu_ = nullptr;
    QCheckBox* link_ = nullptr;
    std::array<QPushButton*, 5> execution_{};
    QLineEdit* address_ = nullptr;
    QPushButton* go_ = nullptr;
    QPushButton* follow_ = nullptr;
    QPushButton* import_ = nullptr;
    QComboBox* bookmarks_ = nullptr;
    QTableWidget* code_ = nullptr;
    QTabWidget* stateTabs_ = nullptr;
    QTableWidget* registers_ = nullptr;
    QGroupBox* flags_ = nullptr;
    std::array<QCheckBox*, 8> flagBits_{};
    QPlainTextEdit* stack_ = nullptr;
    QTableWidget* breakpoints_ = nullptr;
    QWidget* breakpointEditor_ = nullptr;
    QComboBox* breakCpu_ = nullptr;
    QCheckBox* breakEnabled_ = nullptr;
    std::array<QCheckBox*, 3> breakEvents_{};
    QLineEdit* breakFirst_ = nullptr;
    QLineEdit* breakLast_ = nullptr;
    QLineEdit* breakCondition_ = nullptr;
    QLineEdit* breakIgnore_ = nullptr;
    QCheckBox* breakOnce_ = nullptr;
    QPushButton* breakNew_ = nullptr;
    QPushButton* breakSave_ = nullptr;
    QPushButton* breakRemove_ = nullptr;

    DebuggerSnapshot snapshot_;
    QVector<DebuggerBreakpoint> savedBreakpoints_;
    std::optional<PendingConfiguration> pendingConfiguration_;
    std::optional<DebugProject> queuedProject_;
    QHash<quint32, QMap<quint32, Symbol>> symbols_;
    QString gameKey_;
    quint64 generation_ = 0;
    quint64 nextConfigureRequest_ = 0, lastConfigureResult_ = 0;
    quint32 cpuId_ = KR_DEBUG_M68K;
    quint32 addressValue_ = 0;
    quint32 editingBreakpoint_ = 0;
    bool followPc_ = true;
    bool needsConfiguration_ = false, resetNextHits_ = false;
    bool reportedProjectPending_ = false;
};
