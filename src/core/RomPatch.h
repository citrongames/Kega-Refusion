#pragma once
#include <QByteArray>
#include <QString>

class RomPatch final {
public:
    static constexpr qsizetype MaxPatchSize = 256 * 1024 * 1024;

    // Applies IPS or BPS to the raw cartridge bytes. Failure returns an empty
    // array and sets error. The source and patch are never changed.
    static QByteArray apply(const QByteArray& source, const QByteArray& patch, QString& error);
};
