// Explicit real-ROM probe with isolated per-run save data.
#include "core/LibretroHost.h"
#include "core/MovieSession.h"
#include "core/RewindBuffer.h"
#include "core/ScriptStateEnvelope.h"
#include "script/ScriptRuntime.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace {
constexpr int Repeats = 2;
enum Workload { Baseline, Empty, Constant, Counter, LargeStable, LargeChanging };
constexpr std::array WorkloadNames{"baseline", "empty runtime", "constant", "counter",
                                   "large fixed length", "large varying length"};

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void checkScriptUpdates(ScriptRuntime& runtime) {
    for (const auto& update : runtime.takeUpdates())
        if (update.status == ScriptStatus::Error) throw std::runtime_error(update.text.toStdString());
}

void startScript(ScriptRuntime& runtime, const LibretroHost& host, Workload workload) {
    ScriptCommand command;
    command.id = 1;
    command.path = QStringLiteral("timeline-benchmark.lua");
    command.source = QByteArrayLiteral("local mode = ") + QByteArray::number(workload) + QByteArrayLiteral(R"lua(
local initial = 0
if mode >= 4 then
    initial = {count = 0, a = string.rep("a", 4096), b = string.rep("b", 4096),
               c = string.rep("c", 4096), d = string.rep("d", 3900)}
end
assert(timeline.define("benchmark", 1, initial))
if mode >= 3 then
    emu.on("frame", function()
        local value = timeline.get()
        if mode == 3 then
            assert(timeline.set(value + 1))
        else
            value.count = value.count + 1
            if mode == 5 then value.d = string.rep("d", 3900 + value.count % 64) end
            assert(timeline.set(value))
        end
    end)
end
emu.on("draw", function()
    local value = timeline.get()
    draw.text(0, 0, tostring(type(value) == "table" and value.count or value))
end)
)lua");
    runtime.command(command, host);
    checkScriptUpdates(runtime);
    require(runtime.hasTimelineData(), "script could not declare timeline data");
}

using Digests = std::array<QByteArray, 3>;
Digests digests(const LibretroHost& host, bool audio) {
    const QImage& frame = host.frame();
    const QByteArray state = host.captureRewindState();
    require(!frame.isNull() && !state.isEmpty() && audio == !host.audio().isEmpty(),
            "missing final state or video, or unexpected PCM");
    const QByteArrayView video(reinterpret_cast<const char*>(frame.constBits()), frame.sizeInBytes());
    return {QCryptographicHash::hash(state, QCryptographicHash::Sha256).toHex(),
            QCryptographicHash::hash(video, QCryptographicHash::Sha256).toHex(),
            QCryptographicHash::hash(host.audio(), QCryptographicHash::Sha256).toHex()};
}

struct Sample {
    double microsecondsPerFrame;
    qsizetype historyBytes;
    size_t historyFrames;
    qsizetype scriptBytes;
};

Sample measure(LibretroHost& host, int frames, int stride, Workload workload) {
    std::unique_ptr<ScriptRuntime> runtime;
    QByteArray initial;
    if (workload != Baseline) {
        runtime = std::make_unique<ScriptRuntime>();
        if (workload != Empty) startScript(*runtime, host, workload);
        initial = runtime->captureTimeline();
        require(ScriptRuntime::validateTimeline(initial), "initial timeline data are invalid");
        if (workload == Empty) {
            const QByteArray state = host.captureRewindState();
            const QByteArray wrapped = ScriptStateEnvelope::wrap(state, initial);
            require(initial.isNull() && initial.capacity() == 0 &&
                    wrapped.constData() == state.constData(), "empty runtime allocated a timeline payload");
        }
    }
    const bool normal = stride == 1;
    RewindBuffer history;
    history.configure(normal);
    const MovieInput controls;
    MovieSession movie(host, {});
    QString error;
    QElapsedTimer timer;
    timer.start();
    for (int remaining = frames - 1; remaining >= 0; --remaining) {
        const bool present = remaining % stride == 0;
        if (!movie.beforeFrame(controls, error)) throw std::runtime_error(error.toStdString());
        host.runFrame(present, normal);
        movie.afterFrame();
        if (runtime && runtime->hasScripts())
            runtime->event(ScriptEvent::Frame, host, 1, host.runClock().frames, 0, present, &controls);
        if (normal) {
            const auto state = host.captureRewindState();
            const auto script = runtime ? runtime->captureTimeline() : QByteArray();
            if (!history.captureForReplay(state, host.aspectRatio(), controls, nullptr,
                                          host.rewindPreviewNeedsPair(), script))
                throw std::runtime_error("could not retain history");
        }
    }
    const double microseconds = double(timer.nsecsElapsed()) / (1000.0 * frames);
    QByteArray scriptData;
    if (runtime) {
        checkScriptUpdates(*runtime);
        scriptData = runtime->captureTimeline();
        require(ScriptRuntime::validateTimeline(scriptData), "final timeline data are invalid");
        if (workload <= Constant)
            require(scriptData.constData() == initial.constData(), "unchanged timeline data were rebuilt");
        if (workload >= Constant) {
            const auto overlay = runtime->event(ScriptEvent::Draw, host, 1, host.runClock().frames);
            checkScriptUpdates(*runtime);
            require(overlay && overlay->commands.size() == 1 &&
                    overlay->commands.first().text == QString::number(workload == Constant ? 0 : frames),
                    "script did not observe the expected number of frames");
        }
    }
    return {microseconds, history.bytesUsed(), history.size(), scriptData.size()};
}

void runTrials(LibretroHost& host, const QByteArray& checkpoint, int frames, int stride) {
    std::array<std::array<Sample, Repeats>, WorkloadNames.size()> samples{};
    Digests reference;
    for (int repeat = 0; repeat < Repeats; ++repeat) {
        for (size_t offset = 0; offset < WorkloadNames.size(); ++offset) {
            const size_t index = (offset + repeat) % WorkloadNames.size();
            require(host.restoreRewindState(checkpoint), "could not restore test checkpoint");
            samples[index][repeat] = measure(host, frames, stride, Workload(index));
            const auto actual = digests(host, stride == 1);
            if (index == Baseline && repeat == 0) reference = actual;
            if (actual != reference)
                throw std::runtime_error(QStringLiteral("State, video or PCM diverged for %1 at stride %2, trial %3")
                    .arg(QString::fromLatin1(WorkloadNames[index])).arg(stride).arg(repeat + 1).toStdString());
        }
    }
    std::printf("render=1/%d audio=%s rewind=%s state=%s video=%s pcm=%s\n", stride,
                stride == 1 ? "on" : "off", stride == 1 ? "on" : "off",
                reference[0].constData(), reference[1].constData(), reference[2].constData());
    const auto average = [](const auto& trials) {
        return (trials[0].microsecondsPerFrame + trials[1].microsecondsPerFrame) / Repeats;
    };
    const double baseline = average(samples[Baseline]);
    for (size_t i = 0; i < WorkloadNames.size(); ++i) {
        const auto& trials = samples[i];
        const double mean = average(trials);
        std::printf("  %-20s %8.2f us/frame (%7.2f, %7.2f) %+6.2f%%  history=%lld bytes/%zu frames  Lua=%lld bytes\n",
                    WorkloadNames[i], mean, trials[0].microsecondsPerFrame, trials[1].microsecondsPerFrame,
                    100.0 * (mean / baseline - 1.0),
                    static_cast<long long>(std::max(trials[0].historyBytes, trials[1].historyBytes)),
                    trials[1].historyFrames, static_cast<long long>(trials[1].scriptBytes));
    }
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        require(args.size() >= 4 && args.size() <= 6,
                "usage: lua_timeline_benchmark CORE ROM REGION [WARMUP_FRAMES] [MEASURED_FRAMES]");
        const int region = args[3].toInt();
        bool validWarmup = true;
        const int warmup = args.size() > 4 ? args[4].toInt(&validWarmup) : 120;
        const int frames = args.size() > 5 ? args[5].toInt() : 600;
        require((region == 2 || region == 3) && validWarmup && warmup >= 1 && warmup <= 36000 &&
                frames >= 32 && frames <= 36000, "invalid region (2 USA, 3 Europe) or frame count");

        QTemporaryDir data;
        require(data.isValid(), "cannot create isolated data directory");
        LibretroHost host(args[1], data.path());
        QString error;
        if (!host.setRegion(region, error) || !host.load(args[2], error, -1, nullptr, false))
            throw std::runtime_error(error.toStdString());
        for (int i = 0; i < warmup; ++i) host.runFrame();
        const QByteArray checkpoint = host.captureRewindState();
        require(!checkpoint.isEmpty(), "could not capture test checkpoint");
        const double fps = host.fps();
        require(region == 2 ? fps > 59 && fps < 61 : fps > 49 && fps < 51,
                "refresh rate does not match requested region");
        std::printf("%s %s %.5f Hz, %d unattended startup frames, %d timed frames per trial, %d trials each\n",
                    qPrintable(host.contentName()), region == 2 ? "NTSC-U" : "PAL", fps,
                    warmup, frames, Repeats);
        runTrials(host, checkpoint, frames, 1);
        runTrials(host, checkpoint, frames, 32);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Lua timeline benchmark failed: %s\n", error.what());
        return 1;
    }
}
