#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

class RomPatchSet final {
public:
    // memberName is the cartridge filename (the archive member for an
    // archive). Look beside romPath, then in fallbackDirectory if no match
    // exists. On failure leave content intact and clear applied.
    static bool apply(QByteArray& content, const QString& romPath, const QString& memberName,
                      const QString& fallbackDirectory, QStringList& applied, QString& error);
};
