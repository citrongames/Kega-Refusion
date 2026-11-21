#pragma once
#include "DebugInspectionAbi.h"
#include "DebugWatch.h"
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>
#include <QtGlobal>

// Copied snapshots only. MainWindow and the core worker share no live pointers.
struct DebugInspectionRequest {
    quint64 serial = 0;
    quint32 parts = 0;
    quint32 domain = KR_MEM_RAM;
    quint32 offset = 0;
    quint32 length = 256;
    quint32 instance = 0;
    bool watchCatalog = false;
    QVector<DebugWatchEntry> watches;
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
    quint32 instance = 0, instanceCount = 1;
    QVector<DebugWatchDomain> watchDomains;
    QVector<DebugWatchSample> watchValues;
    QString gameKey;
    bool canEdit = false;
    QVector<quint32> writableDomains;
    bool available() const { return status.isEmpty() && info.version == KR_INSPECT_VERSION; }
};


struct DebugMemoryWrite {
    quint64 generation = 0;
    quint32 instance = 0, domain = 0, offset = 0;
    QByteArray before, after;
};
