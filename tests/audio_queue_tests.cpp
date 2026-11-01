#include "ui/AudioQueue.h"

#include <QByteArray>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}

QByteArray frames(int count, char seed = 'a') {
    QByteArray result;
    result.reserve(count * 4);
    for (int n = 0; n < count; ++n)
        result.append(QByteArray(4, char(seed + n % 20)));
    return result;
}

void partialWrites() {
    AudioQueue queue;
    queue.setSampleRate(48000);
    const QByteArray expected = frames(1200);
    queue.append(expected);
    QByteArray received;
    const auto first = queue.writeTo(640, [&](const char* data, qint64 count) {
        const qint64 accepted = qMin<qint64>(count, 17);
        received.append(data, qsizetype(accepted));
        return accepted;
    });
    require(first.accepted == 136 && !first.failed, "partial write pump did not yield after bounded work");
    require(queue.size() == expected.size() - received.size(), "unwritten tail was discarded");
    const auto blocked = queue.writeTo(9999, [](const char*, qint64) { return qint64(0); });
    require(blocked.accepted == 0 && !blocked.failed, "temporary backend backpressure treated as an error");
    const auto failed = queue.writeTo(9999, [](const char*, qint64) { return qint64(-1); });
    require(failed.failed && queue.size() == expected.size() - received.size(),
            "write failure consumed queued samples");
    queue.writeTo(9999, [&](const char* data, qint64 count) {
        received.append(data, qsizetype(count));
        return count;
    });
    require(queue.empty() && received == expected, "partial writes changed byte order or lost samples");
}

void boundedOverflow() {
    AudioQueue queue;
    queue.setSampleRate(48000);
    const QByteArray huge = frames(48000);
    require(queue.append(huge) > 0, "oversized producer chunk did not report dropped samples");
    require(queue.size() <= queue.capacity() && queue.durationUsecs() <= AudioQueue::MaximumUsecs,
            "software queue is not bounded");
    require(QByteArray(queue.data(), queue.size()) == huge.right(queue.size()),
            "overflow did not retain most recent frames");

    // An arbitrary partial-byte backend write must not shift stereo alignment
    // when old complete frames are discarded under backpressure.
    queue.clear();
    queue.append(frames(2880, 'a'));
    QByteArray first;
    queue.writeTo(1, [&](const char* data, qint64 count) {
        first.append(data, qsizetype(count));
        return count;
    });
    const QByteArray newest = frames(48000, 'x');
    queue.append(newest);
    require(queue.size() <= queue.capacity(), "short-write overflow exceeded hard bound");
    require(QByteArray(queue.data(), 3) == QByteArray(3, 'a'),
            "overflow lost the unfinished sample frame");
    queue.keepNewest(queue.bytesForUsecs(AudioQueue::TargetUsecs));
    require(queue.size() % 4 == 0 && queue.durationUsecs() <= AudioQueue::TargetUsecs,
            "discontinuity retained a partial sample or excessive latency");
    require(QByteArray(queue.data(), queue.size()) == newest.right(queue.size()),
            "discontinuity did not keep newest complete samples");
}

void resetAndRates() {
    AudioQueue queue;
    queue.setSampleRate(44100);
    require(queue.bytesForUsecs(1000000) == 176400, "44100Hz duration conversion is wrong");
    queue.append(frames(1470));
    require(queue.durationUsecs() == 33333, "PCM duration uses bytes instead of stereo frames");
    queue.clear();
    require(queue.empty() && queue.durationUsecs() == 0, "reset left stale game audio");
    queue.setSampleRate(48000);
    require(queue.empty() && queue.bytesForUsecs(1000000) == 192000,
            "rate change retained audio or stale conversion");
    queue.append(QByteArray(3, 'x'));
    require(queue.empty(), "malformed partial producer sample was accepted");
    queue.setSampleRate(0);
    queue.append(frames(100));
    require(queue.empty() && queue.durationUsecs() == 0 && queue.capacity() == 0,
            "unavailable/invalid rate failed to disable buffering");
}

void sustainedProducerAndStalls() {
    // 30 seconds at 50/60Hz and several sample rates, with an actual fractional
    // number of samples/frame. Simulate a device consuming every millisecond,
    // then a stopped device and recovery to prove boundedness and fresh reset.
    for (const int rate : {32000, 44100, 48000}) {
        for (const int fps : {50, 60}) {
            AudioQueue queue;
            queue.setSampleRate(rate);
            qint64 fraction = 0, produced = 0, consumed = 0, sampleClock = 0;
            int nextFrame = 0;
            bool started = false;
            for (int tick = 0; tick < 30000; ++tick) {
                while (qint64(nextFrame) * 1000 / fps <= tick) {
                    fraction += rate;
                    const int count = int(fraction / fps);
                    fraction %= fps;
                    produced += count * 4;
                    require(queue.append(QByteArray(count * 4, 'p')) == 0,
                            "nominal sample rate drift dropped complete PCM chunks");
                    ++nextFrame;
                }
                if (!started && queue.durationUsecs() >= AudioQueue::PrefillUsecs) started = true;
                if (started) {
                    sampleClock += rate;
                    const qint64 due = sampleClock / 1000 * 4;
                    sampleClock %= 1000;
                    const auto result = queue.writeTo(due, [](const char*, qint64 count) { return count; });
                    consumed += result.accepted;
                    require(result.accepted == due, "steady audio underrun after initial prefill");
                }
            }
            require(produced == consumed + queue.size(),
                    "fractional frame audio accumulated clock drift");
            consumed += queue.writeTo(queue.size(), [](const char*, qint64 count) { return count; }).accepted;
            require(consumed == produced && queue.empty(), "final drain lost fractional-rate PCM");

            for (int frame = 0; frame < fps * 30; ++frame) {
                queue.append(QByteArray((rate / fps) * 4, 's'));
                require(queue.durationUsecs() <= AudioQueue::MaximumUsecs,
                        "stopped device accumulated unbounded latency");
            }
            queue.clear();
            const QByteArray fresh((rate / fps) * 4, 'n');
            queue.append(fresh);
            QByteArray recovered;
            queue.writeTo(queue.size(), [&](const char* data, qint64 count) {
                recovered.append(data, qsizetype(count));
                return count;
            });
            require(recovered == fresh && queue.empty(),
                    "device fallback/reset replayed stale audio on recovery");
        }
    }
}
}

int main() {
    try {
        partialWrites();
        boundedOverflow();
        resetAndRates();
        sustainedProducerAndStalls();
        std::puts("audio queue tests passed: partial writes, bounds, alignment, rates, stalls and reset");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "audio queue tests failed: %s\n", error.what());
        return 1;
    }
}
