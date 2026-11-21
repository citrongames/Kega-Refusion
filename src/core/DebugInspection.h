#pragma once
#include "DebugInspectionAbi.h"
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QtGlobal>

// Copied snapshots only. MainWindow and the core worker share no live pointers.
struct DebugInspectionRequest {
    quint64 serial = 0;
    quint32 parts = 0;
    quint32 domain = KR_MEM_RAM;
    quint32 offset = 0;
    quint32 length = 256;
};
struct DebugInspectionSnapshot {
    quint64 serial = 0;
    quint64 generation = 0;
    quint64 frame = 0;
    quint32 parts = 0;
    QString status; // translated by backend when unavailable
    kr_inspect_info info{};
    quint32 memoryDomain = 0, memoryOffset = 0;
    QByteArray memory;
    QHash<quint32, QByteArray> blocks; // requested video domains only
    bool available() const { return status.isEmpty() && info.version == KR_INSPECT_VERSION; }
};
