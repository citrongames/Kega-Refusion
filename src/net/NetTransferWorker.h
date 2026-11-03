#pragma once

#include "NetFileTransfer.h"
#include <QObject>
#include <QStringList>
#include <atomic>
#include <optional>

// Move to a QThread before invoking slots. No file operation belongs on the GUI,
// emulation or socket event threads. A controller owns transport and consent UI.
class NetTransferWorker final : public QObject {
    Q_OBJECT
public:
    explicit NetTransferWorker(QObject* parent = nullptr);
    void requestCancel() noexcept { cancelled_.store(true); } // Safe across threads.

public slots:
    void setSharingEnabled(bool enabled);
    void prepare(const QString& path, int archiveIndex = -1);
    void acceptManifest(const QByteArray& manifestBytes, const QString& dataPath,
                        const QString& downloadRoot = {});
    void cleanupTemporary(const QStringList& directories);
    void cleanupStale(const QString& dataPath);
    void receiveChunk(const QByteArray& bytes);
    void requestChunk(quint64 peerId, const QByteArray& bytes);
    void cancel();

signals:
    void prepared(const QByteArray& manifestBytes, const QString& error);
    void preparedSource(const QString& sourcePath, int archiveIndex,
                        const QByteArray& manifestBytes, const QString& error);
    void chunkReady(quint64 peerId, const QByteArray& bytes);
    void requestFailed(quint64 peerId, const QString& error);
    void requestReady(const QByteArray& bytes);
    void progress(qint64 received, qint64 total);
    void completed(const QString& path);
    void failed(const QString& error);
    void temporaryCleaned(const QStringList& directories);

private:
    void queueNext();
    std::optional<NetFileTransfer::Sender> sender_;
    NetFileTransfer::Receiver receiver_;
    std::atomic_bool cancelled_{false};
    bool sharingEnabled_ = false;
    QByteArray receivingId_;
};
