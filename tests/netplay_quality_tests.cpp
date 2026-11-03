#include "net/NetDelayPolicy.h"

#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void high(NetDelayPolicy& policy, std::int64_t nowMs, bool eligible = true) {
    policy.observe(200, 20, 60.0, nowMs, eligible, false);
}

void lanAndTransient() {
    NetDelayPolicy lan;
    for (int i = 0; i < 40; ++i) {
        lan.observe(1, 0, 60.0, i * 1000, true, true);
        require(lan.frames() == 0 && lan.pending() == 0,
                "LAN samples must keep zero input delay");
    }

    NetDelayPolicy transient;
    high(transient, 0);
    transient.observe(1, 0, 60.0, 1000, true, true);
    high(transient, 2000);
    high(transient, 3000);
    require(transient.pending() == 0 && transient.frames() == 0,
            "one transient and two later high samples must not accumulate into a rise");
    transient.observe(1, 0, 60.0, 4000, true, true);
    require(transient.pending() == 0,
            "a transient must not leave a pending delay increase");
}

void boundedRiseAndAcknowledgement() {
    NetDelayPolicy policy;
    std::int64_t now = 0;
    for (int expected = 1; expected <= NetDelayPolicy::MaxDelay; ++expected) {
        high(policy, now);
        high(policy, now + 1000);
        require(policy.pending() == expected - 1,
                "delay rise requires three high samples");
        high(policy, now + 2000);
        require(policy.pending() == expected && policy.frames() == expected - 1,
                "persistent congestion must request exactly one additional frame");
        high(policy, now + 2100);
        require(policy.pending() == expected && policy.frames() == expected - 1,
                "an unacknowledged delay request must not accumulate more frames");
        policy.applied(policy.pending(), now + 2100);
        require(policy.frames() == expected && policy.pending() == expected,
                "applied acknowledgement must establish the actual delay");
        now += 7100;
    }
    for (int i = 0; i < 30; ++i) {
        high(policy, now + i * 1000);
        require(policy.frames() == 4 && policy.pending() == 4,
                "persistent high latency must remain bounded at four frames");
    }
    policy.applied(100, now);
    require(policy.frames() == 4 && policy.pending() == 4,
            "applied delay must clamp to the four-frame limit");
    policy.applied(-1, now);
    require(policy.frames() == 0 && policy.pending() == 0,
            "applied delay must not become negative");
}

void riseCooldown() {
    NetDelayPolicy policy;
    policy.applied(1, 10000);
    high(policy, 11000);
    high(policy, 12000);
    high(policy, 14999);
    require(policy.pending() == 1,
            "three high samples must not bypass the five-second rise cooldown");
    high(policy, 15000);
    require(policy.pending() == 2 && policy.frames() == 1,
            "rise must become eligible exactly five seconds after the last change");
    policy.applied(2, 15000);
    high(policy, 15001);
    high(policy, 15002);
    high(policy, 19999);
    require(policy.pending() == 2,
            "the next rise must get a fresh five-second cooldown");
    high(policy, 20000);
    require(policy.pending() == 3,
            "continued high latency must allow another one-frame rise after cooldown");
}

void quietSlowFall() {
    NetDelayPolicy policy;
    policy.applied(3, 0);
    for (int i = 0; i < 14; ++i) {
        policy.observe(1, 0, 60.0, 16000 + i * 1000, true, true);
        require(policy.pending() == 3,
                "elapsed cooldown alone must not bypass fifteen quiet samples");
    }
    policy.observe(1, 0, 60.0, 30000, true, true);
    require(policy.pending() == 2 && policy.frames() == 3,
            "fifteen quiet samples must request only a one-frame decrease");
    policy.applied(2, 30000);
    for (int i = 1; i <= 15; ++i)
        policy.observe(1, 0, 60.0, 30000 + i, true, true);
    require(policy.pending() == 2,
            "fifteen quiet samples must not bypass the fifteen-second fall cooldown");
    policy.observe(1, 0, 60.0, 44999, true, true);
    require(policy.pending() == 2,
            "fall must remain blocked one millisecond before cooldown expires");
    policy.observe(1, 0, 60.0, 45000, true, true);
    require(policy.pending() == 1 && policy.frames() == 2,
            "fall must become eligible exactly fifteen seconds after the last change");

    NetDelayPolicy noisy;
    noisy.applied(2, 0);
    for (int i = 0; i < 30; ++i)
        noisy.observe(1, 0, 60.0, 20000 + i * 1000, true, false);
    require(noisy.pending() == 2, "active rollback/noise must prevent a delay decrease");
    for (int i = 0; i < 14; ++i)
        noisy.observe(1, 0, 60.0, 50000 + i * 1000, true, true);
    noisy.observe(1, 0, 60.0, 64000, true, false);
    for (int i = 0; i < 14; ++i)
        noisy.observe(1, 0, 60.0, 65000 + i * 1000, true, true);
    require(noisy.pending() == 2,
            "a nonquiet sample must reset the quiet decrease streak");
    noisy.observe(1, 0, 60.0, 79000, true, true);
    require(noisy.pending() == 1,
            "a fresh complete quiet streak must allow a one-frame decrease");
}

void freezeOnPauseOrStaleStats() {
    NetDelayPolicy policy;
    policy.applied(1, 0);
    high(policy, 5000);
    high(policy, 6000);
    high(policy, 7000, false);
    require(policy.pending() == 1 && policy.frames() == 1,
            "ineligible paused/stale samples must freeze the applied delay");
    high(policy, 8000);
    high(policy, 9000);
    require(policy.pending() == 1,
            "resuming must start a fresh high-sample streak");
    high(policy, 10000);
    require(policy.pending() == 2,
            "three fresh eligible samples after resume must allow a rise");
    high(policy, 11000, false);
    require(policy.pending() == 1,
            "suspending must cancel an unapplied delay request");
    for (int i = 0; i < 30; ++i)
        policy.observe(1, 0, 60.0, 20000 + i * 1000, false, true);
    require(policy.pending() == 1 && policy.frames() == 1,
            "long paused/stale quiet periods must not decrease delay");

    for (int i = 0; i < 30; ++i)
        policy.observe(-1, 0, 60.0, 60000 + i * 1000, true, true);
    require(policy.pending() == 1 && policy.frames() == 1,
            "unknown RTT must not be treated as a zero-latency connection");
}

void regionAndJitterTargets() {
    NetDelayPolicy pal, ntsc;
    for (int i = 0; i < 3; ++i) {
        pal.observe(68, 0, 50.0, i * 1000, true, true);
        ntsc.observe(68, 0, 60.0, i * 1000, true, true);
    }
    require(pal.pending() == 0 && ntsc.pending() == 1,
            "68ms RTT must require zero frames at 50Hz and one at 60Hz");

    NetDelayPolicy steady, variable;
    for (int i = 0; i < 3; ++i) {
        steady.observe(60, 1, 60.0, i * 1000, true, true);
        variable.observe(60, 2, 60.0, i * 1000, true, true);
    }
    require(steady.pending() == 0 && variable.pending() == 1,
            "two jitter budgets and ceiling must affect the target at a frame boundary");
}

void freshSessionAndInvalidFps() {
    NetDelayPolicy oldSession;
    oldSession.applied(4, 1000);
    NetDelayPolicy newSession;
    require(newSession.frames() == 0 && newSession.pending() == 0,
            "a new session must not inherit the previous session's delay");
    high(newSession, 0);
    high(newSession, 1000);
    high(newSession, 2000);
    require(newSession.pending() == 1,
            "a new session must not inherit the previous session's cooldown");

    const double invalid[] = {0.0, -60.0, std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()};
    for (double fps : invalid) {
        NetDelayPolicy policy;
        policy.applied(2, 0);
        high(policy, 5000);
        high(policy, 6000);
        policy.observe(200, 20, fps, 7000, true, false);
        require(policy.frames() == 2 && policy.pending() == 2,
                "invalid FPS must freeze delay without an invalid numeric conversion");
        high(policy, 8000);
        high(policy, 9000);
        require(policy.pending() == 2,
                "invalid FPS must reset the sample streak");
        high(policy, 10000);
        require(policy.pending() == 3,
                "valid FPS must recover after three fresh samples");
    }
}

void rttEstimate() {
    NetRttEstimate estimate;
    require(estimate.pingMs() == -1 && estimate.jitterMs() == 0,
            "RTT must remain explicitly unknown before the first quality reply");
    estimate.sample(100);
    require(estimate.pingMs() == 100 && estimate.jitterMs() == 0,
            "first quality reply must seed RTT without artificial jitter");
    estimate.sample(140);
    require(estimate.pingMs() == 110 && estimate.jitterMs() == 10,
            "RTT and successive-sample variation must use one-quarter EWMA");
    estimate.sample(120);
    require(estimate.pingMs() == 113 && estimate.jitterMs() == 13,
            "RTT and variation must retain fractional EWMA values before rounding");
    estimate.sample(120);
    require(estimate.pingMs() == 114 && estimate.jitterMs() == 9,
            "unchanged RTT must decay jitter from successive RTT differences");
    estimate.sample(80);
    require(estimate.pingMs() == 106 && estimate.jitterMs() == 17,
            "RTT variation must use the previous raw sample rather than the smoothed mean");
    NetRttEstimate newSession;
    require(newSession.pingMs() == -1 && newSession.jitterMs() == 0,
            "new RTT estimator must not inherit previous peer measurements");
}
} // namespace

int main() {
    try {
        lanAndTransient();
        boundedRiseAndAcknowledgement();
        riseCooldown();
        quietSlowFall();
        freezeOnPauseOrStaleStats();
        regionAndJitterTargets();
        freshSessionAndInvalidFps();
        rttEstimate();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "netplay quality tests failed: %s\n", error.what());
        return 1;
    }
    std::puts("netplay quality: LAN/transient, bounded rise, cooldown, quiet fall, pause/stale freeze, region/jitter targets, fresh session, invalid FPS and RTT EWMA passed");
    return 0;
}
