#pragma once

#include <cstdint>

// One wall-clock window of completed emulation frames.
class PerformanceMetrics final {
public:
    void reset(std::int64_t nowNs) {
        windowStartNs_ = nowNs;
        frames_ = 0;
    }

    void recordFrame() { ++frames_; }

    bool takeSample(std::int64_t nowNs, double& framesPerSecond) {
        const auto elapsedNs = nowNs - windowStartNs_;
        if (elapsedNs < 1000000000) return false;
        framesPerSecond = double(frames_) * 1000000000.0 / double(elapsedNs);
        reset(nowNs);
        return true;
    }

private:
    std::int64_t windowStartNs_ = 0;
    unsigned frames_ = 0;
};
