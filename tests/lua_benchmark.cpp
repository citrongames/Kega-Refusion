// Explicit real-ROM probe. Never reads or writes the player's saved game data.
// lua_benchmark CORE ROM REGION [WARMUP_FRAMES] [MEASURED_FRAMES]
#include "core/LibretroHost.h"
#include "core/MovieSession.h"
#include "script/ScriptRuntime.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>
#include <stdexcept>

namespace {
constexpr int Repeats = 3;
constexpr const char* Workloads[] = {"baseline", "no-scripts", "hud", "automation"};

void require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}

void startScript(ScriptRuntime& runtime, const LibretroHost& host, const QByteArray& source) {
    ScriptCommand command;
    command.id = 1;
    command.path = QStringLiteral("benchmark.lua");
    command.source = source;
    runtime.command(command, host);
    for (const auto& update : runtime.takeUpdates()) {
        if (update.status == ScriptStatus::Error)
            throw std::runtime_error(update.text.toStdString());
    }
    require(runtime.hasScripts(), "Benchmark script did not start");
}

void startHud(ScriptRuntime& runtime, const LibretroHost& host) {
    // Exercise three independent work RAM reads, formatted HUD text and
    // native-pixel graphics. Values need no game-specific address table.
    startScript(runtime, host, QByteArrayLiteral(R"(
local ram
for _, domain in ipairs(memory.domains()) do
    if domain.base == 0xff0000 and domain.size >= 0x10000 then
        ram = domain.id
        break
    end
end
assert(ram, "Mega Drive work RAM unavailable")
emu.on("frame", function()
    local a = assert(memory.read(ram, 0x100, 2))
    local b = assert(memory.read(ram, 0x120, 2))
    local c = assert(memory.read(ram, 0x140, 1))
    draw.text(8, 8, string.format("%04X %04X %02X", a, b, c), 0xffffffff, "hud")
    draw.rect(96 + (a % 32), 100 + (b % 32), 16, 16, 0x00ffffff, 0xff802040)
    draw.line(96, 96, 96 + (c % 32), 116, 0xffff00ff)
end)
)"));
}

void startAutomation(ScriptRuntime& runtime, const LibretroHost& host) {
    // Exercise the controller tables and merge without changing the neutral
    // input sequence. After timing, read the completed count via a draw event.
    startScript(runtime, host, QByteArrayLiteral(R"(
local sampled, observed = nil, 0
emu.on("beforeframe", function()
    sampled = assert(input.get(1))
    assert(input.set(1, {left = sampled.left, right = sampled.right}))
end)
emu.on("frame", function()
    local effective = assert(input.get(1))
    assert(sampled and effective.left == sampled.left and effective.right == sampled.right)
    sampled = nil
    observed = observed + 1
end)
emu.on("draw", function()
    draw.text(0, 0, tostring(observed))
end)
)"));
}

template<class BeforeFrame, class AfterFrame>
double timeFrames(LibretroHost& host, int frames, int stride,
                  BeforeFrame beforeFrame, AfterFrame afterFrame) {
    const bool audio = stride == 1;
    MovieSession movie(host, {});
    QString error;
    QElapsedTimer elapsed;
    elapsed.start();
    // End on one of the presented frames for a comparable video digest.
    for (int remaining = frames - 1; remaining >= 0; --remaining) {
        const bool present = remaining % stride == 0;
        MovieInput input;
        beforeFrame(input);
        if (!movie.beforeFrame(input, error)) throw std::runtime_error(error.toStdString());
        host.runFrame(present, audio);
        movie.afterFrame();
        afterFrame(present, input);
    }
    return double(elapsed.nsecsElapsed()) / (1000.0 * frames);
}

using Digests = std::array<QByteArray, 3>;
Digests digests(const LibretroHost& host, bool audio) {
    const QImage& frame = host.frame();
    const QByteArray state = host.serialize();
    require(!frame.isNull() && !state.isEmpty() && (!audio || !host.audio().isEmpty()),
            "missing final video, state or audio");
    const QByteArrayView video(reinterpret_cast<const char*>(frame.constBits()), frame.sizeInBytes());
    return {QCryptographicHash::hash(state, QCryptographicHash::Sha256).toHex(),
            QCryptographicHash::hash(video, QCryptographicHash::Sha256).toHex(),
            QCryptographicHash::hash(host.audio(), QCryptographicHash::Sha256).toHex()};
}

void measure(LibretroHost& host, const QByteArray& checkpoint, LibretroHost::RunClock clock,
             int frames, int stride) {
    std::array<std::array<double, Repeats>, std::size(Workloads)> samples{};
    Digests reference;
    for (int repeat = 0; repeat < Repeats; ++repeat) {
        // Rotate trial order to reduce systematic warm-up or thermal bias.
        for (int offset = 0; offset < int(std::size(Workloads)); ++offset) {
            const int workload = (repeat + offset) % int(std::size(Workloads));
            require(host.unserialize(checkpoint), "could not restore benchmark checkpoint");
            host.setRunClock(clock);

            if (workload == 0) {
                samples[workload][repeat] = timeFrames(host, frames, stride,
                    [](MovieInput&) {}, [](bool, const MovieInput&) {});
            } else {
                ScriptRuntime runtime;
                runtime.setHostPolicy(true, false, false);
                if (workload == 2) startHud(runtime, host);
                if (workload == 3) startAutomation(runtime, host);
                ScriptOverlayPtr overlay;
                samples[workload][repeat] = timeFrames(host, frames, stride,
                    [&](MovieInput& input) {
                        if (runtime.hasScripts())
                            runtime.beforeFrame(host, 1, host.runClock().frames, input);
                    },
                    [&](bool present, const MovieInput& input) {
                        if (runtime.hasScripts())
                            overlay = runtime.event(ScriptEvent::Frame, host, 1,
                                                    host.runClock().frames, 0, present, &input);
                    });
                if (workload == 3)
                    overlay = runtime.event(ScriptEvent::Draw, host, 1, host.runClock().frames);
                for (const auto& update : runtime.takeUpdates())
                    if (update.status == ScriptStatus::Error)
                        throw std::runtime_error(update.text.toStdString());
                if (workload == 2)
                    require(overlay && overlay->commands.size() == 3 &&
                            overlay->frame == host.runClock().frames, "HUD did not produce three commands for the last video frame");
                if (workload == 3)
                    require(overlay && overlay->commands.size() == 1 &&
                            overlay->commands.first().text == QString::number(frames),
                            "Automation did not sample and observe every frame");
            }

            const Digests actual = digests(host, stride == 1);
            if (repeat == 0 && workload == 0) reference = actual;
            for (size_t i = 0; i < actual.size(); ++i) {
                if (actual[i] != reference[i]) {
                    constexpr const char* fields[] = {"serialized state", "video", "PCM"};
                    const QString error = QStringLiteral("%1 differed for %2, stride %3, trial %4")
                        .arg(QString::fromLatin1(fields[i]), QString::fromLatin1(Workloads[workload]))
                        .arg(stride).arg(repeat + 1);
                    throw std::runtime_error(error.toStdString());
                }
            }
        }
    }

    std::printf("render=1/%d audio=%s state=%s video=%s pcm=%s\n",
                stride, stride == 1 ? "on" : "off",
                reference[0].constData(), reference[1].constData(), reference[2].constData());
    for (auto& sample : samples) std::sort(sample.begin(), sample.end());
    const double baseline = samples.front()[Repeats / 2];
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        const double median = sample[Repeats / 2];
        std::printf("  %-10s %8.2f us/frame (%7.2f to %7.2f), %+6.2f%% vs baseline\n",
                    Workloads[i], median, sample.front(), sample.back(), 100.0 * (median / baseline - 1.0));
    }
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        require(args.size() >= 4 && args.size() <= 6,
                "usage: lua_benchmark CORE ROM REGION [WARMUP_FRAMES] [MEASURED_FRAMES]");
        const int region = args[3].toInt();
        bool validWarmup = true;
        const int warmup = args.size() > 4 ? args[4].toInt(&validWarmup) : 360;
        const int frames = args.size() > 5 ? args[5].toInt() : 1800;
        require((region == 2 || region == 3) && validWarmup && warmup >= 0 && warmup <= 36000 &&
                frames >= 32 && frames <= 36000, "invalid region (2 USA, 3 Europe) or frame count");

        QTemporaryDir data;
        require(data.isValid(), "cannot create isolated data directory");
        LibretroHost host(args[1], data.path());
        QString error;
        if (!host.setRegion(region, error) || !host.load(args[2], error, -1, nullptr, false))
            throw std::runtime_error(error.toStdString());
        for (int i = 0; i < warmup; ++i) host.runFrame();
        const QByteArray saved = host.serialize();
        require(!saved.isEmpty(), "cannot save benchmark checkpoint");
        const auto clock = host.runClock();
        const double fps = host.fps();
        require(region == 2 ? fps > 59 && fps < 61 : fps > 49 && fps < 51,
                "the native refresh rate does not match the requested region");
        std::printf("%s %s %.5f Hz, %d unattended startup frames, %d timed frames per trial, %d trials each\n",
                    qPrintable(host.contentName()), region == 2 ? "NTSC-U" : "PAL", fps,
                    warmup, frames, Repeats);
        measure(host, saved, clock, frames, 1);
        measure(host, saved, clock, frames, 32);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Lua benchmark failed: %s\n", error.what());
        return 1;
    }
}
