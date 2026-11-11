#include "core/LibretroHost.h"
#include "core/RewindBuffer.h"
#include "net/NetplayTimeline.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace {
constexpr int runSeconds = 120;
constexpr int restoreFrames = 300;

double mean(const std::vector<qint64>& times) {
    double total = 0;
    for (const auto time : times) total += time;
    return times.empty() ? 0 : total / times.size() / 1000.0;
}
double percentile(std::vector<qint64> times, double fraction) {
    if (times.empty()) return 0;
    std::sort(times.begin(), times.end());
    return times[size_t((times.size() - 1) * fraction)] / 1000.0;
}
double retainedSeconds(const RewindBuffer& history, double fps) {
    return history.size() > 1 ? (history.size() - 1) / fps : 0;
}
MovieInput controlsForFrame(int frame) {
    MovieInput controls;
    // The same normal per-frame input reaches both histories.
    controls.buttons[0] = (frame >= 180 && frame < 184 ? 1U << RETRO_DEVICE_ID_JOYPAD_START : 0) |
        (frame >= 600 && (frame / 180) % 2 ? 1U << RETRO_DEVICE_ID_JOYPAD_RIGHT : 0) |
        (frame >= 600 && frame % 90 < 8 ? 1U << RETRO_DEVICE_ID_JOYPAD_Y : 0);
    return controls;
}
// Include decoding, native restore, optional preview replay and the canonical
// target restore in both timings. A buffer-only timing hides replay's CPU cost.
bool measureRestore(RewindBuffer& history, LibretroHost& host, std::vector<qint64>& times) {
    for (int i = 0; i < restoreFrames && history.size() > 1; ++i) {
        QElapsedTimer timer;
        timer.start();
        QByteArray state;
        QImage image;
        double aspect = host.aspectRatio();
        InputDisplayState input;
        RewindBuffer::ReplayFrame replay;
        if (!history.stepBack(state, image, aspect, 1, &input, &replay)) return false;
        bool previewRestored = true;
        if (!replay.previousState.isEmpty()) {
            host.beginRewindPreview();
            previewRestored = host.restoreRewindState(replay.previousState);
            if (previewRestored) {
                if (replay.firstFieldControls) {
                    if (replay.firstFieldControls->commands & MovieTape::SoftReset) host.reset();
                    NetplayTimeline::apply(host, *replay.firstFieldControls);
                    host.runRewindPreviewFrame(true,false,false);
                }
                if (replay.controls.commands & MovieTape::SoftReset) host.reset();
                NetplayTimeline::apply(host, replay.controls);
                host.runRewindPreviewFrame(true,replay.singleFieldFallback);
                image = host.frame();
                aspect = host.aspectRatio();
                previewRestored = !image.isNull();
            }
        }
        // Preview generation may change noncanonical native scratch state.
        // Production always restores the retained target before continuing.
        const bool restored = host.restoreRewindState(state);
        if (!replay.previousState.isEmpty()) host.endRewindPreview();
        if (!restored || !previewRestored) return false;
        host.restoreFramePreview(image, aspect);
        times.push_back(timer.nsecsElapsed());
    }
    return true;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "Usage: rewind_benchmark core ROM [ROM...]\n");
        return 2;
    }
    // These are measured histories after runSeconds, not maximum capacities.
    // An eviction flag distinguishes a full rolling ring from one still filling.
    std::puts("game,fps,frames,emulated_seconds,pixel_bytes,current_bytes,pixel_seconds,current_seconds,"
              "pixel_evicted,current_evicted,replay_captures,pixel_fallback_captures,"
              "serialize_mean_us,serialize_p99_us,pixel_capture_mean_us,current_capture_mean_us,"
              "pixel_capture_p99_us,current_capture_p99_us,pixel_restore_mean_us,current_restore_mean_us,"
              "pixel_restore_p99_us,current_restore_p99_us");
    for (int a = 2; a < argc; ++a) {
        QTemporaryDir data;
        LibretroHost host(QString::fromLocal8Bit(argv[1]), data.path());
        QString error;
        if (!host.load(QString::fromLocal8Bit(argv[a]), error)) {
            std::fprintf(stderr, "%s\n", qPrintable(error));
            return 1;
        }
        // Both use the current 128 MiB retained-capacity accounting and input
        // display snapshots. Pixel mode is the previous per-frame image path.
        RewindBuffer pixels, current;
        pixels.configure(true);
        current.configure(true);
        QImage seed = host.frame();
        if (seed.isNull()) {
            seed = QImage(1, 1, QImage::Format_RGB32);
            seed.fill(Qt::black);
        }
        const auto initialState = host.captureRewindState();
        const auto initialInput = host.inputDisplay();
        if (!pixels.capture(initialState, seed, host.aspectRatio(), &initialInput) ||
            !current.capture(initialState, seed, host.aspectRatio(), &initialInput)) return 3;
        const bool needsFirstPicture = seed.width() <= 1 || seed.height() <= 1;
        bool pixelEvicted = false, currentEvicted = false;
        int replayCaptures = 0, pixelFallbackCaptures = 0;
        std::vector<qint64> serializeTimes, pixelTimes, currentTimes, pixelRestoreTimes, currentRestoreTimes;
        QElapsedTimer timer;
        const double fps = host.fps();
        const int frames = int(fps * runSeconds);
        for (int i = 0; i < frames; ++i) {
            const auto controls = controlsForFrame(i);
            NetplayTimeline::apply(host, controls);
            host.runFrame();
            const auto input = host.inputDisplay();
            timer.start();
            const auto state = host.captureRewindState();
            serializeTimes.push_back(timer.nsecsElapsed());
            const auto pixelCount = pixels.size(), currentCount = current.size();
            timer.restart();
            if (!pixels.capture(state, host.frame(), host.aspectRatio(), &input)) return 3;
            pixelTimes.push_back(timer.nsecsElapsed());
            const bool canReplay = i > 0 || !needsFirstPicture;
            timer.restart();
            const bool captured = canReplay
                ? current.captureForReplay(state, host.aspectRatio(), controls, &input, host.rewindPreviewNeedsPair())
                : current.capture(state, host.frame(), host.aspectRatio(), &input);
            if (!captured) return 3;
            currentTimes.push_back(timer.nsecsElapsed());
            if (canReplay) ++replayCaptures;
            else ++pixelFallbackCaptures;
            pixelEvicted |= pixels.size() <= pixelCount;
            currentEvicted |= current.size() <= currentCount;
        }
        // Snapshot forward measurements before destructive reverse traversal.
        const auto pixelBytes = pixels.bytesUsed(), currentBytes = current.bytesUsed();
        const double pixelSeconds = retainedSeconds(pixels, fps), currentSeconds = retainedSeconds(current, fps);
        if (!measureRestore(pixels, host, pixelRestoreTimes) ||
            !measureRestore(current, host, currentRestoreTimes)) return 4;
        QString game = QFileInfo(QString::fromLocal8Bit(argv[a])).fileName();
        game.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        std::printf("\"%s\",%.9f,%d,%.3f,%lld,%lld,%.3f,%.3f,%d,%d,%d,%d,"
                    "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
            qPrintable(game), fps, frames, frames / fps, static_cast<long long>(pixelBytes),
            static_cast<long long>(currentBytes), pixelSeconds, currentSeconds, int(pixelEvicted),
            int(currentEvicted), replayCaptures, pixelFallbackCaptures, mean(serializeTimes),
            percentile(serializeTimes, .99), mean(pixelTimes), mean(currentTimes),
            percentile(pixelTimes, .99), percentile(currentTimes, .99), mean(pixelRestoreTimes),
            mean(currentRestoreTimes), percentile(pixelRestoreTimes, .99), percentile(currentRestoreTimes, .99));
    }
}
