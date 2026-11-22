#pragma once
#include "AudioQueue.h"
#include <QObject>
#include <QAudioDevice>
#include <QThread>
#include <QString>
#include <mutex>
#ifdef KEGA_AUDIO_DIAGNOSTICS
#include <QJsonObject>
#endif
class AudioOutputWorker;
class QMediaDevices;

// GUI-facing controls. The sink and timer live on audioThread_; device discovery stays on the GUI thread.
class AudioOutput final : public QObject {
    Q_OBJECT
public:
    explicit AudioOutput(QObject* parent = nullptr);
    ~AudioOutput() override;
    void shutdown();
    // Direct producer calls, bounded by the worker queue; safe during shutdown.
    void play(const QByteArray& pcm, int sampleRate);
    void clear();
    // GUI controls. Device/sink operations are dispatched to the audio thread.
    void setMuted(bool muted);
    void setVolume(int percent);
    void playAchievementSound();
    AudioQueue::LatencyProfile latencyProfile() const { return profile_; }
    void setLatencyProfile(AudioQueue::LatencyProfile profile);
    QByteArray outputDeviceId() const { return outputDeviceId_; }
    void setOutputDeviceId(const QByteArray& id);
#ifdef KEGA_AUDIO_DIAGNOSTICS
    QJsonObject diagnosticSnapshot();
#endif
signals:
    void outputDevicesChanged();
    void unavailable(const QString& message);
    void clockFeedback(qint64 queuedUsecs, qint64 targetUsecs, bool available);
private:
    void refreshOutputDevice();
    QMediaDevices* devices_ = nullptr;
    std::mutex workerAccess_;
    AudioOutputWorker* worker_ = nullptr;
    QThread audioThread_;
    AudioQueue::LatencyProfile profile_ = AudioQueue::LatencyProfile::Normal;
    QByteArray outputDeviceId_;
    int volume_ = 80;
    bool muted_ = false;
};
