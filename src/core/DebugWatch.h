#pragma once
#include "DebugInspectionAbi.h"
#include <QByteArray>
#include <QString>
#include <QVector>

// Physical-domain offsets; no guessed CPU-bus address conversion.
struct DebugWatchEntry {
    enum Format { Hex, Unsigned, Signed, Binary };
    enum Order { Native, Big, Little };
    quint32 id=0, instance=0, domain=KR_MEM_RAM, offset=0;
    quint32 width=1;
    Format format=Unsigned;
    Order order=Native;
    QString name;
};
struct DebugWatchDomain {
    quint32 instance=0;
    kr_inspect_domain native{};
};
struct DebugWatchSample {
    quint32 id=0;
    QByteArray bytes; // Empty means unavailable; at most four canonical guest bytes.
    bool bigEndian=false;
};
constexpr int DebugWatchLimit=128;
