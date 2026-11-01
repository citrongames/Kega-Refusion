#include "AudioOutput.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QMetaObject>
#include <QtGlobal>

#include <utility>

namespace {
using SinkState = decltype(std::declval<QAudioSink>().state());
using SinkError = decltype(std::declval<QAudioSink>().error());
}

AudioOutput::AudioOutput(QObject* parent) : QObject(parent) {
    clock_.start();
    pumpTimer_.setInterval(5);
    pumpTimer_.setTimerType(Qt::PreciseTimer);
    connect(&pumpTimer_, &QTimer::timeout, this, &AudioOutput::pump);
    devices_ = new QMediaDevices(this);
    connect(devices_, &QMediaDevices::audioOutputsChanged, this, [this] {
        retryAtMs_ = 0;
        clear();
    });
}

AudioOutput::~AudioOutput() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        queue_.clear();
    }
    pumpTimer_.stop();
    releaseSink();
}

void AudioOutput::requestPumpLocked() {
    if (closing_ || wakePending_) return;
    wakePending_ = true;
    QMetaObject::invokeMethod(this, [this] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wakePending_ = false;
            if (closing_) return;
        }
        pump();
    }, Qt::QueuedConnection);
}

void AudioOutput::play(const QByteArray& pcm, int sampleRate) {
    if (pcm.isEmpty() || sampleRate <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) return;
    if (sampleRate != queue_.sampleRate()) {
        queue_.setSampleRate(sampleRate);
        resetPending_ = true;
        retryResetPending_ = true;
        ++generation_;
        emit clockFeedback(0, AudioQueue::TargetUsecs, false);
    }
    if (queue_.append(pcm) > 0) {
        // A stalled UI must recover close to the current frame, rather than
        // spending seconds playing a full stale buffer after the stall.
        queue_.keepNewest(queue_.bytesForUsecs(AudioQueue::TargetUsecs));
        resetPending_ = true;
        ++generation_;
        emit clockFeedback(0, AudioQueue::TargetUsecs, false);
    }
    requestPumpLocked();
}

void AudioOutput::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) return;
    queue_.clear();
    resetPending_ = true;
    retryResetPending_ = true;
    ++generation_;
    // Serialize invalidation with publishing feedback. No feedback from the
    // previous game/state may become authoritative after this reset.
    emit clockFeedback(0, AudioQueue::TargetUsecs, false);
    requestPumpLocked();
}

void AudioOutput::releaseSink() {
    ++sinkGeneration_;
    stream_ = nullptr;
    sampleRate_ = 0;
    prefilling_ = true;
    wroteSamples_ = false;
    if (!sink_) return;
    disconnect(sink_, nullptr, this, nullptr);
    // stop() drains synchronously on some backends; reset() discards promptly.
    sink_->reset();
    delete sink_;
    sink_ = nullptr;
}

void AudioOutput::fail(const QString& message) {
    releaseSink();
    retryAtMs_ = clock_.elapsed() + 1000;
    std::uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.keepNewest(queue_.bytesForUsecs(AudioQueue::TargetUsecs));
        generation = generation_;
    }
    feedback(generation, false);
    if (!notified_) {
        notified_ = true;
        emit unavailable(message);
    }
}

bool AudioOutput::configure(int sampleRate) {
    releaseSink();
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        fail(QStringLiteral("Звук недоступен: устройство вывода не найдено"));
        return false;
    }

    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) {
        fail(QStringLiteral("Звук недоступен: формат ядра не поддерживается устройством"));
        return false;
    }

    sink_ = new QAudioSink(device, format, this);
    // Backends may choose another size. All occupancy measurements below use
    // the actual bufferSize() after start(), never this requested value.
    sink_->setBufferSize(qMax<qint64>(AudioQueue::FrameBytes, qint64(sampleRate) * 4 * 40 / 1000));
    sink_->setVolume(muted_ ? 0.0 : volume_ / 100.0);
    const std::uint64_t sinkGeneration = sinkGeneration_;
    connect(sink_, &QAudioSink::stateChanged, this, [this, sinkGeneration](SinkState state) {
        if (!sink_ || sinkGeneration != sinkGeneration_) return;
        if (state == SinkState::StoppedState && sink_->error() != SinkError::NoError)
            fail(QStringLiteral("Устройство звука отключено; повторное подключение…"));
    }, Qt::QueuedConnection);
    stream_ = sink_->start();
    if (!stream_ || sink_->error() != SinkError::NoError) {
        fail(QStringLiteral("Не удалось запустить вывод звука; повторная попытка…"));
        return false;
    }
    sampleRate_ = sampleRate;
    notified_ = false;
    retryAtMs_ = 0;
    return true;
}

void AudioOutput::feedback(std::uint64_t generation, bool available, qint64 deviceBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_ || generation != generation_ || resetPending_) return;
    emit clockFeedback(available ? queue_.usecsForBytes(queue_.size() + deviceBytes) : 0,
                       AudioQueue::TargetUsecs, available);
}

void AudioOutput::pump() {
    int rate;
    std::uint64_t generation;
    bool reset;
    bool resetRetry;
    qint64 pendingUsecs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) return;
        rate = queue_.sampleRate();
        generation = generation_;
        reset = std::exchange(resetPending_, false);
        resetRetry = std::exchange(retryResetPending_, false);
        pendingUsecs = queue_.durationUsecs();
    }
    if (reset) {
        releaseSink();
        if (resetRetry) retryAtMs_ = 0;
    }
    if (pendingUsecs == 0 && !sink_) {
        pumpTimer_.stop();
        feedback(generation, false);
        return;
    }
    if (!pumpTimer_.isActive()) pumpTimer_.start();

    if (!sink_) {
        // Two ordinary video frames provide a small scheduling cushion before
        // the first sample, without filling a 100ms device buffer.
        if (pendingUsecs < AudioQueue::PrefillUsecs || clock_.elapsed() < retryAtMs_) {
            feedback(generation, false);
            return;
        }
        if (!configure(rate)) return;
    }
    if (sampleRate_ != rate) return;
    if (sink_->state() == SinkState::StoppedState || sink_->state() == SinkState::SuspendedState) {
        fail(QStringLiteral("Вывод звука остановлен; повторная попытка…"));
        return;
    }

    qint64 deviceBytes = qMax<qint64>(0, sink_->bufferSize() - sink_->bytesFree());
    if (wroteSamples_ && sink_->state() == SinkState::IdleState && deviceBytes == 0)
        prefilling_ = true;
    if (prefilling_ && pendingUsecs < AudioQueue::PrefillUsecs) {
        feedback(generation, false);
        // Once input has ended, there is no reason to poll an empty device.
        if (pendingUsecs == 0 && deviceBytes == 0) pumpTimer_.stop();
        return;
    }
    prefilling_ = false;

    AudioQueue::WriteResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || resetPending_) return;
        const qint64 highWater = queue_.bytesForUsecs(40000);
        const qint64 writable = qMin<qint64>(sink_->bytesFree(), qMax<qint64>(0, highWater - deviceBytes));
        result = queue_.writeTo(writable,
            [this](const char* data, qint64 size) { return stream_->write(data, size); });
    }
    if (result.failed) {
        fail(QStringLiteral("Ошибка передачи звука; повторная попытка…"));
        return;
    }
    wroteSamples_ = wroteSamples_ || result.accepted > 0;
    deviceBytes = qMax<qint64>(0, sink_->bufferSize() - sink_->bytesFree());
    feedback(generation, wroteSamples_, deviceBytes);
}

void AudioOutput::setMuted(bool muted) {
    muted_ = muted;
    if (sink_) sink_->setVolume(muted_ ? 0.0 : volume_ / 100.0);
}

void AudioOutput::setVolume(int percent) {
    volume_ = qBound(0, percent, 100);
    if (sink_) sink_->setVolume(muted_ ? 0.0 : volume_ / 100.0);
}
