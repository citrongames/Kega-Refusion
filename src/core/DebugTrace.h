#pragma once
#include "DebugExecutionAbi.h"
#include "DebugTraceAbi.h"
#include <QByteArray>
#include <QString>
#include <QVector>

enum DebugTraceBoundary {
    KR_TRACE_RESET=1, KR_TRACE_POWER, KR_TRACE_RESTORE, KR_TRACE_REGISTER_EDIT,
    KR_TRACE_MEMORY_EDIT, KR_TRACE_TOPOLOGY, KR_TRACE_UNLOAD
};

struct DebugTraceRecord {
    quint64 sequence=0, frame=0;
    quint32 instance=0, epoch=0;
    kr_trace_record native{};
};
struct DebugTraceConfig {
    quint32 cpu=KR_DEBUG_M68K; // encoded instance<<8 | CPU; 0 means all targets
    quint32 filters=KR_TRACE_INSTRUCTION|KR_TRACE_VIDEO|KR_TRACE_DMA|KR_TRACE_IRQ|KR_TRACE_DISC|KR_TRACE_LINK|KR_TRACE_MAPPER;
    quint32 memoryMiB=16; // bounded 1..64
    QString filePath; // empty = recent ring only
};
struct DebugTraceCommand {
    enum Kind { Refresh, Start, Stop, Clear, Export } kind=Refresh;
    DebugTraceConfig config;
    QString path;
};
struct DebugTraceRow {
    DebugTraceRecord record;
    QString instruction;
    quint32 instructionBytes=0;
};
struct DebugTraceSnapshot {
    bool available=false, active=false, writing=false, canNavigate=false;
    quint64 captureId=0;
    quint64 total=0, overwritten=0;
    quint32 capacity=0, epoch=0, filters=0;
    QVector<kr_debug_target> targets;
    QVector<kr_debug_target> recordTargets; // Captured topology; retained after game change.
    QVector<DebugTraceRow> rows; // bounded recent window, never whole ring per poll
    QString status, filePath, game;
};
