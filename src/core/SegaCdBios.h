#pragma once

#include <QString>
#include <QStringList>

// Imports user-provided Sega CD BIOS images into the libretro system directory.
// Region values match the application setting: 1 Japan, 2 USA, 3 Europe.
class SegaCdBios final {
public:
    static QStringList importDirectory(const QString& sourceDirectory, const QString& dataPath,
                                       QString& error);
    static QString status(const QString& dataPath, int region);
    static bool validate(const QString& dataPath, int region, QString& error);
};
