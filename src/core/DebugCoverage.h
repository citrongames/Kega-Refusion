#pragma once
#include "DebugCoverageAbi.h"
#include "DebugExecutionAbi.h"
#include <QByteArray>
#include <QString>
#include <QVector>

struct DebugCoverageSource {
    kr_cov_source native{};
    quint64 code=0, data=0, both=0;
};
struct DebugCoverageConfig {
    quint32 instance=0;
    quint32 cpu=0; // Native single CPU bit; 0=all supported CPUs on this console.
    bool execution=true, reads=true;
};
struct DebugCoverageCommand {
    enum Kind { Refresh, Start, Stop, Clear, Export } kind=Refresh;
    DebugCoverageConfig config;
    quint32 instance=0, source=0, offset=0;
    quint64 captureId=0; // Export/Clear reject stale capture; 0 before first capture.
    QString path;
};
struct DebugCoverageSnapshot {
    bool available=false, active=false, writing=false, canNavigate=false, hasCapture=false;
    quint32 capturedInstance=0;
    quint64 captureId=0, revision=0, allocatedBytes=0;
    quint64 unknownCode=0, unknownData=0, boundaries=0;
    quint32 instance=0, source=0, offset=0, inspectDomain=0, inspectOffset=0;
    quint32 execCpus=0, readCpus=0;
    QVector<kr_debug_target> targets; // Native CPU IDs for selected available console.
    QVector<quint32> instances;
    QVector<DebugCoverageSource> sources; // Captured physical regions if capture exists.
    QByteArray page; // <=256 unpacked 0/1/2/3 coverage flags, never ROM contents.
    QString game, status;
};
