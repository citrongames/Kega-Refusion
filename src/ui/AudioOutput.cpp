#include "i18n/Localization.h"
#include "AudioOutput.h"
#include "AudioOutputWorker.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QMetaObject>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace {
using SinkState = decltype(std::declval<QAudioSink>().state());
using SinkError = decltype(std::declval<QAudioSink>().error());
}

AudioOutputWorker::AudioOutputWorker(QObject* parent) : QObject(parent) {
    initializeAchievementChime();
    clock_.start();
    pumpTimer_.setInterval(5);
    pumpTimer_.setTimerType(Qt::PreciseTimer);
    connect(&pumpTimer_, &QTimer::timeout, this, &AudioOutputWorker::pump);
    pumpTimer_.setParent(this);
}

AudioOutputWorker::~AudioOutputWorker() {
    shutdown();
}

void AudioOutputWorker::beginShutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
    queue_.clear();
}

void AudioOutputWorker::shutdown() {
    Q_ASSERT(QThread::currentThread() == thread());
    beginShutdown();
    pumpTimer_.stop();
    releaseSink();
}

void AudioOutputWorker::setOutputDevice(const QAudioDevice& device) {
    const bool changed = device.id() != outputDevice_.id();
    outputDevice_ = device;
    if (!changed) return;
    retryAtMs_ = 0;
    clear();
}

void AudioOutputWorker::requestPumpLocked() {
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

void AudioOutputWorker::play(const QByteArray& pcm, int sampleRate) {
    if (pcm.isEmpty() || sampleRate <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) return;
    if (sampleRate != queue_.sampleRate()) {
        queue_.setSampleRate(sampleRate);
        chimeActive_ = false;
        sinkReady_ = false;
        resetPending_ = true;
        retryResetPending_ = true;
        ++generation_;
        emit clockFeedback(0, queue_.timing().target, false);
    }
    // Overflow already drops stale whole frames while preserving a partial
    // device write. Keep the live sink: reopening it can take longer than this
    // bounded queue, causing another overflow and an endless reopen loop.
#ifdef KEGA_AUDIO_DIAGNOSTICS
    inputBytes_ += pcm.size();
    inputHash_.addData(pcm);
    const auto inputNow = clock_.nsecsElapsed();
    if (lastInputNs_) maximumInputGapNs_ = std::max(maximumInputGapNs_, inputNow-lastInputNs_);
    lastInputNs_ = inputNow;
    const auto previousBytes = queue_.size();
#endif
    appendWithAchievementChime(pcm, sampleRate);
#ifdef KEGA_AUDIO_DIAGNOSTICS
    const auto dropped = previousBytes + pcm.size() - queue_.size();
    droppedBytes_ += dropped;
    if (dropped && dropEvents_.size() < 64)
        dropEvents_.append(QJsonObject{{"ms",double(clock_.elapsed())},{"bytes",double(dropped)}});
#endif
    requestPumpLocked();
}

void AudioOutputWorker::initializeAchievementChime() {
    constexpr double tau = 6.2831853071795864769;
    for (size_t i = 0; i < chime_.size(); ++i) {
        const double time = double(i) / ChimeRate;
        const auto envelope = [time](double start, double duration) {
            if (time < start || time >= start + duration) return 0.0;
            const double elapsed = time - start;
            const double attack = std::min(1.0, elapsed / 0.006);
            const double fade = 1.0 - elapsed / duration;
            return attack * fade * fade;
        };
        const double first = envelope(0.0, 0.19) * std::sin(tau * 659.25 * time);
        const double second = envelope(0.075, 0.175) *
                              std::sin(tau * 987.77 * (time - 0.075));
        chime_[i] = std::int16_t(std::lround(2600.0 * (first + second)));
    }
}

void AudioOutputWorker::appendWithAchievementChime(const QByteArray& pcm, int sampleRate) {
    if (!chimeActive_ || !sinkReady_ || muted_ || resetPending_) {
        queue_.append(pcm);
        return;
    }
    const qsizetype aligned = pcm.size() - pcm.size() % AudioQueue::FrameBytes;
    const qsizetype capacity = qsizetype(queue_.capacity());
    const qsizetype keep = qMin(aligned, capacity - capacity % AudioQueue::FrameBytes);
    if (keep <= 0) {
        queue_.append(pcm);
        return;
    }
    const std::uint64_t step = (std::uint64_t(ChimeRate) << 32) / std::uint64_t(sampleRate);
    const std::uint64_t end = std::uint64_t(ChimeSamples) << 32;
    if (!step || chimePositionFixed_ >= end) {
        chimeActive_ = false;
        queue_.append(pcm);
        return;
    }
    const std::uint64_t skipped = std::uint64_t((aligned - keep) / AudioQueue::FrameBytes);
    if (skipped >= (end - chimePositionFixed_ + step - 1) / step) {
        chimeActive_ = false;
        queue_.append(pcm);
        return;
    }
    chimePositionFixed_ += skipped * step;
    // Only the newest bounded PCM can survive AudioQueue overflow. Copy that
    // portion while the chime is active; ordinary playback uses the input as-is.
    QByteArray mixed(pcm.constData() + aligned - keep, keep);
    for (qsizetype offset = 0; offset < keep; offset += AudioQueue::FrameBytes) {
        const size_t index = size_t(chimePositionFixed_ >> 32);
        if (index >= chime_.size()) {
            chimeActive_ = false;
            break;
        }
        const int tone = chime_[index];
        for (qsizetype channel = 0; channel < AudioQueue::FrameBytes; channel += 2) {
            std::int16_t original;
            std::memcpy(&original, mixed.constData() + offset + channel, sizeof(original));
            const auto result = std::int16_t(std::clamp(int(original) + tone, -32768, 32767));
            std::memcpy(mixed.data() + offset + channel, &result, sizeof(result));
        }
        chimePositionFixed_ += step;
    }
    queue_.append(mixed);
}

void AudioOutputWorker::playAchievementSound() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_ || muted_ || resetPending_ || !sinkReady_ || !sink_ || !stream_ ||
        queue_.sampleRate() <= 0 || queue_.sampleRate() != sampleRate_ ||
        sink_->state() != SinkState::ActiveState) return;
    chimePositionFixed_ = 0;
    chimeActive_ = true;
}

void AudioOutputWorker::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) return;
    invalidateLocked();
}

void AudioOutputWorker::setLatencyProfile(AudioQueue::LatencyProfile profile) {
    profile = AudioQueue::normalizeProfile(int(profile));
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_ || profile == queue_.latencyProfile()) return;
    queue_.setLatencyProfile(profile);
    invalidateLocked();
}

void AudioOutputWorker::invalidateLocked() {
    queue_.clear();
    chimeActive_ = false;
    sinkReady_ = false;
    resetPending_ = true;
    retryResetPending_ = true;
    ++generation_;
    // Serialize invalidation with publishing feedback. No feedback from the
    // previous game/state may become authoritative after this reset.
    emit clockFeedback(0, queue_.timing().target, false);
    requestPumpLocked();
}

void AudioOutputWorker::releaseSink() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sinkReady_ = false;
        chimeActive_ = false;
    }
    ++sinkGeneration_;
    stream_ = nullptr;
    sampleRate_ = 0;
    prefilling_ = true;
    wroteSamples_ = false;
    auto* sink = std::exchange(sink_, nullptr);
    if (!sink) return;
    disconnect(sink, nullptr, this, nullptr);
    // stop() drains synchronously on some backends; reset() discards promptly.
    sink->reset();
    delete sink;
}

void AudioOutputWorker::fail(const QString& message, std::uint64_t generation) {
    releaseSink();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // An old device failure must not trim new PCM or defer a replacement
        // that was requested while a backend open/reset/write was in progress.
        if (closing_ || generation != generation_ || resetPending_) return;
        retryAtMs_ = clock_.elapsed() + 1000;
        queue_.keepNewest(queue_.bytesForUsecs(queue_.timing().target));
    }
    feedback(generation, false);
    if (!notified_) {
        notified_ = true;
        emit unavailable(message);
    }
}

bool AudioOutputWorker::configure(int sampleRate, qint64 deviceUsecs, std::uint64_t generation) {
    releaseSink();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || generation != generation_ || resetPending_) return false;
    }
    const QAudioDevice device = outputDevice_;
    if (device.isNull()) {
        fail(QStringLiteral("Звук недоступен: устройство вывода не найдено"), generation);
        return false;
    }

    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) {
        fail(QStringLiteral("Звук недоступен: формат ядра не поддерживается устройством"), generation);
        return false;
    }

    sink_ = new QAudioSink(device, format, this);
    // Backends may choose another size. All occupancy measurements below use
    // the actual bufferSize() after start(), never this requested value.
    sink_->setBufferSize(qMax<qint64>(AudioQueue::FrameBytes, qint64(sampleRate) * AudioQueue::FrameBytes * deviceUsecs / 1000000));
    sink_->setVolume(muted_ ? 0.0 : volume_ / 100.0);
    const std::uint64_t sinkGeneration = sinkGeneration_;
    connect(sink_, &QAudioSink::stateChanged, this, [this, sinkGeneration, generation](SinkState state) {
        if (!sink_ || sinkGeneration != sinkGeneration_) return;
        if (state == SinkState::StoppedState && sink_->error() != SinkError::NoError)
            fail(QStringLiteral("Устройство звука отключено; повторное подключение…"), generation);
    }, Qt::QueuedConnection);
#ifdef KEGA_AUDIO_DIAGNOSTICS
    {
        std::lock_guard<std::mutex> lock(diagnosticMutex_);
        diagnosticActive_ = false;
    }
    // Observe transitions when emitted: the ordinary queued error handler may
    // run after the first write and would misclassify the initial empty state.
    connect(sink_, &QAudioSink::stateChanged, this, [this](SinkState state) {
        std::lock_guard<std::mutex> lock(diagnosticMutex_);
        if (state == SinkState::IdleState && diagnosticActive_) {
            ++idleTransitions_;
            if (clock_.elapsed() > 2000) ++idleAfterWarmup_;
            if (idleTimes_.size() < 64) idleTimes_.append(double(clock_.elapsed()));
        }
        diagnosticActive_ = state == SinkState::ActiveState;
    }, Qt::DirectConnection);
#endif
    stream_ = sink_->start();
    if (!stream_ || sink_->error() != SinkError::NoError) {
        fail(QStringLiteral("Не удалось запустить вывод звука; повторная попытка…"), generation);
        return false;
    }
#ifdef KEGA_AUDIO_DIAGNOSTICS
    ++sinkOpens_;
#endif
    sampleRate_ = sampleRate;
    notified_ = false;
    retryAtMs_ = 0;
    return true;
}

void AudioOutputWorker::feedback(std::uint64_t generation, bool available, qint64 deviceBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_ || generation != generation_ || resetPending_) return;
#ifdef KEGA_AUDIO_DIAGNOSTICS
    if (available) {
        const auto occupancy = queue_.usecsForBytes(queue_.size()+deviceBytes);
        if (minimumQueuedUs_ < 0 || occupancy < minimumQueuedUs_) minimumQueuedUs_ = occupancy;
        maximumQueuedUs_ = std::max(maximumQueuedUs_, occupancy);
    }
#endif
    emit clockFeedback(available ? queue_.usecsForBytes(queue_.size() + deviceBytes) : 0,
                       queue_.timing().target, available);
}

void AudioOutputWorker::pump() {
    Q_ASSERT(QThread::currentThread() == thread());
    int rate;
    std::uint64_t generation;
    bool reset;
    bool resetRetry;
    qint64 pendingUsecs;
    AudioQueue::Timing timing;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) return;
        rate = queue_.sampleRate();
        generation = generation_;
        reset = std::exchange(resetPending_, false);
        resetRetry = std::exchange(retryResetPending_, false);
        pendingUsecs = queue_.durationUsecs();
        timing = queue_.timing();
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
        // Wait for this profile's scheduling cushion, not a full device buffer.
        if (pendingUsecs < timing.prefill || clock_.elapsed() < retryAtMs_) {
            feedback(generation, false);
            return;
        }
        if (!configure(rate, timing.device, generation)) return;
    }
    if (sampleRate_ != rate) return;
    if (sink_->state() == SinkState::StoppedState || sink_->state() == SinkState::SuspendedState) {
        fail(QStringLiteral("Вывод звука остановлен; повторная попытка…"), generation);
        return;
    }

    qint64 deviceBytes = qMax<qint64>(0, sink_->bufferSize() - sink_->bytesFree());
    if (wroteSamples_ && sink_->state() == SinkState::IdleState && deviceBytes == 0)
        prefilling_ = true;
    if (prefilling_ && pendingUsecs < timing.prefill) {
        feedback(generation, false);
        // Once input has ended, there is no reason to poll an empty device.
        if (pendingUsecs == 0 && deviceBytes == 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            sinkReady_ = false;
            chimeActive_ = false;
            pumpTimer_.stop();
        }
        return;
    }
    prefilling_ = false;

    AudioQueue::WriteResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || generation != generation_ || resetPending_) return;
        const qint64 highWater = queue_.bytesForUsecs(timing.device);
        const qint64 writable = qMin<qint64>(sink_->bytesFree(), qMax<qint64>(0, highWater - deviceBytes));
        result = queue_.writeTo(writable,
            [this](const char* data, qint64 size) {
                const auto accepted = stream_->write(data, size);
#ifdef KEGA_AUDIO_DIAGNOSTICS
                if (accepted > 0) { writtenBytes_ += accepted; writtenHash_.addData(QByteArrayView(data,accepted)); }
                else if (accepted < 0) ++writeErrors_;
#endif
                return accepted;
            });
    }
    if (result.failed) {
        fail(QStringLiteral("Ошибка передачи звука; повторная попытка…"), generation);
        return;
    }
    wroteSamples_ = wroteSamples_ || result.accepted > 0;
    if (result.accepted > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!closing_ && generation == generation_ && !resetPending_) sinkReady_ = true;
    }
    deviceBytes = qMax<qint64>(0, sink_->bufferSize() - sink_->bytesFree());
    feedback(generation, wroteSamples_, deviceBytes);
}

void AudioOutputWorker::setMuted(bool muted) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        muted_ = muted;
        if (muted) chimeActive_ = false;
    }
    if (sink_) sink_->setVolume(muted_ ? 0.0 : volume_ / 100.0);
}

void AudioOutputWorker::setVolume(int percent) {
    volume_ = qBound(0, percent, 100);
    if (sink_) sink_->setVolume(muted_ ? 0.0 : volume_ / 100.0);
}

#ifdef KEGA_AUDIO_DIAGNOSTICS
QJsonObject AudioOutputWorker::diagnosticSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> diagnosticsLock(diagnosticMutex_);
    return {{"input_bytes",double(inputBytes_)},{"accepted_bytes",double(writtenBytes_)},
        {"dropped_bytes",double(droppedBytes_)},{"drop_events",dropEvents_},{"pending_bytes",double(queue_.size())},
        {"input_sha256",QString::fromLatin1(inputHash_.result().toHex())},
        {"accepted_sha256",QString::fromLatin1(writtenHash_.result().toHex())},
        {"sink_idle_events",double(idleTransitions_)},{"idle_after_2s",double(idleAfterWarmup_)},{"idle_event_ms",idleTimes_},{"sink_opens",double(sinkOpens_)},
        {"write_errors",double(writeErrors_)},{"queue_min_us",double(minimumQueuedUs_)},
        {"queue_max_us",double(maximumQueuedUs_)},{"max_input_gap_ms",maximumInputGapNs_/1000000.0},
        {"sink_buffer_bytes",sink_?double(sink_->bufferSize()):0.0},
        {"device",outputDevice_.description()},{"sample_rate",sampleRate_}};
}
#endif

AudioOutput::AudioOutput(QObject* parent) : QObject(parent), worker_(new AudioOutputWorker) {
    // Initialize Qt's global media/device integration on the application
    // thread, as before. Only sink servicing belongs to the worker.
    devices_ = new QMediaDevices(this);
    const QAudioDevice initialDevice = QMediaDevices::defaultAudioOutput();
    connect(devices_, &QMediaDevices::audioOutputsChanged, this, [this] {
        refreshOutputDevice();
        emit outputDevicesChanged();
    });
    audioThread_.setObjectName(QStringLiteral("Audio output"));
    worker_->moveToThread(&audioThread_);
    connect(&audioThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &AudioOutputWorker::clockFeedback, this, &AudioOutput::clockFeedback,
            Qt::DirectConnection);
    connect(worker_, &AudioOutputWorker::unavailable, this, [this](const QString& source) {
        emit unavailable(Localization::text(source));
    });
    // Device snapshot precedes producer wakes and UI commands.
    QMetaObject::invokeMethod(worker_, [worker=worker_,initialDevice] {
        worker->setOutputDevice(initialDevice);
    }, Qt::QueuedConnection);
    audioThread_.start();
}
AudioOutput::~AudioOutput() { shutdown(); }
void AudioOutput::shutdown() {
    AudioOutputWorker* worker;
    {
        std::lock_guard<std::mutex> lock(workerAccess_);
        worker = std::exchange(worker_, nullptr);
        if (!worker) return;
        worker->beginShutdown();
    }
    // No worker operation waits for the GUI. All multimedia resources are
    // released on their owning thread before quit/join and deferred deletion.
    QMetaObject::invokeMethod(worker, &AudioOutputWorker::shutdown, Qt::BlockingQueuedConnection);
    audioThread_.quit();
    audioThread_.wait();
}
void AudioOutput::play(const QByteArray& pcm, int rate) {
    std::lock_guard<std::mutex> lock(workerAccess_);
    if (worker_) worker_->play(pcm,rate);
}
void AudioOutput::clear() {
    std::lock_guard<std::mutex> lock(workerAccess_);
    if (worker_) worker_->clear();
}
void AudioOutput::setLatencyProfile(AudioQueue::LatencyProfile profile) {
    profile = AudioQueue::normalizeProfile(int(profile));
    if (profile == profile_) return;
    profile_ = profile;
    std::lock_guard<std::mutex> lock(workerAccess_);
    if (worker_) worker_->setLatencyProfile(profile);
}
void AudioOutput::setOutputDeviceId(const QByteArray& id) {
    if (id == outputDeviceId_) return;
    outputDeviceId_ = id;
    refreshOutputDevice();
}
void AudioOutput::setMuted(bool muted) {
    if (muted == muted_) return;
    muted_ = muted;
    std::lock_guard<std::mutex> lock(workerAccess_);
    if (worker_) QMetaObject::invokeMethod(worker_, [worker=worker_,muted] { worker->setMuted(muted); });
}
void AudioOutput::setVolume(int percent) {
    percent = qBound(0,percent,100);
    if (percent == volume_) return;
    volume_ = percent;
    std::lock_guard<std::mutex> lock(workerAccess_);
    if (worker_) QMetaObject::invokeMethod(worker_, [worker=worker_,percent] { worker->setVolume(percent); });
}
void AudioOutput::playAchievementSound() {
    std::lock_guard<std::mutex> lock(workerAccess_);
    if (worker_) QMetaObject::invokeMethod(worker_, &AudioOutputWorker::playAchievementSound);
}
#ifdef KEGA_AUDIO_DIAGNOSTICS
QJsonObject AudioOutput::diagnosticSnapshot() {
    std::lock_guard<std::mutex> lock(workerAccess_);
    QJsonObject result;
    if (worker_) QMetaObject::invokeMethod(worker_, [worker=worker_,&result] {
        result = worker->diagnosticSnapshot();
    }, Qt::BlockingQueuedConnection);
    return result;
}
#endif

void AudioOutput::refreshOutputDevice() {
    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (!outputDeviceId_.isEmpty()) {
        for (const auto& candidate : QMediaDevices::audioOutputs())
            if (candidate.id() == outputDeviceId_) { device = candidate; break; }
    }
    std::lock_guard<std::mutex> lock(workerAccess_);
    if (worker_) QMetaObject::invokeMethod(worker_, [worker=worker_,device] {
        worker->setOutputDevice(device);
    });
}
