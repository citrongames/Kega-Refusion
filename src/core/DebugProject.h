#pragma once
#include "DebugExecution.h"
#include "DebugWatch.h"
#include <QString>
#include <QVector>

struct DebugProjectSymbol {
    quint32 cpu=1, address=0;
    QString label, comment;
    bool bookmarked=false;
};
struct DebugProject {
    QString game;
    QVector<DebuggerBreakpoint> breakpoints;
    QVector<DebugProjectSymbol> symbols;
    QVector<DebugWatchEntry> watches;
};
// Strict, bounded, versioned JSON. Explicit operations only; transactional reads
// and atomic writes. The file contains no ROM, live memory or emulator state.
bool loadDebugProject(const QString& path, DebugProject& result, QString& error);
bool saveDebugProject(const QString& path, const DebugProject& project, QString& error);
