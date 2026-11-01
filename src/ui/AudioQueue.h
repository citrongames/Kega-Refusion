#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <algorithm>

// Interleaved stereo Int16 PCM. The owner serializes producer and consumer access.
class AudioQueue {
public:
    static constexpr qint64 FrameBytes = 4;
    static constexpr qint64 TargetUsecs = 35000;
    static constexpr qint64 PrefillUsecs = 30000;
    static constexpr qint64 MaximumUsecs = 60000;

    void setSampleRate(int rate) {
        sampleRate_ = rate;
        capacity_ = bytesForUsecs(MaximumUsecs);
        clear();
    }

    void clear() { bytes_.clear(); phase_ = 0; }
    qsizetype size() const { return bytes_.size(); }
    bool empty() const { return bytes_.isEmpty(); }
    const char* data() const { return bytes_.constData(); }
    int sampleRate() const { return sampleRate_; }
    qint64 capacity() const { return capacity_; }

    qint64 bytesForUsecs(qint64 usecs) const {
        return sampleRate_ > 0 ? (qint64(sampleRate_) * usecs / 1000000) * FrameBytes : 0;
    }
    qint64 usecsForBytes(qint64 bytes) const {
        return sampleRate_ > 0 ? bytes * 1000000 / (qint64(sampleRate_) * FrameBytes) : 0;
    }
    qint64 durationUsecs() const { return usecsForBytes(bytes_.size()); }

    // Keep an incomplete frame left by a short write, then the newest whole
    // frames. Allocation stays bounded even if the producer submits a huge chunk.
    qint64 append(const QByteArray& pcm) {
        const qsizetype incoming = pcm.size() - pcm.size() % FrameBytes;
        if (incoming <= 0 || capacity_ <= 0) return 0;
        const qsizetype fragment = qMin<qsizetype>(bytes_.size(), (FrameBytes - phase_) % FrameBytes);
        const qint64 wholeBudget = ((capacity_ - fragment) / FrameBytes) * FrameBytes;
        const qint64 oldWhole = bytes_.size() - fragment;
        const qint64 discard = qMax<qint64>(0, oldWhole + incoming - wholeBudget);
        const qint64 discardOld = qMin(discard, oldWhole);
        const qint64 discardNew = discard - discardOld;
        QByteArray next;
        next.reserve(qsizetype(qMin<qint64>(capacity_, bytes_.size() + qint64(incoming))));
        next.append(bytes_.constData(), fragment);
        next.append(bytes_.constData() + fragment + discardOld, qsizetype(oldWhole - discardOld));
        next.append(pcm.constData() + discardNew, qsizetype(incoming - discardNew));
        bytes_.swap(next);
        return discard;
    }

    // A discontinuity resets the device too: discard the partial old sample and
    // retain a recent, frame-aligned window for the replacement stream.
    void keepNewest(qint64 maximumBytes) {
        const qsizetype fragment = qMin<qsizetype>(bytes_.size(), (FrameBytes - phase_) % FrameBytes);
        if (fragment) bytes_.remove(0, fragment);
        phase_ = 0;
        maximumBytes = qMax<qint64>(0, maximumBytes / FrameBytes * FrameBytes);
        if (bytes_.size() > maximumBytes)
            bytes_.remove(0, bytes_.size() - qsizetype(maximumBytes));
    }

    struct WriteResult { qint64 accepted = 0; bool failed = false; };
    template<class Writer>
    WriteResult writeTo(qint64 available, Writer&& writer) {
        WriteResult result;
        // A backend repeatedly accepting tiny writes must not monopolize the UI.
        for (int attempts = 0; attempts < 8 && available > 0 && !empty(); ++attempts) {
            const qint64 offered = qMin<qint64>(available, size());
            const qint64 written = writer(data(), offered);
            if (written < 0 || written > offered) { result.failed = true; break; }
            if (written == 0) break;
            bytes_.remove(0, qsizetype(written));
            phase_ = (phase_ + written) % FrameBytes;
            available -= written;
            result.accepted += written;
        }
        return result;
    }

private:
    QByteArray bytes_;
    int sampleRate_ = 0;
    qint64 capacity_ = 0;
    qint64 phase_ = 0;
};
