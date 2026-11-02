#include "i18n/Localization.h"
#include "CoreFileSystem.h"
#include "Mp3Stream.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace {
enum class VirtualKind { Memory, Alias, Mp3 };
struct VirtualFile {
    VirtualKind kind = VirtualKind::Memory;
    QByteArray bytes;
    QString source;
};

thread_local QHash<QString, VirtualFile> virtualFiles;
thread_local QString vfsError;
thread_local quint64 mp3PcmBytes = 0;

QString normalized(const QString& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

QString decoded(const char* path) {
    return path ? normalized(QString::fromUtf8(path)) : QString();
}

int32_t statSize(qint64 size) {
    return static_cast<int32_t>(std::clamp<qint64>(size, 0, std::numeric_limits<int32_t>::max()));
}

bool virtualDirectory(const QString& path) {
    const QString prefix = path.endsWith(QLatin1Char('/')) ? path : path + QLatin1Char('/');
    for (auto it = virtualFiles.cbegin(); it != virtualFiles.cend(); ++it)
        if (it.key().startsWith(prefix)) return true;
    return false;
}

void setVirtualError(const QString& path, const QString& detail) {
    vfsError = Localization::text(QStringLiteral("Не удалось открыть виртуальный файл %1: %2")).arg(path, detail);
    qWarning().noquote() << vfsError;
}

struct DirEntry {
    QByteArray name;
    bool directory = false;
};
} // namespace

struct retro_vfs_file_handle {
    enum class Kind { Real, Memory, Mp3 } kind = Kind::Real;
    QByteArray originalPath;
    QFile file;
    QByteArray bytes;
    std::unique_ptr<Mp3Stream> mp3;
    qint64 cursor = 0;
    bool writable = false;
    bool virtualAlias = false;
};

struct retro_vfs_dir_handle {
    QVector<DirEntry> entries;
    qsizetype next = 0;
    qsizetype current = -1;
};

namespace {
const char* getPath(retro_vfs_file_handle* handle) {
    return handle ? handle->originalPath.constData() : nullptr;
}

retro_vfs_file_handle* openFile(const char* rawPath, unsigned mode, unsigned) {
    if (!rawPath || !*rawPath) return nullptr;
    const QString path = decoded(rawPath);
    const unsigned access = mode & RETRO_VFS_FILE_ACCESS_READ_WRITE;
    const bool update = (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) != 0;
    if (access == 0 || (access == RETRO_VFS_FILE_ACCESS_READ && update) ||
        (mode & ~(RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING)))
        return nullptr;

    auto handle = std::make_unique<retro_vfs_file_handle>();
    handle->originalPath = QByteArray(rawPath);
    auto it = virtualFiles.constFind(path);
    if (it != virtualFiles.cend()) {
        if (access != RETRO_VFS_FILE_ACCESS_READ || update) {
            setVirtualError(path, Localization::text(QStringLiteral("виртуальные файлы доступны только для чтения")));
            return nullptr;
        }
        if (it->kind == VirtualKind::Memory) {
            handle->kind = retro_vfs_file_handle::Kind::Memory;
            handle->bytes = it->bytes;
        } else if (it->kind == VirtualKind::Mp3) {
            handle->kind = retro_vfs_file_handle::Kind::Mp3;
            handle->mp3 = std::make_unique<Mp3Stream>();
            QString error;
            if (!handle->mp3->open(it->source, error)) {
                setVirtualError(path, error);
                return nullptr;
            }
        } else {
            handle->virtualAlias = true;
            handle->file.setFileName(it->source);
            if (!handle->file.open(QIODevice::ReadOnly)) {
                setVirtualError(path, handle->file.errorString());
                return nullptr;
            }
        }
        return handle.release();
    }

    handle->file.setFileName(path);
    QIODevice::OpenMode qtMode = QIODevice::NotOpen;
    if (access == RETRO_VFS_FILE_ACCESS_READ) qtMode = QIODevice::ReadOnly;
    else {
        handle->writable = true;
        if (update && !QFileInfo::exists(path)) return nullptr;
        qtMode = update || access == RETRO_VFS_FILE_ACCESS_READ_WRITE
            ? QIODevice::ReadWrite : QIODevice::WriteOnly;
        if (!update) qtMode |= QIODevice::Truncate;
    }
    if (!handle->file.open(qtMode)) return nullptr;
    return handle.release();
}

int closeFile(retro_vfs_file_handle* handle) {
    if (!handle) return -1;
    bool okay = true;
    if (handle->kind == retro_vfs_file_handle::Kind::Real && handle->file.isOpen()) {
        if (handle->writable) okay = handle->file.flush();
        handle->file.close();
    }
    delete handle;
    return okay ? 0 : -1;
}

int64_t fileSize(retro_vfs_file_handle* handle) {
    if (!handle) return -1;
    switch (handle->kind) {
    case retro_vfs_file_handle::Kind::Memory: return handle->bytes.size();
    case retro_vfs_file_handle::Kind::Mp3: return handle->mp3->size();
    case retro_vfs_file_handle::Kind::Real: return handle->file.size();
    }
    return -1;
}

int64_t fileTell(retro_vfs_file_handle* handle) {
    if (!handle) return -1;
    switch (handle->kind) {
    case retro_vfs_file_handle::Kind::Memory: return handle->cursor;
    case retro_vfs_file_handle::Kind::Mp3: return handle->mp3->pos();
    case retro_vfs_file_handle::Kind::Real: return handle->file.pos();
    }
    return -1;
}

int64_t fileSeek(retro_vfs_file_handle* handle, int64_t offset, int origin) {
    if (!handle) return -1;
    qint64 base = 0;
    if (origin == RETRO_VFS_SEEK_POSITION_CURRENT) base = fileTell(handle);
    else if (origin == RETRO_VFS_SEEK_POSITION_END) base = fileSize(handle);
    else if (origin != RETRO_VFS_SEEK_POSITION_START) return -1;
    if (base < 0 || (offset > 0 && base > std::numeric_limits<qint64>::max() - offset) ||
        (offset < 0 && base < std::numeric_limits<qint64>::min() - offset)) return -1;
    const qint64 target = base + offset;
    if (target < 0) return -1;
    bool okay = false;
    switch (handle->kind) {
    case retro_vfs_file_handle::Kind::Memory: handle->cursor = target; okay = true; break;
    case retro_vfs_file_handle::Kind::Mp3: okay = handle->mp3->seek(target); break;
    case retro_vfs_file_handle::Kind::Real: okay = handle->file.seek(target); break;
    }
    return okay ? target : -1;
}

int64_t fileRead(retro_vfs_file_handle* handle, void* data, uint64_t length) {
    if (!handle || (!data && length) || length > static_cast<uint64_t>(std::numeric_limits<qint64>::max()))
        return -1;
    if (length == 0) return 0;
    const qint64 count = static_cast<qint64>(length);
    switch (handle->kind) {
    case retro_vfs_file_handle::Kind::Memory: {
        const qint64 available = std::max<qint64>(0, handle->bytes.size() - handle->cursor);
        const qint64 toRead = std::min(count, available);
        if (toRead) std::memcpy(data, handle->bytes.constData() + handle->cursor,
                                static_cast<size_t>(toRead));
        handle->cursor += toRead;
        return toRead;
    }
    case retro_vfs_file_handle::Kind::Mp3: {
        const qint64 before = handle->mp3->pos();
        const qint64 result = handle->mp3->read(static_cast<char*>(data), count);
        if (result > 0) mp3PcmBytes += quint64(std::max<qint64>(0,result-std::max<qint64>(0,44-before)));
        if (result < 0) setVirtualError(QString::fromUtf8(handle->originalPath),
                                       Localization::text(QStringLiteral("ошибка чтения MP3")));
        return result;
    }
    case retro_vfs_file_handle::Kind::Real: {
        const qint64 result = handle->file.read(static_cast<char*>(data), count);
        if (result < 0 && handle->virtualAlias)
            setVirtualError(QString::fromUtf8(handle->originalPath), handle->file.errorString());
        return result;
    }
    }
    return -1;
}

int64_t fileWrite(retro_vfs_file_handle* handle, const void* data, uint64_t length) {
    if (!handle || !handle->writable || handle->kind != retro_vfs_file_handle::Kind::Real ||
        (!data && length) || length > static_cast<uint64_t>(std::numeric_limits<qint64>::max())) return -1;
    if (length == 0) return 0;
    return handle->file.write(static_cast<const char*>(data), static_cast<qint64>(length));
}

int fileFlush(retro_vfs_file_handle* handle) {
    if (!handle) return -1;
    if (!handle->writable) return 0;
    return handle->file.flush() ? 0 : -1;
}

int fileRemove(const char* path) {
    const QString name = decoded(path);
    if (name.isEmpty() || virtualFiles.contains(name)) return -1;
    return QFile::remove(name) ? 0 : -1;
}

int fileRename(const char* oldPath, const char* newPath) {
    const QString from = decoded(oldPath), to = decoded(newPath);
    if (from.isEmpty() || to.isEmpty() || virtualFiles.contains(from) || virtualFiles.contains(to))
        return -1;
    return QFile::rename(from, to) ? 0 : -1;
}

int64_t fileTruncate(retro_vfs_file_handle* handle, int64_t length) {
    if (!handle || !handle->writable || handle->kind != retro_vfs_file_handle::Kind::Real || length < 0)
        return -1;
    return handle->file.resize(length) ? 0 : -1;
}

int fileStat(const char* rawPath, int32_t* size) {
    const QString path = decoded(rawPath);
    if (path.isEmpty()) return 0;
    auto it = virtualFiles.constFind(path);
    if (it != virtualFiles.cend()) {
        if (it->kind == VirtualKind::Memory) {
            if (size) *size = statSize(it->bytes.size());
            return RETRO_VFS_STAT_IS_VALID;
        }
        const QFileInfo source(it->source);
        if (!source.isFile()) return 0;
        if (size) *size = it->kind == VirtualKind::Mp3 ? 0 : statSize(source.size());
        return RETRO_VFS_STAT_IS_VALID;
    }
    const QFileInfo info(path);
    if (!info.exists()) {
        if (!virtualDirectory(path)) return 0;
        if (size) *size = 0;
        return RETRO_VFS_STAT_IS_VALID | RETRO_VFS_STAT_IS_DIRECTORY;
    }
    if (size) *size = statSize(info.size());
    int result = RETRO_VFS_STAT_IS_VALID;
    if (info.isDir()) result |= RETRO_VFS_STAT_IS_DIRECTORY;
    else if (!info.isFile()) result |= RETRO_VFS_STAT_IS_CHARACTER_SPECIAL;
    return result;
}

int makeDirectory(const char* rawPath) {
    const QString path = decoded(rawPath);
    if (path.isEmpty() || virtualFiles.contains(path)) return -1;
    if (QFileInfo::exists(path) || virtualDirectory(path)) return -2;
    return QDir().mkdir(path) ? 0 : -1;
}

retro_vfs_dir_handle* openDirectory(const char* rawPath, bool includeHidden) {
    const QString path = decoded(rawPath);
    if (path.isEmpty()) return nullptr;
    QDir dir(path);
    if (!dir.exists() && !virtualDirectory(path)) return nullptr;
    auto handle = std::make_unique<retro_vfs_dir_handle>();
    QSet<QByteArray> seen;
    if (dir.exists()) {
        QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
        if (includeHidden) filters |= QDir::Hidden;
        for (const QFileInfo& info : dir.entryInfoList(filters, QDir::Name)) {
            const QByteArray name = info.fileName().toUtf8();
            handle->entries.append({name, info.isDir()});
            seen.insert(name);
        }
    }
    const QString prefix = path.endsWith(QLatin1Char('/')) ? path : path + QLatin1Char('/');
    for (auto it = virtualFiles.cbegin(); it != virtualFiles.cend(); ++it) {
        if (!it.key().startsWith(prefix)) continue;
        const QString remainder = it.key().mid(prefix.size());
        const int slash = remainder.indexOf(QLatin1Char('/'));
        const QString entry = slash < 0 ? remainder : remainder.left(slash);
        if (entry.isEmpty() || (!includeHidden && entry.startsWith(QLatin1Char('.')))) continue;
        const QByteArray name = entry.toUtf8();
        if (seen.contains(name)) continue;
        handle->entries.append({name, slash >= 0});
        seen.insert(name);
    }
    return handle.release();
}

bool readDirectory(retro_vfs_dir_handle* handle) {
    if (!handle || handle->next >= handle->entries.size()) return false;
    handle->current = handle->next++;
    return true;
}

const char* directoryName(retro_vfs_dir_handle* handle) {
    return handle && handle->current >= 0 && handle->current < handle->entries.size()
        ? handle->entries[handle->current].name.constData() : nullptr;
}

bool directoryIsDir(retro_vfs_dir_handle* handle) {
    return handle && handle->current >= 0 && handle->current < handle->entries.size() &&
           handle->entries[handle->current].directory;
}

int closeDirectory(retro_vfs_dir_handle* handle) {
    if (!handle) return -1;
    delete handle;
    return 0;
}
} // namespace

retro_vfs_interface* CoreFileSystem::interface() {
    static retro_vfs_interface callbacks{
        getPath, openFile, closeFile, fileSize, fileTell, fileSeek, fileRead, fileWrite,
        fileFlush, fileRemove, fileRename, fileTruncate, fileStat, makeDirectory,
        openDirectory, readDirectory, directoryName, directoryIsDir, closeDirectory
    };
    return &callbacks;
}

void CoreFileSystem::clearVirtualFiles() { virtualFiles.clear(); mp3PcmBytes = 0; }

void CoreFileSystem::addMemoryFile(QString virtualPath, QByteArray bytes) {
    virtualFiles.insert(normalized(virtualPath), {VirtualKind::Memory, std::move(bytes), {}});
}

void CoreFileSystem::addAlias(QString virtualPath, QString actualPath, bool mp3) {
    virtualFiles.insert(normalized(virtualPath),
        {mp3 ? VirtualKind::Mp3 : VirtualKind::Alias, {}, normalized(actualPath)});
}

QString CoreFileSystem::lastError() { return vfsError; }
void CoreFileSystem::clearError() { vfsError.clear(); }

quint64 CoreFileSystem::mp3PcmBytesRead() { return mp3PcmBytes; }
