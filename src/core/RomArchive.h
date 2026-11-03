#pragma once
#include "CartridgeLimits.h"
#include <QByteArray>
#include <QString>
#include <QVector>

// Reads cartridge data into memory only. Member paths are never written to disk.
class RomArchive final {
public:
    struct Entry {
        QString name;
        int index = -1; // Zero-based header index, including non-cartridge entries.
        qint64 size = 0;
    };

    static constexpr qint64 MaxArchiveSize = 512LL * 1024 * 1024;
    static constexpr qint64 MaxMemberSize = CartridgeLimits::MaxRomSize;
    static constexpr qint64 MaxDecodedSize = 128LL * 1024 * 1024;
    static constexpr int MaxEntries = 4096;

    static bool isArchive(const QString& path);
    // On failure returns an empty list and sets error. An empty archive or an
    // archive without supported cartridges also sets an explanatory error.
    static QVector<Entry> list(const QString& path, QString* error = nullptr);
    // Revalidates index/name/size, rejecting a stale selection or changed member.
    static QByteArray extract(const QString& path, const Entry& member,
                              QString* error = nullptr);
};
