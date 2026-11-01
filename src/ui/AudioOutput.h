#pragma once

#include "AudioQueue.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <mutex>

class QAudioSink;
class QIODevice;
class QMediaDevices;

class AudioOutput final : public QObject {
    Q_OBJECT
public:
    explicit AudioOutput(QObject* parent = nullptr);
    ~AudioOutput() override;

    // Producer entry points are thread-safe. Connect the emulation thread
    // directly: PCM is bounded here, never queued as one GUI event per chunk.
    void play(const QByteArray& pcm, int sampleRate);
    void clear();
    void setMuted(bool muted);
    void setVolume(int percent);

signals:
    void unavailable(const QString& message);
    // Combined software/device buffer occupancy; the session timestamps this
    // feedback and falls back to its monotonic clock when it becomes stale.
    void clockFeedback(qint64 queuedUsecs, qint64 targetUsecs, bool available);

private:
    void requestPumpLocked();
    void pump();
    bool configure(int sampleRate);
    void releaseSink();
    void fail(const QString& message);
    void feedback(std::uint64_t generation, bool available, qint64 deviceBytes = 0);

    std::mutex mutex_;
    AudioQueue queue_;
    std::uint64_t generation_ = 0;
    bool resetPending_ = false;
    bool retryResetPending_ = false;
    bool wakePending_ = false;
    bool closing_ = false;

    QTimer pumpTimer_;
    QElapsedTimer clock_;
    QMediaDevices* devices_ = nullptr;
    QAudioSink* sink_ = nullptr;
    QIODevice* stream_ = nullptr;
    int sampleRate_ = 0;
    std::uint64_t sinkGeneration_ = 0;
    qint64 retryAtMs_ = 0;
    int volume_ = 80;
    bool muted_ = false;
    bool notified_ = false;
    bool prefilling_ = true;
    bool wroteSamples_ = false;
};
