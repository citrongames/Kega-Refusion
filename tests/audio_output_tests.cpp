#include "ui/AudioOutput.h"

#include <QCoreApplication>
#include <QEvent>

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
void require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}

class WakeCounter final : public QObject {
public:
    int count = 0;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::MetaCall) ++count;
        return QObject::eventFilter(watched, event);
    }
};
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        AudioOutput audio;
        WakeCounter wakes;
        audio.installEventFilter(&wakes);
        std::atomic<int> invalidations{0};
        std::atomic<int> activeFeedback{0};
        std::atomic<bool> producerInvalidation{false};
        const auto guiThread = std::this_thread::get_id();
        QObject::connect(&audio, &AudioOutput::clockFeedback, &app,
            [&](qint64, qint64 target, bool available) {
                if (target != AudioQueue::TargetUsecs) ++activeFeedback;
                if (available) ++activeFeedback;
                else {
                    ++invalidations;
                    if (std::this_thread::get_id() != guiThread) producerInvalidation = true;
                }
            }, Qt::DirectConnection);

        // Keep the GUI deliberately blocked during this burst. Only the newest
        // bounded PCM and a single wake may remain. Final clear ensures that
        // flushing the GUI never opens an audio device, even on a headless host.
        std::thread producer([&] {
            const QByteArray pcm(3200, 'p');
            for (int chunk = 0; chunk < 20000; ++chunk)
                audio.play(pcm, chunk < 10000 ? 44100 : 48000);
            audio.clear();
        });
        producer.join();
        // Only the two format changes and explicit clear reset the stream.
        // Overflow must not restart a slow device forever while PCM arrives.
        require(invalidations == 3,
                "queue overflow repeatedly reset the audio stream");
        require(producerInvalidation && invalidations > 0,
                "producer clear did not invalidate audio clock before GUI processing");
        require(wakes.count == 0, "GUI processed producer work while deliberately blocked");
        QCoreApplication::sendPostedEvents(&audio, QEvent::MetaCall);
        require(wakes.count == 1, "PCM burst queued more than one GUI wake");
        require(activeFeedback == 0, "cleared PCM published an active stale audio clock");

        audio.play(QByteArray(4, 'n'), 1);
        audio.clear();
        QCoreApplication::sendPostedEvents(&audio, QEvent::MetaCall);
        require(wakes.count == 2 && activeFeedback == 0,
                "producer wake did not recover after a reset");
        audio.setMuted(true);
        audio.setVolume(120);

        // QObject destruction removes a pending coalesced wake safely.
        {
            AudioOutput pending;
            pending.play(QByteArray(4, 'x'), 1);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        std::puts("audio output tests passed: concurrent producer, coalesced wake, immediate clock reset");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "audio output tests failed: %s\n", error.what());
        return 1;
    }
}
