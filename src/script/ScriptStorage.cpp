#include "ScriptStorage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QThreadPool>
#include <atomic>
#include <mutex>
#include <utility>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <QTemporaryFile>
#include <fcntl.h>
#include <io.h>
#else
#include <QUuid>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
constexpr int MaxRequestsPerScript = 8;
constexpr int MaxRequests = 32;
constexpr qsizetype MaxOutstandingBytes = 8 * 1024 * 1024;
constexpr const char Pending[] = "pending";
constexpr const char NotFound[] = "file not found";
constexpr const char BadDirectory[] = "script storage directory is unavailable or unsafe";
constexpr const char BadFile[] = "unsafe script storage file";
constexpr const char ReadFailed[] = "script storage read failed";
constexpr const char WriteFailed[] = "script storage write failed";
constexpr const char Cancelled[] = "script storage request cancelled";

bool asciiAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

struct Location {
    QString root, source, identity, folder;

    // Accessed by the serial storage worker only. Canonicalization can touch disk.
    const QString& scriptFolder() {
        if (!folder.isEmpty()) return folder;
        if (identity.isEmpty()) {
            const QFileInfo info(source);
            identity = info.canonicalFilePath();
            if (identity.isEmpty()) identity = info.absoluteFilePath();
#ifdef Q_OS_WIN
            identity = identity.toCaseFolded();
#endif
        }
        const QString path = QDir::cleanPath(QDir::fromNativeSeparators(identity));
        const auto hash = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
        QString stem = QFileInfo(path).completeBaseName().left(32);
        for (qsizetype i = 0; i < stem.size(); ++i)
            if (stem[i].unicode() > 127 || !asciiAlnum(stem[i].toLatin1())) stem[i] = '-';
        folder = stem.toLower() + '-' + QString::fromLatin1(hash);
        return folder;
    }
};

struct FileResult {
    QByteArray data;
    const char* error = nullptr;
};

FileResult readFile(QFile& file, int offset, int length) {
    constexpr const char* tooLarge = "script storage file exceeds 1 MiB";
    const auto size = file.size();
    if (size > ScriptStorage::MaxFileBytes) return {{}, tooLarge};
    if (offset >= size) return {};
    if (!file.seek(offset)) return {{}, ReadFailed};
    auto data = file.read(qMin(qint64(length), size - offset));
    if (file.error() != QFile::NoError) return {{}, ReadFailed};
    if (file.size() > ScriptStorage::MaxFileBytes) return {{}, tooLarge};
    return {std::move(data), nullptr};
}

#ifdef Q_OS_WIN
class Handle final {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (valid()) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE value() const { return value_; }
    bool valid() const { return value_ != INVALID_HANDLE_VALUE; }
    void release() { value_ = INVALID_HANDLE_VALUE; }
    void reset(HANDLE value) {
        if (valid()) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_;
};

bool missing(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

const wchar_t* native(const QString& path) {
    return reinterpret_cast<const wchar_t*>(path.utf16());
}

class StorageDirectory final {
public:
    const char* open(const Location& location, const QString& folder, bool create) {
        const QString root = QDir::toNativeSeparators(location.root);
        if (const auto* error = openDirectory(root_, root, create)) return error;
        path_ = root + '\\' + folder;
        return openDirectory(scope_, path_, create);
    }

    FileResult read(QByteArrayView name, int offset, int length) const {
        const auto path = filePath(name);
        Handle handle(openFile(path, GENERIC_READ));
        if (!handle.valid()) return {{}, missing(GetLastError()) ? NotFound : ReadFailed};
        if (!regularFile(handle)) return {{}, BadFile};

        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle.value()), _O_RDONLY | _O_BINARY);
        if (fd < 0) return {{}, ReadFailed};
        handle.release();
        QFile file;
        if (!file.open(fd, QIODevice::ReadOnly | QIODevice::Unbuffered, QFileDevice::AutoCloseHandle)) {
            _close(fd);
            return {{}, ReadFailed};
        }
        return readFile(file, offset, length);
    }

    const char* write(QByteArrayView name, const QByteArray& data, const std::atomic_bool& cancelled) const {
        const auto destination = filePath(name);
        {
            Handle existing(openFile(destination, FILE_READ_ATTRIBUTES));
            if (existing.valid() ? !regularFile(existing) : !missing(GetLastError())) return BadFile;
        }
        QString source;
        {
            QTemporaryFile temp(QDir::fromNativeSeparators(path_) + QStringLiteral("/.storage-XXXXXX"));
            if (!temp.open() || temp.write(data) != data.size() || !temp.flush()) return WriteFailed;
            source = QDir::toNativeSeparators(temp.fileName());
            temp.setAutoRemove(false);
        }
        const bool stopped = cancelled.load(std::memory_order_relaxed);
        if (!stopped && MoveFileExW(native(source), native(destination),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return nullptr;
        QFile::remove(source);
        return stopped ? Cancelled : WriteFailed;
    }

private:
    static const char* openDirectory(Handle& handle, const QString& dir, bool create) {
        if (create) CreateDirectoryW(native(dir), nullptr);
        // Hold both directory handles without share-delete while opening the leaf.
        handle.reset(CreateFileW(native(dir), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!handle.valid()) return !create && missing(GetLastError()) ? NotFound : BadDirectory;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle.value(), &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return BadDirectory;
        return nullptr;
    }

    static HANDLE openFile(const QString& path, DWORD access) {
        return CreateFileW(native(path), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    }

    static bool regularFile(const Handle& handle) {
        BY_HANDLE_FILE_INFORMATION info{};
        return GetFileType(handle.value()) == FILE_TYPE_DISK &&
               GetFileInformationByHandle(handle.value(), &info) && info.nNumberOfLinks == 1 &&
               !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    }

    QString filePath(QByteArrayView name) const { return path_ + '\\' + QString::fromLatin1(name); }
    Handle root_, scope_;
    QString path_;
};
#else
class Fd final {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int value() const { return value_; }
    bool valid() const { return value_ >= 0; }
    void reset(int value = -1) {
        if (valid()) ::close(value_);
        value_ = value;
    }
private:
    int value_;
};

class StorageDirectory final {
public:
    const char* open(const Location& location, const QString& folder, bool create) {
        const QFileInfo root(location.root);
        const auto parent = QFile::encodeName(root.absolutePath());
        Fd base(::open(parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!base.valid()) return BadDirectory;
        const auto leaf = QFile::encodeName(root.fileName());
        const Fd storage(openDirectory(base.value(), leaf.constData(), create));
        if (!storage.valid()) return !create && errno == ENOENT ? NotFound : BadDirectory;
        scope_.reset(openDirectory(storage.value(), folder.toLatin1().constData(), create));
        if (!scope_.valid()) return !create && errno == ENOENT ? NotFound : BadDirectory;
        return nullptr;
    }

    FileResult read(QByteArrayView name, int offset, int length) const {
        const auto leaf = name.toByteArray();
        const Fd fd(::openat(scope_.value(), leaf.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (!fd.valid()) return {{}, errno == ENOENT ? NotFound : errno == ELOOP ? BadFile : ReadFailed};
        struct stat info{};
        if (::fstat(fd.value(), &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1) return {{}, BadFile};
        QFile file;
        if (!file.open(fd.value(), QIODevice::ReadOnly | QIODevice::Unbuffered)) return {{}, ReadFailed};
        return readFile(file, offset, length);
    }

    const char* write(QByteArrayView name, const QByteArray& data, const std::atomic_bool& cancelled) const {
        const auto leaf = name.toByteArray();
        struct stat info{};
        if (::fstatat(scope_.value(), leaf.constData(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISREG(info.st_mode) || info.st_nlink != 1) return BadFile;
        } else if (errno != ENOENT) {
            return BadFile;
        }
        const auto temporary = ".storage-" + QUuid::createUuid().toByteArray(QUuid::Id128);
        const Fd fd(::openat(scope_.value(), temporary.constData(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!fd.valid()) return WriteFailed;

        QFile file;
        const bool ready = file.open(fd.value(), QIODevice::WriteOnly) &&
                           file.write(data) == data.size() && file.flush() && ::fsync(fd.value()) == 0;
        const bool stopped = cancelled.load(std::memory_order_relaxed);
        if (ready && !stopped &&
            ::renameat(scope_.value(), temporary.constData(), scope_.value(), leaf.constData()) == 0)
            return nullptr;
        ::unlinkat(scope_.value(), temporary.constData(), 0);
        return stopped ? Cancelled : WriteFailed;
    }

private:
    static int openDirectory(int parent, const char* name, bool create) {
        if (create && ::mkdirat(parent, name, 0700) && errno != EEXIST) return -1;
        return ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }

    Fd scope_;
};
#endif

struct Limits {
    // This lock guards counters only, never file operations or data copies.
    std::mutex mutex;
    int count = 0;
    qsizetype bytes = 0;
    qint64 nextId = 0;
};

struct Reservation {
    Limits& limits;
    qsizetype bytes;
    qint64 id = 0;

    Reservation(Limits& limits, qsizetype bytes) : limits(limits), bytes(bytes) {
        const std::lock_guard lock(limits.mutex);
        if (limits.count < MaxRequests && bytes <= MaxOutstandingBytes - limits.bytes) {
            ++limits.count;
            limits.bytes += bytes;
            id = ++limits.nextId;
        }
    }

    ~Reservation() {
        if (!id) return;
        const std::lock_guard lock(limits.mutex);
        --limits.count;
        limits.bytes -= bytes;
    }
};

struct Worker {
    Limits limits;
    QThreadPool pool;
    Worker() { pool.setMaxThreadCount(1); }
};

struct Request {
    explicit Request(Limits& limits, qsizetype bytes) : reservation(limits, bytes) {}

    void process() {
        if (cancelled.load(std::memory_order_relaxed)) return;
        try {
            StorageDirectory directory;
            error = directory.open(*location, location->scriptFolder(), write);
            if (!error && !cancelled.load(std::memory_order_relaxed)) {
                if (write) {
                    error = directory.write(name, data, cancelled);
                    data.clear();
                } else {
                    auto result = directory.read(name, offset, length);
                    data = std::move(result.data);
                    error = result.error;
                }
            }
        } catch (...) {
            error = write ? WriteFailed : ReadFailed;
        }
        done.store(true, std::memory_order_release);
    }

    Reservation reservation;
    std::shared_ptr<Location> location;
    QByteArray name, data;
    const char* error = nullptr;
    int offset = 0, length = 0;
    bool write = false;
    std::atomic_bool cancelled{false}, done{false};
};
}

struct ScriptStorage::State {
    std::shared_ptr<Location> location;
    QHash<qint64, std::shared_ptr<Request>> requests;
};

ScriptStorage::ScriptStorage(QString root, QString path, QString identity)
    : state_(std::make_unique<State>()) {
    state_->location = std::make_shared<Location>(
        Location{std::move(root), std::move(path), std::move(identity), {}});
}

ScriptStorage::~ScriptStorage() {
    for (const auto& request : std::as_const(state_->requests))
        request->cancelled.store(true, std::memory_order_relaxed);
}

bool ScriptStorage::validName(QByteArrayView name) {
    if (name.isEmpty() || name.size() > MaxNameBytes || !asciiAlnum(name.front()) || name.back() == '.')
        return false;
    for (const char c : name)
        if (!asciiAlnum(c) && c != '_' && c != '-' && c != '.') return false;
    const auto dot = name.indexOf('.');
    const auto stem = name.first(dot < 0 ? name.size() : dot).toByteArray().toUpper();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return false;
    return !(stem.size() == 4 && (stem.startsWith("LPT") || stem.startsWith("COM")) &&
             stem.back() >= '0' && stem.back() <= '9');
}

ScriptStorage::Submission ScriptStorage::read(QByteArrayView name, int offset, int length) {
    return submit(name, {}, false, offset, length);
}

ScriptStorage::Submission ScriptStorage::write(QByteArrayView name, QByteArrayView data) {
    return submit(name, data, true, 0, 0);
}

ScriptStorage::Submission ScriptStorage::submit(QByteArrayView name, QByteArrayView data,
                                                bool write, int offset, int length) {
    if (state_->location->root.isEmpty() || state_->location->source.isEmpty()) return {0, BadDirectory};
    if (state_->requests.size() == MaxRequestsPerScript) return {0, "script storage queue is full"};
    static Worker worker;
    // A read reserves its worst-case working buffer before it reaches the worker.
    auto request = std::make_shared<Request>(worker.limits, write ? data.size() : length);
    const qint64 id = request->reservation.id;
    if (!id) return {0, "script storage queue is full"};
    request->location = state_->location;
    request->name = name.toByteArray();
    request->data = data.toByteArray();
    request->offset = offset;
    request->length = length;
    request->write = write;
    state_->requests.insert(id, request);
    worker.pool.start([request = std::move(request)] { request->process(); });
    return {id, nullptr};
}

ScriptStorage::Result ScriptStorage::result(qint64 id) const {
    const auto it = state_->requests.constFind(id);
    if (it == state_->requests.cend()) return {"unknown storage request", {}, false};
    const auto& request = **it;
    if (!request.done.load(std::memory_order_acquire)) return {Pending, {}, false};
    return {request.error, request.data, request.write && !request.error, true};
}

void ScriptStorage::release(qint64 id) {
    const auto it = state_->requests.find(id);
    if (it != state_->requests.end() && (*it)->done.load(std::memory_order_acquire))
        state_->requests.erase(it);
}
