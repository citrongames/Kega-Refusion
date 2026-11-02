#pragma once

#include <cstdint>

// One wall-clock window of completed emulation frames. Game FPS is an input-read
// estimate: a frame is counted when the emulated game reads a controller port.
class PerformanceMetrics final {
public:
    struct Snapshot {
        double emulatedFps = 0.0;
        double speedPercent = 0.0;
        double gameFps = 0.0;
        bool gameFpsAvailable = false;
    };

    void reset(std::int64_t nowNs) {
        windowStartNs_ = nowNs;
        frames_ = 0;
        inputReadFrames_ = 0;
    }

    void recordFrame(bool inputRead) {
        ++frames_;
        if (inputRead) ++inputReadFrames_;
    }

    bool takeSample(std::int64_t nowNs, double nativeFps, bool gameFpsSupported,
                    Snapshot& sample) {
        const auto elapsedNs = nowNs - windowStartNs_;
        if (elapsedNs < 1000000000) return false;
        if (nativeFps > 0.0) {
            sample.emulatedFps = double(frames_) * 1000000000.0 / double(elapsedNs);
            sample.speedPercent = sample.emulatedFps * 100.0 / nativeFps;
            sample.gameFpsAvailable = gameFpsSupported && frames_ > 0;
            sample.gameFps = sample.gameFpsAvailable
                ? nativeFps * double(inputReadFrames_) / double(frames_) : 0.0;
        } else {
            sample = {};
        }
        reset(nowNs);
        return true;
    }

private:
    std::int64_t windowStartNs_ = 0;
    unsigned frames_ = 0;
    unsigned inputReadFrames_ = 0;
};
