#include "NetTransferWorker.h"
#include <QBuffer>
#include <QDataStream>
#include <QFileInfo>
#include <cstring>
#include <utility>

namespace {
constexpr int RequestBytes = 4 + 32 + 4 + 8;
constexpr int ChunkHeaderBytes = RequestBytes + 32;

QByteArray encodeRequest(const QByteArray& id, const NetFileTransfer::Request& request) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.writeRawData("KFRQ", 4);
    stream.writeRawData(id.constData(), 32);
    stream << qint32(request.fileIndex) << qint64(request.offset);
    return bytes;
}
bool decodeRequest(const QByteArray& bytes, const QByteArray& id,
                   NetFileTransfer::Request* request) {
    if (id.size() != 32 || bytes.size() != RequestBytes) return false;
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return false;
    QDataStream stream(&buffer);
    char magic[4], suppliedId[32];
    qint32 fileIndex = -1;
    qint64 offset = -1;
    if (stream.readRawData(magic, 4) != 4 || std::memcmp(magic, "KFRQ", 4) != 0 ||
        stream.readRawData(suppliedId, 32) != 32 ||
        std::memcmp(suppliedId, id.constData(), 32) != 0) return false;
    stream >> fileIndex >> offset;
    if (stream.status() != QDataStream::Ok || !buffer.atEnd()) return false;
    *request = {fileIndex, offset};
    return true;
}
QByteArray encodeChunk(const QByteArray& id, const NetFileTransfer::Chunk& chunk) {
    QByteArray bytes;
    bytes.reserve(ChunkHeaderBytes + chunk.data.size());
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.writeRawData("KFCH", 4);
    stream.writeRawData(id.constData(), 32);
    stream << qint32(chunk.fileIndex) << qint64(chunk.offset);
    stream.writeRawData(chunk.sha256.constData(), 32);
    stream.writeRawData(chunk.data.constData(), chunk.data.size());
    return bytes;
}
bool decodeChunk(const QByteArray& bytes, const QByteArray& id,
                 NetFileTransfer::Chunk* chunk) {
    if (id.size() != 32 || bytes.size() <= ChunkHeaderBytes ||
        bytes.size() > ChunkHeaderBytes + NetFileTransfer::ChunkSize) return false;
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return false;
    QDataStream stream(&buffer);
    char magic[4], suppliedId[32], suppliedHash[32];
    qint32 fileIndex = -1;
    qint64 offset = -1;
    if (stream.readRawData(magic, 4) != 4 || std::memcmp(magic, "KFCH", 4) != 0 ||
        stream.readRawData(suppliedId, 32) != 32 ||
        std::memcmp(suppliedId, id.constData(), 32) != 0) return false;
    stream >> fileIndex >> offset;
    if (stream.readRawData(suppliedHash, 32) != 32) return false;
    const int count = bytes.size() - ChunkHeaderBytes;
    QByteArray data(count, Qt::Uninitialized);
    if (stream.readRawData(data.data(), count) != count ||
        stream.status() != QDataStream::Ok || !buffer.atEnd()) return false;
    chunk->fileIndex = fileIndex;
    chunk->offset = offset;
    chunk->sha256 = QByteArray(suppliedHash, 32);
    chunk->data = std::move(data);
    return true;
}
}

NetTransferWorker::NetTransferWorker(QObject* parent) : QObject(parent) {}

void NetTransferWorker::setSharingEnabled(bool enabled) {
    sharingEnabled_ = enabled;
}

void NetTransferWorker::prepare(const QString& path, int archiveIndex) {
    sender_.reset();
    cancelled_.store(false);
    const auto finish = [this,&path,archiveIndex](const QByteArray& bytes, const QString& error) {
        emit prepared(bytes,error);
        emit preparedSource(path,archiveIndex,bytes,error);
    };
    std::optional<RomArchive::Entry> selected;
    QString error;
    if (RomArchive::isArchive(path)) {
        if (archiveIndex < 0) {
            finish({}, QStringLiteral("Select a ROM archive entry first"));
            return;
        }
        const auto entries = RomArchive::list(path, &error);
        for (const auto& entry : entries) {
            if (entry.index == archiveIndex) { selected = entry; break; }
        }
        if (!selected) {
            finish({}, error.isEmpty() ? QStringLiteral("Selected ROM entry is unavailable") : error);
            return;
        }
    }
    auto source = NetFileTransfer::prepare(path, selected, &error, &cancelled_);
    if (!source || cancelled_.load()) {
        finish({}, error.isEmpty() ? QStringLiteral("Media preparation cancelled") : error);
        return;
    }
    sender_.emplace(std::move(*source));
    finish(sender_->manifest().toBytes(), {});
}

void NetTransferWorker::acceptManifest(const QByteArray& manifestBytes,
                                       const QString& dataPath,
                                       const QString& downloadRoot) {
    receiver_.cancel();
    QString error;
    const auto manifest = NetFileTransfer::Manifest::fromBytes(manifestBytes, &error);
    if (!manifest || !receiver_.begin(*manifest, dataPath, &error, downloadRoot)) {
        emit failed(error);
        return;
    }
    receivingId_ = manifest->id;
    emit progress(receiver_.bytesReceived(), receiver_.totalBytes());
    queueNext();
}

void NetTransferWorker::receiveChunk(const QByteArray& bytes) {
    NetFileTransfer::Chunk chunk;
    if (!receiver_.active() || !decodeChunk(bytes, receivingId_, &chunk)) {
        emit failed(QStringLiteral("Invalid or stale media chunk"));
        return;
    }
    QString error;
    if (!receiver_.writeChunk(chunk, &error)) {
        emit failed(error);
        return;
    }
    emit progress(receiver_.bytesReceived(), receiver_.totalBytes());
    queueNext();
}

void NetTransferWorker::requestChunk(quint64 peerId, const QByteArray& bytes) {
    if (!sharingEnabled_ || !sender_) {
        emit requestFailed(peerId,QStringLiteral("Media sharing is disabled"));
        return;
    }
    NetFileTransfer::Request request;
    if (!decodeRequest(bytes, sender_->manifest().id, &request)) {
        emit requestFailed(peerId,QStringLiteral("Invalid or stale media request"));
        return;
    }
    QString error;
    const auto chunk = sender_->readChunk(request, &error);
    if (!chunk) { emit requestFailed(peerId,error); return; }
    emit chunkReady(peerId, encodeChunk(sender_->manifest().id, *chunk));
}

void NetTransferWorker::cancel() {
    requestCancel();
    receiver_.cancel();
}

void NetTransferWorker::queueNext() {
    if (receiver_.complete()) {
        emit completed(receiver_.completedPath());
        return;
    }
    const auto request = receiver_.nextRequest();
    if (request) emit requestReady(encodeRequest(receivingId_, *request));
}

void NetTransferWorker::cleanupTemporary(const QStringList& directories) {
    QStringList cleaned;
    for (const auto& directory : directories) {
        QString error;
        if (NetFileTransfer::TemporaryArea::cleanupOwned(directory, &error))
            cleaned.append(directory);
        else if (!error.isEmpty())
            emit failed(error);
    }
    emit temporaryCleaned(cleaned);
}

void NetTransferWorker::cleanupStale(const QString& dataPath) {
    NetFileTransfer::TemporaryArea::migrateLegacySessions(dataPath);
    NetFileTransfer::TemporaryArea::cleanupStale(dataPath);
}
