#pragma once

#include "../core/RomArchive.h"
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>
#include <optional>
#include <memory>
#include <atomic>

class QLockFile;

// Transport agnostic; prepare and readChunk must run off the emulation thread.
class NetFileTransfer final {
public:
    // Only these marked, locked directories belong to temporary netplay sessions.
    class TemporaryArea final {
    public:
        enum class Kind { Downloads, Session };
        static std::unique_ptr<TemporaryArea> create(const QString& dataPath, Kind kind,
                                                      QString* error = nullptr);
        static void cleanupStale(const QString& dataPath);
        static void migrateLegacySessions(const QString& dataPath);
        static bool cleanupOwned(const QString& directory, QString* error = nullptr);
        ~TemporaryArea();
        const QString& directory() const { return directory_; }
        const QString& id() const { return id_; }
    private:
        TemporaryArea(QString id, QString directory, std::unique_ptr<QLockFile> lock);
        QString id_, directory_;
        std::unique_ptr<QLockFile> lock_;
    };
    static constexpr qint64 ChunkSize = 64 * 1024;
    static constexpr int MaxFiles = 512;
    static constexpr qint64 MaxFileSize = 8LL * 1024 * 1024 * 1024;
    static constexpr qint64 MaxTotalSize = 32LL * 1024 * 1024 * 1024;
    struct File { QString name; qint64 size = 0; QByteArray sha256; };
    struct Manifest {
        QString entry;
        QVector<File> files;
        QByteArray id;
        qint64 totalSize = 0;
        QByteArray toBytes() const;
        static std::optional<Manifest> fromBytes(const QByteArray& bytes, QString* error = nullptr);
    };
    struct SourceFile {
        File file;
        QString path;
        QByteArray contents;
        QDateTime modified;
    };
    struct Prepared {
        Manifest manifest;
        QVector<SourceFile> sources;
        QString sourceRoot;
        const std::atomic_bool* cancelled = nullptr; // Used only while preparing.
    };
    struct Request { int fileIndex = -1; qint64 offset = 0; };
    struct Chunk { int fileIndex = -1; qint64 offset = 0; QByteArray data; QByteArray sha256; };
    static std::optional<Prepared> prepare(const QString& sourcePath,
                                           const std::optional<RomArchive::Entry>& archiveEntry,
                                           QString* error = nullptr,
                                           const std::atomic_bool* cancelled = nullptr);
    class Sender final {
    public:
        explicit Sender(Prepared prepared);
        const Manifest& manifest() const { return prepared_.manifest; }
        std::optional<Chunk> readChunk(const Request& request, QString* error = nullptr) const;
    private:
        Prepared prepared_;
    };
    class Receiver final {
    public:
        // Call only after local user action. Host paths are never used for output.
        bool begin(const Manifest& manifest, const QString& dataPath, QString* error = nullptr,
                   const QString& downloadRoot = {});
        std::optional<Request> nextRequest(QString* error = nullptr);
        bool writeChunk(const Chunk& chunk, QString* error = nullptr);
        void cancel() { active_ = false; } // Preserve verified .part for resume.
        bool active() const { return active_; }
        bool complete() const { return complete_; }
        qint64 bytesReceived() const { return bytesReceived_; }
        qint64 totalBytes() const { return manifest_.totalSize; }
        QString completedPath() const;
    private:
        bool settleFile(int index, QString* error);
        bool inspectPartial(int index, qint64* offset, QString* error);
        Manifest manifest_;
        QString directory_;
        QVector<qint64> offsets_;
        qint64 bytesReceived_ = 0;
        bool active_ = false;
        bool complete_ = false;
    };
};
