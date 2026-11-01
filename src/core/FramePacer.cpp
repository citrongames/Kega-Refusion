#include "FramePacer.h"
#include <algorithm>
#include <chrono>
#include <thread>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
void FramePacer::reset(qint64 nowNs) { deadline_=nowNs; initialized_=true; scale_=1.0; }
qint64 FramePacer::next(qint64 nowNs,qint64 periodNs) {
    periodNs=std::clamp<qint64>(periodNs,100000,1000000000);
    if(!initialized_)reset(nowNs);
    deadline_+=periodNs;
    // A long stall must not trigger a burst of stale frames/audio on recovery.
    if(nowNs-deadline_>2*periodNs)deadline_=nowNs;
    return deadline_;
}
double FramePacer::audioPeriodScale(qint64 nowNs,qint64 feedbackNs,qint64 queuedUs,qint64 targetUs,bool available) {
    if(!available || targetUs<=0 || feedbackNs<=0 || nowNs<feedbackNs || nowNs-feedbackNs>250000000) {
        scale_=1.0;return scale_;
    }
    const double error=double(queuedUs-targetUs)/double(targetUs);
    const double desired=1.0+std::clamp(error*0.02,-0.005,0.005);
    scale_+=0.04*(desired-scale_);
    return scale_;
}

FrameWaiter::FrameWaiter() {
#ifdef Q_OS_WIN
    constexpr DWORD highResolution = 0x00000002;
    timer_ = CreateWaitableTimerExW(nullptr,nullptr,highResolution,TIMER_MODIFY_STATE|SYNCHRONIZE);
    if (!timer_) {
        timer_ = CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_MODIFY_STATE|SYNCHRONIZE);
        multimedia_ = LoadLibraryW(L"winmm.dll");
        if (multimedia_) {
            using Resolution = UINT (WINAPI*)(UINT);
            auto begin = reinterpret_cast<Resolution>(GetProcAddress(static_cast<HMODULE>(multimedia_),"timeBeginPeriod"));
            if (begin) resolutionRequested_ = begin(1)==0;
        }
    }
#endif
}
FrameWaiter::~FrameWaiter() {
#ifdef Q_OS_WIN
    if (timer_) CloseHandle(static_cast<HANDLE>(timer_));
    if (multimedia_) {
        if (resolutionRequested_) {
            using Resolution = UINT (WINAPI*)(UINT);
            auto end = reinterpret_cast<Resolution>(GetProcAddress(static_cast<HMODULE>(multimedia_),"timeEndPeriod"));
            if (end) end(1);
        }
        FreeLibrary(static_cast<HMODULE>(multimedia_));
    }
#endif
}
void FrameWaiter::waitUntil(qint64 deadlineNs) {
    using Clock = std::chrono::steady_clock;
#ifdef Q_OS_WIN
    const qint64 remaining = deadlineNs-std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    if (remaining<=0) return;
    if (timer_) {
        LARGE_INTEGER due;
        due.QuadPart = -((remaining+99)/100); // Relative 100ns units; never round to zero.
        if (SetWaitableTimer(static_cast<HANDLE>(timer_),&due,0,nullptr,nullptr,FALSE) &&
            WaitForSingleObject(static_cast<HANDLE>(timer_),INFINITE)==WAIT_OBJECT_0) return;
    }
#endif
    std::this_thread::sleep_until(Clock::time_point(std::chrono::nanoseconds(deadlineNs)));
}
