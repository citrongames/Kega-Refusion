#pragma once
#include "DebugExecutionAbi.h"
#include "DebugCoverageAbi.h"
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
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
                Refresh, Configure, Navigate, SetLink, WriteRegister, Validate };
    Kind kind = Refresh;
    quint32 cpu = 1, address = 0;
    bool followPc = true;
    quint32 physicalSource = 0, physicalOffset = 0;
    quint64 navigationRequestId = 0;
    bool linkEnabled = false;
    QVector<DebuggerBreakpoint> breakpoints;
    quint32 editedBreakpointId = 0;
    quint64 configureRequestId = 0;
    bool resetBreakpointHits = false;
    QString gameKey;
    QString registerName;
    quint32 registerValue = 0, registerMask = 0xffffffffu;
};
struct DebuggerSnapshot {
    quint64 generation = 0, frame = 0;
    quint64 configureRequestId = 0;
    bool configureAccepted = false;
    QString configureError;
    bool attached = false, stopped = false, partialFrame = false;
    bool hasNavigationState = false, followingPc = true;
    quint64 navigationRequestId = 0;
    bool navigationAccepted = false;
    QString navigationError;
    quint32 navigationAddress = 0;
    quint32 selectedCpu = 1, eventCpu = 0, event = 0, address = 0, value = 0, accessSize = 0;
    QString status;
    QVector<kr_debug_cpu> cpus;
    QVector<kr_debug_target> targets;
    QVector<kr_debug_instruction> instructions;
    QVector<kr_cov_location> instructionLocations;
    QVector<kr_cov_source> codeSources;
    QByteArray stack, stackValid;
    quint32 stackAddress = 0;
    QVector<DebuggerBreakpoint> breakpoints;
    QString gameKey;
    bool canEdit = false;
    QStringList writableRegisters;
    bool linkSupported = false, linkEnabled = false;
};
