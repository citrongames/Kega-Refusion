#pragma once

#include "script/ScriptUiTypes.h"

#include <QList>
#include <QObject>

class QWidget;

class ScriptPanelManager final : public QObject {
    Q_OBJECT

public:
    explicit ScriptPanelManager(QWidget* parent);
    ~ScriptPanelManager() override;

    void applySnapshot(const ScriptUiSnapshot& snapshot);
    void rejectEvent(const ScriptUiEvent& event);
    bool ownsWindow(const QWidget* widget) const;

signals:
    void eventRequested(const ScriptUiEvent& event);

private:
    class PanelWindow;

    QWidget* const parentWindow_;
    QList<PanelWindow*> windows_;
    quint64 revision_ = 0, epoch_ = 0, sequence_ = 0;
    bool receivedSnapshot_ = false;
};
