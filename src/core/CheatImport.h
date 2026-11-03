#pragma once

#include "Cheats.h"
#include <QStringList>

namespace Cheats {

struct ImportResult {
    CheatList entries;
    int skipped = 0;
    QStringList warnings;
};

// Reads a RetroArch .cht file. On failure, result is left unchanged.
bool importCht(const QString& path, Platform platform, ImportResult& result, QString& error);
// Writes a native RetroArch .cht file atomically; enabled flags are preserved.
bool exportCht(const QString& path, Platform platform, const CheatList& entries, QString& error);

} // namespace Cheats
