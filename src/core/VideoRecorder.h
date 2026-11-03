#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <memory>

// A single emulation-thread producer hands immutable frame/audio snapshots to a
// bounded encoder worker. All file and codec work stays on that worker.
class VideoRecorder final {
public:
    struct Result {
        QString path;
        QStringList paths;
        QString error;
        quint64 frames = 0;
    };

    VideoRecorder();
    ~VideoRecorder();
    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;

    bool start(const QString& path, QString& error);
    bool capacityAvailable() const;
    bool submit(const QImage& frame, const QByteArray& stereoPcm16,
                int sampleRate, double fps);
    // Both calls return immediately. Already accepted frames are drained.
    void finish();
    void abort(const QString& error);
    bool busy() const;
    bool accepting() const;
    bool completed() const;
    Result result() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
