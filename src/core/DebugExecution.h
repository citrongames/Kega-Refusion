#pragma once
#include "DebugExecutionAbi.h"
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>
#include <QtGlobal>

// UI messages and immutable copies only; all execution belongs to the worker.
struct DebuggerBreakpoint {
    quint32 id = 0, cpu = 1, events = 1, first = 0, last = 0;
    QString condition;
    quint64 ignoreHits = 0, hits = 0;
    bool enabled = true, oneShot = false;
};
struct DebuggerCommand {
    enum Kind { Attach, Detach, Pause, Continue, StepInto, StepOver, StepOut,
                Refresh, Configure, Navigate };
    Kind kind = Refresh;
    quint32 cpu = 1, address = 0;
    bool followPc = true;
    QVector<DebuggerBreakpoint> breakpoints;
    quint32 editedBreakpointId = 0;
};
struct DebuggerSnapshot {
    quint64 generation = 0, frame = 0;
    bool attached = false, stopped = false, partialFrame = false;
    quint32 selectedCpu = 1, eventCpu = 0, event = 0, address = 0, value = 0, accessSize = 0;
    QString status;
    QVector<kr_debug_cpu> cpus;
    QVector<kr_debug_instruction> instructions;
    QByteArray stack, stackValid;
    quint32 stackAddress = 0;
    QVector<DebuggerBreakpoint> breakpoints;
    QString gameKey;
};
