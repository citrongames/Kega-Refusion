#pragma once
#include "i18n/Localization.h"
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QLockFile>
#include <QString>

// Preserve the primary slot as one rolling, adjacent backup before replacing it.
// A failed backup must leave the primary slot untouched.
namespace StateFile {
inline bool preservePrevious(const QString& path, QString& error) {
    if (!QFileInfo::exists(path)) return true;
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        error = Localization::text(QStringLiteral("Не удалось прочитать предыдущее сохранение %1: %2"))
            .arg(path, source.errorString());
        return false;
    }
    const QString backupPath = path + QStringLiteral(".bak");
    QSaveFile backup(backupPath);
    if (!backup.open(QIODevice::WriteOnly)) {
        error = Localization::text(QStringLiteral("Не удалось создать резервную копию %1: %2"))
            .arg(backupPath, backup.errorString());
        return false;
    }
    char buffer[64 * 1024];
    for (;;) {
        const qint64 count = source.read(buffer, sizeof(buffer));
        if (count < 0) {
            error = Localization::text(QStringLiteral("Не удалось прочитать предыдущее сохранение %1: %2"))
                .arg(path, source.errorString());
            return false;
        }
        if (count == 0) break;
        if (backup.write(buffer, count) != count) {
            error = Localization::text(QStringLiteral("Не удалось записать резервную копию %1: %2"))
                .arg(backupPath, backup.errorString());
            return false;
        }
    }
    if (!backup.commit()) {
        error = Localization::text(QStringLiteral("Не удалось записать резервную копию %1: %2"))
            .arg(backupPath, backup.errorString());
        return false;
    }
    return true;
}

inline bool write(const QString& path, const QByteArray& bytes, bool keepPrevious, QString& error) {
    QSaveFile file(path);
    const auto writeError = [&] {
        error = Localization::text(QStringLiteral("Не удалось сохранить %1: %2")).arg(path,file.errorString());
        return false;
    };
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush())
        return writeError();
    // Serialize slot replacements across emulator instances after the new data is staged.
    QLockFile lock(path + QStringLiteral(".lock"));
    if (keepPrevious) {
        if (!lock.tryLock(0)) {
            error = Localization::text(QStringLiteral("Не удалось заблокировать слот для записи: %1")).arg(path);
            return false;
        }
        if (!preservePrevious(path,error)) return false;
    }
    return file.commit() || writeError();
}
}
