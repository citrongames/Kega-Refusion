#include "i18n/Localization.h"
#include "SegaCdBios.h"
#include "DataFolders.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace {
constexpr qsizetype biosSize = 128 * 1024;

struct Region {
    int id;
    char code;
    const char* filename;
    const char* label;
};

constexpr std::array<Region, 3> regions{{
    {1, 'J', "bios_CD_J.bin", "Япония"},
    {2, 'U', "bios_CD_U.bin", "США"},
    {3, 'E', "bios_CD_E.bin", "Европа"},
}};

const Region* findRegion(int id) {
    for (const auto& region : regions)
        if (region.id == id) return &region;
    return nullptr;
}

const Region* findRegion(char code) {
    for (const auto& region : regions)
        if (region.code == code) return &region;
    return nullptr;
}

QString biosPath(const QString& biosDirectory, const Region& region) {
    return QDir(biosDirectory).filePath(QString::fromLatin1(region.filename));
}

// These are fingerprints of the user's six read-only source images, not BIOS data.
// Other revisions are accepted when their internal Sega CD header is consistent.
const Region* knownRegion(const QByteArray& bytes) {
    const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    if (hash == "e8dbee3edcd1b2751c72c1d9ae0dac1aacd9203804bfc1a35ea098d8778bcb1c" ||
        hash == "923eb9da740f98ab1572085e3d351e2c0982df0906b0b99efb68a9997c94cf8f") return findRegion('J');
    if (hash == "807384678ef259494d943eddb6954dfc7069693d0dde0c424e2028488d88e81a" ||
        hash == "b1c2036b79467514eaaa69c76e5ba83801621e821059caa53cb921a0b21e3af4") return findRegion('U');
    if (hash == "16735df92fcc1d0fc6a1eced244867e95281c5006340926082b74e363e073dc4") return findRegion('E');
    return nullptr;
}

const Region* identify(const QByteArray& bytes, QString& reason) {
    if (bytes.size() != biosSize) {
        reason = Localization::text(QStringLiteral("ожидается образ BIOS размером 131072 байта"));
        return nullptr;
    }
    if (const Region* known = knownRegion(bytes)) return known;

    // The platform, product, boot-ROM identifier and territory occupy fixed
    // positions in unmodified Mega-CD/Sega-CD BIOS images.
    const QByteArray title = bytes.mid(0x120, 16).trimmed();
    const QByteArray secondTitle = bytes.mid(0x150, 16).trimmed();
    const bool cdBootRom = title == "MEGA-CD BOOT ROM" || title == "SEGA-CD BOOT ROM" ||
                           title == "CD2 BOOT ROM";
    const quint32 stack = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData()));
    const quint32 reset = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + 4));
    const Region* region = findRegion(bytes.at(0x1f0));
    if (bytes.mid(0x100, 16) != "SEGA MEGA DRIVE " ||
        bytes.mid(0x110, 7) != "(C)SEGA" || !cdBootRom || title != secondTitle ||
        bytes.mid(0x180, 3) != "BR " || stack != 0xfffffd00u ||
        reset < 0x200u || reset >= static_cast<quint32>(biosSize) || (reset & 1u) ||
        !region) {
        reason = Localization::text(QStringLiteral("не распознан оригинальный заголовок Sega CD или его регион"));
        return nullptr;
    }
    // Sega's 16-bit ROM checksum covers all words after the 0x200-byte header.
    // It permits other genuine BIOS revisions while rejecting damaged images.
    const quint16 expectedChecksum = qFromBigEndian<quint16>(
        reinterpret_cast<const uchar*>(bytes.constData() + 0x18e));
    quint16 calculatedChecksum = 0;
    for (qsizetype offset = 0x200; offset < bytes.size(); offset += 2)
        calculatedChecksum = static_cast<quint16>(calculatedChecksum + qFromBigEndian<quint16>(
            reinterpret_cast<const uchar*>(bytes.constData() + offset)));
    if (calculatedChecksum != expectedChecksum) {
        reason = Localization::text(QStringLiteral("контрольная сумма содержимого BIOS не совпадает"));
        return nullptr;
    }
    return region;
}

bool readImage(const QString& path, QByteArray& bytes, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = Localization::text(QStringLiteral("Не удалось прочитать %1: %2")).arg(path, file.errorString());
        return false;
    }
    if (file.size() != biosSize) {
        error = Localization::text(QStringLiteral("Неверный размер BIOS %1: требуется 131072 байта.")).arg(path);
        return false;
    }
    bytes = file.read(biosSize + 1);
    if (bytes.size() != biosSize) {
        error = Localization::text(QStringLiteral("Не удалось полностью прочитать BIOS: %1")).arg(path);
        return false;
    }
    return true;
}

void appendError(QString& error, const QString& detail) {
    if (!error.isEmpty()) error += QLatin1Char('\n');
    error += detail;
}
struct Recognition {
    qint64 size;
    QDateTime modified;
    QDateTime metadataChanged;
    QDateTime created;
    const Region* region;
    QString reason;
};

// These checks run during user actions and loading, outside frame-time paths.
// Cache recognition by metadata, without retaining BIOS bytes.
QMutex recognitionMutex;
QHash<QString, Recognition> recognitionCache;
constexpr int recognitionCacheLimit = 256;

const Region* recognizeFile(const QFileInfo& info, QString& reason) {
    if (!info.isFile() || !info.isReadable() || info.size() != biosSize) {
        reason = Localization::text(QStringLiteral("ожидается доступный образ BIOS размером 131072 байта"));
        return nullptr;
    }
    const QString path = info.absoluteFilePath();
    {
        QMutexLocker lock(&recognitionMutex);
        const auto cached = recognitionCache.constFind(path);
        if (cached != recognitionCache.cend() && cached->size == info.size() &&
            cached->modified == info.lastModified() &&
            cached->metadataChanged == info.metadataChangeTime() &&
            cached->created == info.birthTime()) {
            reason = cached->reason;
            return cached->region;
        }
    }
    QByteArray bytes;
    if (!readImage(path, bytes, reason)) return nullptr;
    const Region* region = identify(bytes, reason);
    {
        QMutexLocker lock(&recognitionMutex);
        if (recognitionCache.size() >= recognitionCacheLimit) recognitionCache.clear();
        recognitionCache.insert(path, {info.size(), info.lastModified(), info.metadataChangeTime(),
                                       info.birthTime(), region, reason});
    }
    return region;
}
} // namespace

QString SegaCdBios::resolve(const QString& dataPath, int region, QString& error) {
    return resolveInDirectory(DataFolders::path(dataPath, QStringLiteral("bios")), region, error);
}

QString SegaCdBios::resolveInDirectory(const QString& biosDirectory, int region, QString& error) {
    error.clear();
    const Region* expected = findRegion(region);
    if (!expected) {
        error = Localization::text(QStringLiteral("Неизвестный регион Sega CD: %1.")).arg(region);
        return {};
    }
    const QString canonical = biosPath(biosDirectory, *expected);
    QString canonicalError;
    const QFileInfo canonicalInfo(canonical);
    if (canonicalInfo.exists()) {
        QString reason;
        const Region* actual = recognizeFile(canonicalInfo, reason);
        if (actual == expected) return canonical;
        if (!actual) {
            canonicalError = Localization::text(QStringLiteral("BIOS %1 недействителен: %2.")).arg(canonical, reason);
        } else {
            canonicalError = Localization::text(QStringLiteral("BIOS %1 относится к региону «%2», ожидается «%3»."))
                .arg(canonical, Localization::text(QString::fromUtf8(actual->label)),
                     Localization::text(QString::fromUtf8(expected->label)));
        }
    }
    QDir directory(biosDirectory);
    auto entries = directory.entryInfoList(QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDir::NoSort);
    std::sort(entries.begin(), entries.end(), [](const QFileInfo& a, const QFileInfo& b) {
        return a.fileName() < b.fileName();
    });
    for (const QFileInfo& info : std::as_const(entries)) {
        if (info.absoluteFilePath() == canonical || info.size() != biosSize) continue;
        QString reason;
        if (recognizeFile(info, reason) == expected) return info.absoluteFilePath();
    }
    error = canonicalError.isEmpty()
        ? Localization::text(QStringLiteral("Для региона «%1» не найден BIOS: %2."))
              .arg(Localization::text(QString::fromUtf8(expected->label)), canonical)
        : canonicalError;
    return {};
}

bool SegaCdBios::validate(const QString& dataPath, int region, QString& error) {
    return !resolve(dataPath, region, error).isEmpty();
}

QString SegaCdBios::status(const QString& dataPath, int region) {
    return statusInDirectory(DataFolders::path(dataPath, QStringLiteral("bios")), region);
}

QString SegaCdBios::statusInDirectory(const QString& biosDirectory, int region) {
    QString error;
    const QString path = resolveInDirectory(biosDirectory, region, error);
    if (path.isEmpty()) return error;
    const Region* expected = findRegion(region);
    return Localization::text(QStringLiteral("BIOS Sega CD для региона «%1» установлен: %2"))
        .arg(Localization::text(QString::fromUtf8(expected->label)), path);
}

QStringList SegaCdBios::importDirectory(const QString& sourceDirectory, const QString& dataPath,
                                        QString& error) {
    error.clear();
    QStringList imported;
    const QDir source(sourceDirectory);
    if (!source.exists()) {
        error = Localization::text(QStringLiteral("Каталог BIOS не найден: %1")).arg(sourceDirectory);
        return imported;
    }

    const QString biosDirectory = DataFolders::path(dataPath, QStringLiteral("bios"));
    struct Candidate { QString path; QByteArray bytes; };
    std::array<Candidate, 3> candidates;
    for (const QFileInfo& info : source.entryInfoList(QDir::Files | QDir::Readable | QDir::NoDotAndDotDot,
                                                      QDir::Name)) {
        if (info.size() != biosSize) continue;
        QByteArray bytes;
        QString readError;
        if (!readImage(info.absoluteFilePath(), bytes, readError)) continue;
        QString reason;
        const Region* region = identify(bytes, reason);
        if (!region) continue;
        Candidate& candidate = candidates.at(static_cast<size_t>(region->id - 1));
        if (candidate.path.isEmpty()) candidate = {info.absoluteFilePath(), std::move(bytes)};
    }

    bool found = false;
    for (const auto& region : regions) {
        const Candidate& candidate = candidates.at(static_cast<size_t>(region.id - 1));
        if (candidate.path.isEmpty()) continue;
        found = true;
        const QString destination = biosPath(biosDirectory, region);
        QString validationError;
        if (!resolveInDirectory(biosDirectory, region.id, validationError).isEmpty()) continue;
        const QFileInfo existing(destination);
        if (existing.exists() || existing.isSymLink()) {
            appendError(error, Localization::text(QStringLiteral("Существующий файл сохранён без замены. %1"))
                               .arg(validationError));
            continue;
        }
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
            appendError(error, Localization::text(QStringLiteral("Не удалось создать каталог BIOS для %1."))
                               .arg(destination));
            continue;
        }
        QSaveFile output(destination);
        if (!output.open(QIODevice::WriteOnly) ||
            output.write(candidate.bytes) != candidate.bytes.size() || !output.commit()) {
            appendError(error, Localization::text(QStringLiteral("Не удалось импортировать %1: %2"))
                               .arg(candidate.path, output.errorString()));
            continue;
        }
        imported << Localization::text(QString::fromUtf8(region.label));
    }
    if (!found) appendError(error, Localization::text(QStringLiteral("В каталоге %1 не найдено подходящих образов BIOS Sega CD размером 131072 байта."))
                                       .arg(source.absolutePath()));
    return imported;
}
