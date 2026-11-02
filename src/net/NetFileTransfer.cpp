#include "NetFileTransfer.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <utility>

namespace {
using Transfer = NetFileTransfer;
void fail(QString* error, const QString& message) { if (error) *error = message; }
QByteArray hashBytes(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}
bool safeName(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9_.-]{0,95}$"));
    return pattern.match(name).hasMatch() && name != "." && name != "..";
}
QByteArray canonical(const Transfer::Manifest& manifest) {
    QJsonArray files;
    for (const auto& file : manifest.files) {
        QJsonObject object;
        object.insert("name", file.name);
        object.insert("size", double(file.size));
        object.insert("sha256", QString::fromLatin1(file.sha256.toHex()));
        files.append(object);
    }
    QJsonObject object;
    object.insert("version", 1);
    object.insert("entry", manifest.entry);
    object.insert("files", files);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
bool validManifest(const Transfer::Manifest& manifest, QString* error) {
    if (manifest.files.isEmpty() || manifest.files.size() > Transfer::MaxFiles ||
        !safeName(manifest.entry) || manifest.id.size() != 32) {
        fail(error, "Invalid transfer manifest"); return false;
    }
    qint64 total = 0;
    QSet<QString> names;
    for (const auto& file : manifest.files) {
        if (!safeName(file.name) || names.contains(file.name) || file.size <= 0 ||
            file.size > Transfer::MaxFileSize || file.sha256.size() != 32 ||
            total > Transfer::MaxTotalSize - file.size) {
            fail(error, "Invalid transfer file list"); return false;
        }
        names.insert(file.name);
        total += file.size;
    }
    if (!names.contains(manifest.entry) || total != manifest.totalSize ||
        hashBytes(canonical(manifest)) != manifest.id) {
        fail(error, "Transfer manifest checksum mismatch"); return false;
    }
    return true;
}
QByteArray hashFile(const QString& path, qint64* size, QString* error,
                    const std::atomic_bool* cancelled = nullptr) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { fail(error, "Cannot open media file: " + path); return {}; }
    if (file.size() <= 0 || file.size() > Transfer::MaxFileSize) {
        fail(error, "Invalid media file size: " + path); return {};
    }
    *size = file.size();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(Transfer::ChunkSize, Qt::Uninitialized);
    while (true) {
        if (cancelled && cancelled->load()) { fail(error, "Media preparation cancelled"); return {}; }
        const qint64 count = file.read(buffer.data(), buffer.size());
        if (count < 0) { fail(error, "Cannot read media file: " + path); return {}; }
        if (count == 0) break;
        hash.addData(QByteArrayView(buffer.constData(), count));
    }
    return hash.result();
}
bool pathIsSafe(const QString& path, const QString& root, QString* error) {
    const QFileInfo info(path);
    const QString absolute = QDir::cleanPath(info.absoluteFilePath());
    const QString prefix = root + '/';
    if (!absolute.startsWith(prefix) || !info.isFile()) {
        fail(error, "Media reference escapes selected directory or is missing: " + path); return false;
    }
    QString cursor = root;
    const QString relative = absolute.mid(prefix.size());
    for (const auto& component : relative.split('/')) {
        cursor += '/' + component;
        if (QFileInfo(cursor).isSymLink()) {
            fail(error, "Media symlinks cannot be shared: " + path); return false;
        }
    }
    if (info.canonicalFilePath() != absolute) {
        fail(error, "Media reference changes location: " + path); return false;
    }
    return true;
}
bool safeReference(const QString& root, const QString& from, QString reference,
                   QString* result, QString* error) {
    reference.replace('\\', '/');
    if (reference.isEmpty() || reference.contains("://") ||
        QDir::isAbsolutePath(reference) || reference.contains(':') ||
        reference.split('/').contains("..")) {
        fail(error, "Unsafe media reference: " + reference); return false;
    }
    *result = QDir::cleanPath(QDir(from).absoluteFilePath(reference));
    return pathIsSafe(*result, root, error);
}
QString suffix(const QString& path) {
    const QString result = QFileInfo(path).suffix().toLower();
    if (result.isEmpty() || result.size() > 8 ||
        !QRegularExpression("^[a-z0-9]+$").match(result).hasMatch()) return "bin";
    return result;
}
bool addMemory(Transfer::Prepared* output, const QString& name,
               const QByteArray& bytes, QString* error) {
    if (!safeName(name) || bytes.isEmpty() || bytes.size() > 65536 ||
        output->sources.size() >= Transfer::MaxFiles) {
        fail(error, "Invalid generated media file"); return false;
    }
    Transfer::SourceFile source;
    source.file = {name, bytes.size(), hashBytes(bytes)};
    source.contents = bytes;
    output->sources.append(std::move(source));
    return true;
}
bool addDisk(Transfer::Prepared* output, const QString& name,
             const QString& path, QString* error) {
    if (!safeName(name) || output->sources.size() >= Transfer::MaxFiles ||
        !pathIsSafe(path, output->sourceRoot, error)) {
        if (output->sources.size() >= Transfer::MaxFiles) fail(error, "Too many media files");
        return false;
    }
    Transfer::SourceFile source;
    source.path = path;
    source.file.name = name;
    source.file.sha256 = hashFile(path, &source.file.size, error, output->cancelled);
    if (source.file.sha256.isEmpty()) return false;
    source.modified = QFileInfo(path).lastModified();
    output->sources.append(std::move(source));
    return true;
}
bool addImage(Transfer::Prepared* output, const QString& sourcePath,
              int discIndex, QString* entry, QString* error) {
    QString path = sourcePath;
    QString ext = suffix(path);
    if (ext == "iso") {
        const QFileInfo info(path);
        const QString sidecar = info.absolutePath() + '/' + info.completeBaseName() + ".cue";
        if (QFileInfo(sidecar).isFile()) { path = sidecar; ext = "cue"; }
    }
    const QString prefix = discIndex < 0 ? "game" : QStringLiteral("disc%1").arg(discIndex);
    if (ext != "cue") {
        if (ext != "iso" && ext != "chd") { fail(error, "Unsupported disc image"); return false; }
        *entry = prefix + '.' + ext;
        return addDisk(output, *entry, path, error);
    }
    if (!pathIsSafe(path, output->sourceRoot, error)) return false;
    QFile cue(path);
    if (!cue.open(QIODevice::ReadOnly) || cue.size() <= 0 || cue.size() > 65536) {
        fail(error, "Cannot read CUE or CUE is too large"); return false;
    }
    const QString text = QString::fromUtf8(cue.readAll());
    static const QRegularExpression fileLine(
        QStringLiteral(R"cue(^(\s*)FILE\s+(?:"([^"]+)"|(\S+))\s+(\S+)\s*$)cue"),
        QRegularExpression::CaseInsensitiveOption);
    QByteArray rewritten;
    int track = 0;
    for (const auto& line : text.split('\n')) {
        if (output->cancelled && output->cancelled->load()) {
            fail(error, "Media preparation cancelled"); return false;
        }
        const auto match = fileLine.match(line.trimmed());
        if (!match.hasMatch()) {
            if (QRegularExpression("^FILE\\b", QRegularExpression::CaseInsensitiveOption)
                    .match(line.trimmed()).hasMatch()) {
                fail(error, "Malformed CUE FILE reference"); return false;
            }
            rewritten += line.toUtf8() + '\n';
            continue;
        }
        if (++track > 99) { fail(error, "Too many CUE tracks"); return false; }
        const QString type = match.captured(4).toUpper();
        if (!QStringList{"BINARY", "MOTOROLA", "WAVE", "OGG", "MP3"}.contains(type)) {
            fail(error, "Unsupported CUE track type"); return false;
        }
        QString referenced;
        const QString reference = match.captured(2).isEmpty() ? match.captured(3) : match.captured(2);
        if (!safeReference(output->sourceRoot, QFileInfo(path).absolutePath(),
                           reference, &referenced, error)) return false;
        const QString name = prefix + QStringLiteral("_track%1.").arg(track, 2, 10, QChar('0')) + suffix(referenced);
        if (!addDisk(output, name, referenced, error)) return false;
        rewritten += "FILE \"" + name.toUtf8() + "\" " + type.toUtf8() + '\n';
    }
    if (track == 0 || !text.contains(QRegularExpression(
            "TRACK\\s+0?1\\s+MODE[12]/", QRegularExpression::CaseInsensitiveOption))) {
        fail(error, "CUE has no supported first data track"); return false;
    }
    *entry = prefix + ".cue";
    return addMemory(output, *entry, rewritten, error);
}
bool safeOwned(const QString& path) {
    return !QFileInfo(path).isSymLink();
}
QString filePath(const QString& directory, const Transfer::File& file) {
    return directory + '/' + file.name;
}
bool safeDownloadedReferences(const QString& path, const Transfer::Manifest& manifest,
                              const QString& name, QString* error) {
    const QString extension = QFileInfo(name).suffix();
    if (extension != "cue" && extension != "m3u") return true;
    QFile textFile(path);
    if (!textFile.open(QIODevice::ReadOnly) || textFile.size() > 65536) {
        fail(error, "Invalid downloaded playlist or CUE"); return false;
    }
    QSet<QString> names;
    for (const auto& file : manifest.files) names.insert(file.name);
    static const QRegularExpression cueFile(
        QStringLiteral(R"cue(^FILE\s+(?:"([^"]+)"|(\S+))\s+\S+\s*$)cue"),
        QRegularExpression::CaseInsensitiveOption);
    int references = 0;
    for (QString line : QString::fromUtf8(textFile.readAll()).split('\n')) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        QString reference;
        if (extension == "m3u") {
            if (line.startsWith('#')) continue;
            reference = line;
            if (!QStringList{"cue", "iso", "chd"}.contains(QFileInfo(reference).suffix())) {
                fail(error, "Downloaded M3U names an unsupported image"); return false;
            }
        } else {
            if (!QRegularExpression("^FILE\\b", QRegularExpression::CaseInsensitiveOption)
                     .match(line).hasMatch()) continue;
            const auto match = cueFile.match(line);
            if (!match.hasMatch()) { fail(error, "Malformed downloaded CUE reference"); return false; }
            reference = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
        }
        if (!safeName(reference) || !names.contains(reference) || reference == name) {
            fail(error, "Downloaded media reference leaves the transfer"); return false;
        }
        ++references;
    }
    if (references == 0 || references > (extension == "m3u" ? 4 : 99)) {
        fail(error, "Downloaded media reference count is invalid"); return false;
    }
    return true;
}
}

QByteArray NetFileTransfer::Manifest::toBytes() const {
    QJsonObject object = QJsonDocument::fromJson(canonical(*this)).object();
    object.insert("id", QString::fromLatin1(id.toHex()));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
std::optional<NetFileTransfer::Manifest> NetFileTransfer::Manifest::fromBytes(
    const QByteArray& bytes, QString* error) {
    if (bytes.isEmpty() || bytes.size() > 128 * 1024) {
        fail(error, "Transfer manifest is too large"); return std::nullopt;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fail(error, "Invalid transfer manifest JSON"); return std::nullopt;
    }
    const auto object = document.object();
    if (object.value("version").toInt() != 1 || !object.value("files").isArray()) {
        fail(error, "Unsupported transfer manifest"); return std::nullopt;
    }
    Manifest result;
    result.entry = object.value("entry").toString();
    result.id = QByteArray::fromHex(object.value("id").toString().toLatin1());
    const auto files = object.value("files").toArray();
    if (files.size() > MaxFiles) { fail(error, "Too many transfer files"); return std::nullopt; }
    for (const auto& value : files) {
        if (!value.isObject()) { fail(error, "Invalid transfer file"); return std::nullopt; }
        const auto item = value.toObject();
        const double size = item.value("size").toDouble(-1);
        if (size <= 0 || size > MaxFileSize || qint64(size) != size) {
            fail(error, "Invalid transfer file size"); return std::nullopt;
        }
        File file{item.value("name").toString(), qint64(size),
                  QByteArray::fromHex(item.value("sha256").toString().toLatin1())};
        if (result.totalSize > MaxTotalSize - file.size) {
            fail(error, "Transfer exceeds size limit"); return std::nullopt;
        }
        result.totalSize += file.size;
        result.files.append(std::move(file));
    }
    if (!validManifest(result, error)) return std::nullopt;
    return result;
}

std::optional<NetFileTransfer::Prepared> NetFileTransfer::prepare(
    const QString& sourcePath, const std::optional<RomArchive::Entry>& archiveEntry,
    QString* error, const std::atomic_bool* cancelled) {
    Prepared result;
    result.cancelled = cancelled;
    const QFileInfo selected(sourcePath);
    if (!selected.isFile() || selected.isSymLink() || QFileInfo(selected.absolutePath()).isSymLink()) {
        fail(error, "Selected media is missing or is a symlink"); return std::nullopt;
    }
    result.sourceRoot = QDir::cleanPath(selected.absolutePath());
    const QString path = QDir::cleanPath(selected.absoluteFilePath());
    if (!pathIsSafe(path, result.sourceRoot, error)) return std::nullopt;
    if (RomArchive::isArchive(path)) {
        if (!archiveEntry) { fail(error, "Select a ROM archive entry first"); return std::nullopt; }
        QString archiveError;
        QByteArray rom = RomArchive::extract(path, *archiveEntry, &archiveError);
        if (rom.isEmpty() || rom.size() > RomArchive::MaxMemberSize) {
            fail(error, archiveError.isEmpty() ? "Invalid selected ROM" : archiveError); return std::nullopt;
        }
        const QString ext = suffix(archiveEntry->name);
        result.manifest.entry = "game." + ext;
        SourceFile source;
        source.file = {result.manifest.entry, rom.size(), hashBytes(rom)};
        source.contents = std::move(rom);
        result.sources.append(std::move(source));
    } else if (selected.suffix().compare("m3u", Qt::CaseInsensitive) == 0) {
        QFile playlist(path);
        if (!playlist.open(QIODevice::ReadOnly) || playlist.size() <= 0 || playlist.size() > 65536) {
            fail(error, "Cannot read M3U or M3U is too large"); return std::nullopt;
        }
        QString text = QString::fromUtf8(playlist.readAll());
        if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
        QByteArray rewritten;
        int disc = 0;
        for (QString line : text.split('\n')) {
            if (cancelled && cancelled->load()) {
                fail(error, "Media preparation cancelled"); return std::nullopt;
            }
            line = line.trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            if (line.startsWith('"') && line.endsWith('"')) line = line.mid(1, line.size() - 2);
            if (disc >= 4) { fail(error, "M3U supports at most four discs"); return std::nullopt; }
            QString referenced, entry;
            if (!safeReference(result.sourceRoot, selected.absolutePath(), line,
                               &referenced, error) ||
                !addImage(&result, referenced, disc++, &entry, error)) return std::nullopt;
            rewritten += entry.toUtf8() + '\n';
        }
        if (disc == 0) { fail(error, "M3U is empty"); return std::nullopt; }
        result.manifest.entry = "game.m3u";
        if (!addMemory(&result, result.manifest.entry, rewritten, error)) return std::nullopt;
    } else if (QStringList{"cue", "iso", "chd"}.contains(selected.suffix().toLower())) {
        if (!addImage(&result, path, -1, &result.manifest.entry, error)) return std::nullopt;
    } else {
        result.manifest.entry = "game." + suffix(path);
        if (!addDisk(&result, result.manifest.entry, path, error)) return std::nullopt;
    }
    for (const auto& source : result.sources) {
        if (result.manifest.totalSize > MaxTotalSize - source.file.size) {
            fail(error, "Transfer exceeds size limit"); return std::nullopt;
        }
        result.manifest.totalSize += source.file.size;
        result.manifest.files.append(source.file);
    }
    result.manifest.id = hashBytes(canonical(result.manifest));
    if (!validManifest(result.manifest, error)) return std::nullopt;
    result.cancelled = nullptr;
    return result;
}

NetFileTransfer::Sender::Sender(Prepared prepared) : prepared_(std::move(prepared)) {}
std::optional<NetFileTransfer::Chunk> NetFileTransfer::Sender::readChunk(
    const Request& request, QString* error) const {
    if (request.fileIndex < 0 || request.fileIndex >= prepared_.sources.size()) {
        fail(error, "Invalid file request"); return std::nullopt;
    }
    const auto& source = prepared_.sources.at(request.fileIndex);
    if (request.offset < 0 || request.offset >= source.file.size || request.offset % ChunkSize != 0) {
        fail(error, "Invalid chunk offset"); return std::nullopt;
    }
    const qint64 count = std::min(ChunkSize, source.file.size - request.offset);
    Chunk chunk;
    chunk.fileIndex = request.fileIndex;
    chunk.offset = request.offset;
    if (source.path.isEmpty()) {
        chunk.data = source.contents.mid(request.offset, count);
    } else {
        if (!pathIsSafe(source.path, prepared_.sourceRoot, error)) return std::nullopt;
        const QFileInfo info(source.path);
        if (info.size() != source.file.size || info.lastModified() != source.modified) {
            fail(error, "Shared media changed; prepare it again"); return std::nullopt;
        }
        QFile file(source.path);
        if (!file.open(QIODevice::ReadOnly) || !file.seek(request.offset)) {
            fail(error, "Cannot seek shared media"); return std::nullopt;
        }
        chunk.data = file.read(count);
    }
    if (chunk.data.size() != count) { fail(error, "Cannot read shared media chunk"); return std::nullopt; }
    chunk.sha256 = hashBytes(chunk.data);
    return chunk;
}

bool NetFileTransfer::Receiver::begin(const Manifest& manifest, const QString& dataPath,
                                      QString* error) {
    active_ = complete_ = false;
    bytesReceived_ = 0;
    if (!validManifest(manifest, error)) return false;
    const QString base = QDir(dataPath).absolutePath();
    const QString netplay = base + "/netplay";
    const QString downloads = netplay + "/downloads";
    const QString directory = downloads + '/' + QString::fromLatin1(manifest.id.toHex());
    if (!safeOwned(netplay) || !safeOwned(downloads) || !safeOwned(directory) ||
        !QDir().mkpath(directory)) {
        fail(error, "Cannot create safe download directory"); return false;
    }
    manifest_ = manifest;
    directory_ = directory;
    offsets_.clear();
    offsets_.resize(manifest.files.size());
    for (int i = 0; i < manifest.files.size(); ++i) {
        if (!inspectPartial(i, &offsets_[i], error)) return false;
        bytesReceived_ += offsets_[i];
    }
    active_ = true;
    complete_ = bytesReceived_ == manifest.totalSize;
    return true;
}

bool NetFileTransfer::Receiver::inspectPartial(int index, qint64* offset, QString* error) {
    const auto& expected = manifest_.files.at(index);
    const QString final = filePath(directory_, expected);
    const QString part = final + ".part";
    const QString hashes = final + ".chunks";
    if (!safeOwned(final) || !safeOwned(part) || !safeOwned(hashes)) {
        fail(error, "Download path contains a symlink"); return false;
    }
    if (QFileInfo::exists(final)) {
        qint64 size = 0;
        if (hashFile(final, &size, error) != expected.sha256 || size != expected.size) {
            fail(error, "Existing downloaded file differs; refusing overwrite"); return false;
        }
        if (!safeDownloadedReferences(final, manifest_, expected.name, error)) return false;
        *offset = expected.size;
        return true;
    }
    *offset = 0;
    if (!QFileInfo::exists(part) && !QFileInfo::exists(hashes)) return true;
    QFile data(part), ledger(hashes);
    if (!data.open(QIODevice::ReadWrite) || !ledger.open(QIODevice::ReadWrite)) {
        fail(error, "Cannot resume partial download"); return false;
    }
    QByteArray buffer(ChunkSize, Qt::Uninitialized);
    while (*offset < expected.size && ledger.bytesAvailable() >= 32) {
        const qint64 count = std::min(ChunkSize, expected.size - *offset);
        if (data.read(buffer.data(), count) != count) break;
        const QByteArray digest = ledger.read(32);
        if (hashBytes(QByteArray(buffer.constData(), count)) != digest) break;
        *offset += count;
    }
    if (!data.resize(*offset) || !ledger.resize((*offset + ChunkSize - 1) / ChunkSize * 32)) {
        fail(error, "Cannot repair partial download"); return false;
    }
    if (*offset == expected.size) return settleFile(index, error);
    return true;
}

bool NetFileTransfer::Receiver::settleFile(int index, QString* error) {
    const auto& expected = manifest_.files.at(index);
    const QString final = filePath(directory_, expected);
    const QString part = final + ".part";
    const QString hashes = final + ".chunks";
    if (QFileInfo::exists(final)) return true;
    qint64 size = 0;
    if (hashFile(part, &size, error) != expected.sha256 || size != expected.size) {
        // A bad partial belongs only to this transfer. Discard it so a retry
        // starts cleanly; never modify an existing final media file.
        QFile::remove(part);
        QFile::remove(hashes);
        fail(error, "Downloaded file checksum mismatch"); return false;
    }
    if (!safeDownloadedReferences(part, manifest_, expected.name, error)) return false;
    if (!QFile::rename(part, final)) { fail(error, "Cannot finish download file"); return false; }
    QFile::remove(hashes);
    return true;
}

std::optional<NetFileTransfer::Request> NetFileTransfer::Receiver::nextRequest(QString* error) {
    if (!active_ || complete_) return std::nullopt;
    for (int i = 0; i < manifest_.files.size(); ++i) {
        if (offsets_.at(i) < manifest_.files.at(i).size) return Request{i, offsets_.at(i)};
    }
    complete_ = true;
    Q_UNUSED(error);
    return std::nullopt;
}

bool NetFileTransfer::Receiver::writeChunk(const Chunk& chunk, QString* error) {
    const auto request = nextRequest(error);
    if (!request || chunk.fileIndex != request->fileIndex || chunk.offset != request->offset) {
        fail(error, "Unexpected transfer chunk"); return false;
    }
    const auto& expected = manifest_.files.at(chunk.fileIndex);
    const qint64 count = std::min(ChunkSize, expected.size - chunk.offset);
    if (chunk.data.size() != count || chunk.sha256.size() != 32 ||
        hashBytes(chunk.data) != chunk.sha256) {
        fail(error, "Transfer chunk checksum mismatch"); return false;
    }
    const QString final = filePath(directory_, expected);
    const QString part = final + ".part";
    const QString hashes = final + ".chunks";
    if (!safeOwned(part) || !safeOwned(hashes) || QFileInfo::exists(final)) {
        fail(error, "Unsafe or completed download path"); return false;
    }
    QFile data(part), ledger(hashes);
    if (!data.open(QIODevice::ReadWrite) || !ledger.open(QIODevice::ReadWrite) ||
        data.size() != chunk.offset || ledger.size() != chunk.offset / ChunkSize * 32 ||
        !data.seek(chunk.offset) || !ledger.seek(ledger.size()) ||
        data.write(chunk.data) != count || !data.flush() ||
        ledger.write(chunk.sha256) != 32 || !ledger.flush()) {
        fail(error, "Cannot write transfer chunk"); return false;
    }
    data.close(); ledger.close();
    offsets_[chunk.fileIndex] += count;
    bytesReceived_ += count;
    if (offsets_.at(chunk.fileIndex) == expected.size && !settleFile(chunk.fileIndex, error)) {
        active_ = false; return false;
    }
    complete_ = bytesReceived_ == manifest_.totalSize;
    return true;
}

QString NetFileTransfer::Receiver::completedPath() const {
    if (!complete_ || !active_) return {};
    return directory_ + '/' + manifest_.entry;
}
