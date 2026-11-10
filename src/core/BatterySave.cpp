#include "i18n/Localization.h"
#include "BatterySave.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>

namespace {
bool readExisting(const QString& path, qsizetype limit, QByteArray& bytes,
                  bool& exists, QString& error) {
    exists = QFileInfo::exists(path);
    if (!exists) { bytes.clear(); return true; }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > limit) {
        error = Localization::text(QStringLiteral("Не удалось прочитать память сохранения: %1")).arg(path);
        return false;
    }
    bytes = file.readAll();
    if (bytes.size() != file.size()) {
        error = Localization::text(QStringLiteral("Ошибка чтения памяти сохранения: %1")).arg(path);
        return false;
    }
    return true;
}
bool atomicWrite(const QString& path, const QByteArray& bytes, QString& error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = Localization::text(QStringLiteral("Не удалось сохранить %1: %2")).arg(path, file.errorString());
        return false;
    }
    return true;
}
}

// The core has just initialized a fresh device. A first save must represent
// an in-game change, even if it happens before the first periodic poll.
void BatterySave::rememberUnwritten(const QString& path, const void* formatted, qsizetype size) {
    if (QFileInfo::exists(path)) return;
    auto& entry = entries_[path];
    entry.persisted = QByteArray(static_cast<const char*>(formatted), size);
    entry.observed = entry.persisted;
    entry.initialized = true;
}

bool BatterySave::update(const QString& path, const void* data, qsizetype size,
                         qint64 now, bool force, bool skipBlank, QString& error) {
    error.clear();
    if (!data || size <= 0 || size > 64 * 1024 * 1024) {
        error = Localization::text(QStringLiteral("Некорректный размер памяти сохранения."));
        return false;
    }
    auto& entry = entries_[path];
    const QByteArray current = QByteArray::fromRawData(static_cast<const char*>(data), size);
    if (!entry.initialized) {
        if (!force && now - entry.lastAttempt < MinimumWriteMs) return true;
        bool exists = false;
        if (!readExisting(path, size, entry.persisted, exists, error)) {
            entry.lastAttempt = now;
            return false;
        }
        if (exists && (path.endsWith(QLatin1String(".brm")) || path.endsWith(QLatin1String(".rtc"))) &&
            (entry.persisted.size() != size ||
             (path.endsWith(QLatin1String(".brm")) && entry.persisted.right(32) != current.right(32)))) {
            entry.lastAttempt = now;
            error = Localization::text(QStringLiteral("Повреждённая память сохранения оставлена без изменений: %1")).arg(path);
            return false;
        }
        // Unused cartridge SRAM must not create empty save/backup pairs.
        if (!exists && skipBlank && std::all_of(current.cbegin(), current.cend(),
                [](char value) { return static_cast<unsigned char>(value) == 0xff; }))
            entry.persisted = QByteArray(current.constData(), current.size());
        entry.initialized = true;
        entry.observed = QByteArray(current.constData(), current.size());
        entry.changedAt = now;
    }
    if (current == entry.persisted) {
        entry.dirtySince = -1;
        return true;
    }
    if (entry.dirtySince < 0) { entry.dirtySince = now; entry.changedAt = now; }
    if (current != entry.observed) {
        entry.observed = QByteArray(current.constData(), current.size());
        entry.changedAt = now;
    }
    if (!force && (now - entry.lastAttempt < MinimumWriteMs ||
        (now - entry.changedAt < SettleMs && now - entry.dirtySince < MinimumWriteMs)))
        return true;
    entry.lastAttempt = now;
    QByteArray previous;
    bool exists = false;
    if (!readExisting(path, size, previous, exists, error)) return false;
    // Keep exactly one previous version. On the first save, seed the backup
    // with that initial version. Never touch either file for unchanged bytes.
    if (exists && previous == current) {
        entry.persisted = QByteArray(current.constData(), current.size());
        entry.dirtySince = -1;
        return true;
    }
    if (!exists && !QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = Localization::text(QStringLiteral("Не удалось создать папку сохранений: %1")).arg(QFileInfo(path).absolutePath());
        return false;
    }
    if (!atomicWrite(path + QStringLiteral(".bak"), exists ? previous : current, error) ||
        !atomicWrite(path, current, error)) return false;
    entry.persisted = QByteArray(current.constData(), current.size());
    entry.dirtySince = -1;
    return true;
}
