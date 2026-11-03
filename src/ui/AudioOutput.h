#pragma once

#include "AudioQueue.h"

#include <QAudioDevice>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <array>
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
    // GUI thread: start a short tone in the existing PCM stream if it is live.
    void playAchievementSound();

    // Selection and device discovery stay on the GUI thread. An empty ID
    // follows the system default; a missing explicit ID is kept for reconnect.
    // GUI thread; changing this resets only audio, never the emulated session.
    AudioQueue::LatencyProfile latencyProfile() const { return queue_.latencyProfile(); }
    void setLatencyProfile(AudioQueue::LatencyProfile profile);
    QByteArray outputDeviceId() const { return outputDeviceId_; }
    void setOutputDeviceId(const QByteArray& id);

signals:
    void outputDevicesChanged();
    void unavailable(const QString& message);
    // Combined software/device buffer occupancy; the session timestamps this
    // feedback and falls back to its monotonic clock when it becomes stale.
    void clockFeedback(qint64 queuedUsecs, qint64 targetUsecs, bool available);

private:
    QAudioDevice resolveOutputDevice() const;
    void refreshOutputDevice();
    void requestPumpLocked();
    void pump();
    bool configure(int sampleRate, qint64 deviceUsecs);
    void invalidateLocked();
    void releaseSink();
    void fail(const QString& message);
    void feedback(std::uint64_t generation, bool available, qint64 deviceBytes = 0);
    void initializeAchievementChime();
    void appendWithAchievementChime(const QByteArray& pcm, int sampleRate);

    static constexpr int ChimeRate = 48000;
    static constexpr size_t ChimeSamples = ChimeRate / 4;

    std::mutex mutex_;
    AudioQueue queue_;
    std::uint64_t generation_ = 0;
    bool resetPending_ = false;
    bool retryResetPending_ = false;
    bool wakePending_ = false;
    bool closing_ = false;
    std::array<std::int16_t, ChimeSamples> chime_{};
    std::uint64_t chimePositionFixed_ = 0;
    bool chimeActive_ = false;
    bool sinkReady_ = false;

    QTimer pumpTimer_;
    QElapsedTimer clock_;
    QMediaDevices* devices_ = nullptr;
    QByteArray outputDeviceId_;
    QAudioDevice outputDevice_;
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
