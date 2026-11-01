#pragma once
#include <QtGlobal>

// Monotonic deadlines preserve the fractional frame period across scheduler jitter.
class FramePacer final {
public:
    void reset(qint64 nowNs);
    qint64 next(qint64 nowNs, qint64 periodNs);
    // Positive queue error lengthens frame time; stale/unavailable feedback is neutral.
    double audioPeriodScale(qint64 nowNs, qint64 feedbackNs, qint64 queuedUs,
                            qint64 targetUs, bool available);
private:
    qint64 deadline_ = 0;
    bool initialized_ = false;
    double scale_ = 1.0;
};

// Windows' generic sleep may quantize to ~15.6ms. Prefer a high-resolution
// waitable timer; the fallback resolution request is scoped to this object.
class FrameWaiter final {
public:
    FrameWaiter();
    ~FrameWaiter();
    FrameWaiter(const FrameWaiter&) = delete;
    FrameWaiter& operator=(const FrameWaiter&) = delete;
    void waitUntil(qint64 deadlineNs);
private:
#ifdef Q_OS_WIN
    void* timer_ = nullptr;
    void* multimedia_ = nullptr;
    bool resolutionRequested_ = false;
#endif
};
