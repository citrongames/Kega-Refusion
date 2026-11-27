#pragma once

#include "script/ScriptTypes.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <functional>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class LuaSourceEditor;

class LuaScriptsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit LuaScriptsDialog(QWidget* parent = nullptr);

    void applyUpdates(const QList<ScriptUpdate>& updates);
    void rejectCommand(const ScriptCommand& command, const QString& reason);
    void setExecutionAvailable(bool available);
    void setMachineRevisionProvider(std::function<quint64()> provider);
    void discardPendingStarts();
    bool confirmDiscardEdits();

signals:
    void commandRequested(const ScriptCommand& command);

private:
    enum class Activity { Idle, Opening, Preparing, Saving, Starting, Stopping };

    struct Entry {
        quint64 id;
        QString path;
        QTreeWidgetItem* item;
        QByteArray source;
        QByteArray diskSource;
        Activity activity = Activity::Opening;
        ScriptStatus status = ScriptStatus::Stopped;
        bool dirty = false;
        bool loaded = false;
        bool stoppingPendingStart = false;
    };

    Entry* entry(quint64 id);
    Entry* selected();
    void selectScript(QTreeWidgetItem* item);
    void storeEditor();
    void showSource(const Entry* entry);
    void openScript();
    void addScript(const QString& path);
    void startScript();
    void stopScript();
    void removeScript();
    void saveScript(bool openEditor = false);
    void setEditorExpanded(bool expanded);
    void jumpToError(QListWidgetItem* item);
    void addLog(const ScriptUpdate& update);
    void setRowStatus(Entry& entry);
    void updateControls();

    QTreeWidget* scripts_ = nullptr;
    QListWidget* log_ = nullptr;
    QLabel* availability_ = nullptr;
    QPushButton* open_ = nullptr;
    QPushButton* run_ = nullptr;
    QPushButton* stop_ = nullptr;
    QPushButton* restart_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* external_ = nullptr;
    QToolButton* editorToggle_ = nullptr;
    QWidget* editorPanel_ = nullptr;
    LuaSourceEditor* editor_ = nullptr;

    QList<Entry> entries_;
    QString lastDirectory_;
    quint64 nextId_ = 1;
    quint64 editingId_ = 0;
    quint64 startGeneration_ = 0;
    std::function<quint64()> machineRevision_;
    bool executionAvailable_ = false;
    bool choosingFile_ = false;
    bool settingSource_ = false;
};
