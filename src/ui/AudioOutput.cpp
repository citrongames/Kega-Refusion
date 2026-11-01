#include "AudioOutput.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QtGlobal>

AudioOutput::AudioOutput(QObject* parent) : QObject(parent) {}

void AudioOutput::configure(int sampleRate) {
    if (sink_) {
        sink_->stop();
        delete sink_;
        sink_ = nullptr;
        stream_ = nullptr;
    }
    sampleRate_ = sampleRate;

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull() || sampleRate <= 0) {
        if (!notified_) {
            notified_ = true;
            emit unavailable(QStringLiteral("Звук недоступен: устройство вывода не найдено"));
        }
        return;
    }

    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) {
        if (!notified_) {
            notified_ = true;
            emit unavailable(QStringLiteral("Звук недоступен: формат ядра не поддерживается устройством"));
        }
        return;
    }

    sink_ = new QAudioSink(device, format, this);
    sink_->setBufferSize(qMax(4096, sampleRate * 4 / 10));
    sink_->setVolume(muted_ ? 0.0 : volume_ / 100.0);
    stream_ = sink_->start();
    if (!stream_) {
        delete sink_;
        sink_ = nullptr;
        if (!notified_) {
            notified_ = true;
            emit unavailable(QStringLiteral("Не удалось запустить вывод звука"));
        }
    } else {
        notified_ = false;
    }
}

void AudioOutput::play(const QByteArray& pcm, int sampleRate) {
    if (sampleRate != sampleRate_)
        configure(sampleRate);
    if (!sink_ || !stream_ || pcm.isEmpty())
        return;

    // Keep audio close to the current frame. Discard a chunk when the device
    // cannot accept it instead of queuing stale sound during a slow frame.
    const int available = sink_->bytesFree();
    if (available < pcm.size())
        return;
    stream_->write(pcm.constData(), pcm.size());
}

void AudioOutput::setMuted(bool muted) {
    muted_ = muted;
    if (sink_)
        sink_->setVolume(muted_ ? 0.0 : volume_ / 100.0);
}

void AudioOutput::setVolume(int percent) {
    volume_ = qBound(0, percent, 100);
    if (sink_)
        sink_->setVolume(muted_ ? 0.0 : volume_ / 100.0);
}

void AudioOutput::clear() {
    if (!sink_)
        return;
    sink_->reset();
    stream_ = sink_->start();
}
