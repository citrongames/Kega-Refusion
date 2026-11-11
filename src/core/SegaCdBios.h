#pragma once

#include <QString>
#include <QStringList>

// Imports user-provided Sega CD BIOS images into the libretro system directory.
// Region values match the application setting: 1 Japan, 2 USA, 3 Europe.
class SegaCdBios final {
public:
    // Returns the actual installed image path, or an empty string with a diagnostic.
    static QString resolve(const QString& dataPath, int region, QString& error);
    // Explicit folder for a loaded session whose configured root is pinned.
    static QString resolveInDirectory(const QString& biosDirectory, int region, QString& error);
    static QStringList importDirectory(const QString& sourceDirectory, const QString& dataPath,
                                       QString& error);
    static QString status(const QString& dataPath, int region);
    static QString statusInDirectory(const QString& biosDirectory, int region);
    static bool validate(const QString& dataPath, int region, QString& error);
};
