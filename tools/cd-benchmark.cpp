#include "core/CoreFileSystem.h"
#include "core/LibretroHost.h"
#include "core/SegaCdBios.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {

constexpr int warmFrames = 3600;
constexpr int timedFrames = 12000;
constexpr int realtimeFrames = 600;

struct Result {
    const char* format = "";
    int pair = 0;
    int order = 0;
    double loadMs = 0;
    double seconds = 0;
    double fps = 0;
    double speed = 0;
    quint64 mp3PcmBytes = 0;
    QByteArray firstFrameHash;
    QByteArray lastFrameHash;
    double cpuPercentOneCore = -1;
    double workingSetAverageMiB = -1;
    double workingSetPeakMiB = -1;
    double privateAverageMiB = -1;
    double privatePeakMiB = -1;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

unsigned buttons(int frame) {
    const bool start = (frame >= 240 && frame < 246) ||
                       (frame >= 900 && frame < 906) ||
                       (frame >= 1800 && ((frame - 1800) % 300) < 6);
    return start ? (1u << RETRO_DEVICE_ID_JOYPAD_START) : 0u;
}

QByteArray frameHash(const QImage& frame) {
    require(!frame.isNull(), "game produced no video frame");
    const QImage pixels = frame.convertToFormat(QImage::Format_RGBA8888);
    require(!pixels.isNull(), "could not convert video frame for comparison");
    return QCryptographicHash::hash(
        QByteArray(reinterpret_cast<const char*>(pixels.constBits()), pixels.sizeInBytes()),
        QCryptographicHash::Sha256).toHex();
}

#ifdef _WIN32
quint64 processCpuTicks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user),
            "cannot read process CPU time");
    const auto ticks = [](const FILETIME& value) {
        return (quint64(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    return ticks(kernel) + ticks(user);
}

struct MemorySamples {
    double workingSetSum = 0;
    double workingSetPeak = 0;
    double privateSum = 0;
    double privatePeak = 0;
    int count = 0;

    void sample() {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        require(GetProcessMemoryInfo(GetCurrentProcess(),
                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)),
                "cannot read process memory use");
        constexpr double mebibyte = 1024.0 * 1024.0;
        const double workingSet = double(counters.WorkingSetSize) / mebibyte;
        const double privateBytes = double(counters.PrivateUsage) / mebibyte;
        workingSetSum += workingSet;
        privateSum += privateBytes;
        workingSetPeak = std::max(workingSetPeak, workingSet);
        privatePeak = std::max(privatePeak, privateBytes);
        ++count;
    }
};
#endif

Result measure(const QString& corePath, const QString& biosSource,
               const QString& cuePath, const char* format, int pair, int order) {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create isolated benchmark data directory");
    QString error;
    const auto imported = SegaCdBios::importDirectory(biosSource, temp.path(), error);
    if (imported.isEmpty()) throw std::runtime_error(error.toStdString());

    LibretroHost host(corePath, temp.path());
    QElapsedTimer clock;
    clock.start();
    if (!host.load(cuePath, error)) throw std::runtime_error(error.toStdString());
    Result result;
    result.format = format;
    result.pair = pair;
    result.order = order;
    result.loadMs = clock.nsecsElapsed() / 1.0e6;
    require(host.isCd(), "benchmark image was not recognized as Sega CD");

    // Use the same controller input and emulated frame numbers for each image.
    // This warmup reaches in-game CD audio before timing begins.
    for (int frame = 0; frame < warmFrames; ++frame) {
        host.setButtons(0, buttons(frame));
        host.runFrame(true, true);
        if (frame == 0) result.firstFrameHash = frameHash(host.frame());
    }
    require(CoreFileSystem::lastError().isEmpty(), "CD read failed during warmup");

    const quint64 beforePcm = CoreFileSystem::mp3PcmBytesRead();
    // The final warmup Start pulse has completed; release it before timing.
    host.setButtons(0, 0);
    clock.restart();
    for (int frame = 0; frame < timedFrames; ++frame) {
        host.runFrame(true, true);
    }
    result.seconds = clock.nsecsElapsed() / 1.0e9;
    result.mp3PcmBytes = CoreFileSystem::mp3PcmBytesRead() - beforePcm;
    if (!CoreFileSystem::lastError().isEmpty())
        throw std::runtime_error(CoreFileSystem::lastError().toStdString());
    result.lastFrameHash = frameHash(host.frame());
    require(result.seconds > 0 && std::isfinite(result.seconds), "invalid elapsed time");
    result.fps = timedFrames / result.seconds;
    result.speed = result.fps / host.fps();
    if (format[0] == 'M')
        require(result.mp3PcmBytes > 1024u * 1024u,
                "MP3 PCM was not read during the timed segment");
    host.unload();
    return result;
}

Result measureRealtime(const QString& corePath, const QString& biosSource,
                       const QString& cuePath, const char* format) {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create isolated realtime data directory");
    QString error;
    const auto imported = SegaCdBios::importDirectory(biosSource, temp.path(), error);
    if (imported.isEmpty()) throw std::runtime_error(error.toStdString());
    LibretroHost host(corePath, temp.path());
    QElapsedTimer loadClock;
    loadClock.start();
    if (!host.load(cuePath, error)) throw std::runtime_error(error.toStdString());
    Result result;
    result.format = format;
    result.loadMs = loadClock.nsecsElapsed() / 1.0e6;
    require(host.isCd(), "realtime image was not recognized as Sega CD");

    for (int frame = 0; frame < warmFrames; ++frame) {
        host.setButtons(0, buttons(frame));
        host.runFrame(true, true);
        if (frame == 0) result.firstFrameHash = frameHash(host.frame());
    }
    require(CoreFileSystem::lastError().isEmpty(), "CD read failed during realtime warmup");
    const quint64 beforePcm = CoreFileSystem::mp3PcmBytesRead();
    host.setButtons(0, 0);

    using Clock = std::chrono::steady_clock;
    const double targetFps = host.fps();
    require(targetFps > 0 && std::isfinite(targetFps), "invalid core frame rate");
#ifdef _WIN32
    MemorySamples memory;
    memory.sample();
    const quint64 cpuStart = processCpuTicks();
#endif
    const auto begin = Clock::now();
    auto nextMemorySample = begin + std::chrono::milliseconds(100);
    for (int frame = 0; frame < realtimeFrames; ++frame) {
        host.runFrame(true, true);
        const auto target = begin + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(double(frame + 1) / targetFps));
        std::this_thread::sleep_until(target);
#ifdef _WIN32
        const auto now = Clock::now();
        if (now >= nextMemorySample) {
            memory.sample();
            nextMemorySample = now + std::chrono::milliseconds(100);
        }
#endif
    }
    const auto end = Clock::now();
#ifdef _WIN32
    const quint64 cpuEnd = processCpuTicks();
    memory.sample();
#endif
    result.seconds = std::chrono::duration<double>(end - begin).count();
    result.mp3PcmBytes = CoreFileSystem::mp3PcmBytesRead() - beforePcm;
    if (!CoreFileSystem::lastError().isEmpty())
        throw std::runtime_error(CoreFileSystem::lastError().toStdString());
    result.lastFrameHash = frameHash(host.frame());
    require(result.seconds > 0 && std::isfinite(result.seconds), "invalid realtime elapsed time");
    result.fps = realtimeFrames / result.seconds;
    result.speed = result.fps / targetFps;
#ifdef _WIN32
    result.cpuPercentOneCore = double(cpuEnd - cpuStart) * 1.0e-7 / result.seconds * 100.0;
    result.workingSetAverageMiB = memory.workingSetSum / memory.count;
    result.workingSetPeakMiB = memory.workingSetPeak;
    result.privateAverageMiB = memory.privateSum / memory.count;
    result.privatePeakMiB = memory.privatePeak;
#endif
    if (format[0] == 'M')
        require(result.mp3PcmBytes > 1024u * 1024u,
                "MP3 PCM was not read during the realtime segment");
    host.unload();
    return result;
}

void printRun(const Result& result, bool videoMatchesReference) {
    std::printf("run,%d,%d,%s,%.3f,%.6f,%.3f,%.3f,%llu,%s,%s,%s,\n",
        result.pair, result.order, result.format,
        result.loadMs, result.seconds, result.fps, result.speed,
        static_cast<unsigned long long>(result.mp3PcmBytes),
        result.firstFrameHash.constData(), result.lastFrameHash.constData(),
        videoMatchesReference ? "true" : "false");
}

void printRealtime(const Result& result, const char* videoMatchesReference) {
    std::printf("realtime,%s,%.3f,%.6f,%.3f,%.3f,%llu,%s,%s,%s,",
        result.format, result.loadMs, result.seconds, result.fps, result.speed,
        static_cast<unsigned long long>(result.mp3PcmBytes),
        result.firstFrameHash.constData(), result.lastFrameHash.constData(),
        videoMatchesReference);
#ifdef _WIN32
    std::printf("%.3f,%.3f,%.3f,%.3f,%.3f\n",
        result.cpuPercentOneCore, result.workingSetAverageMiB,
        result.workingSetPeakMiB, result.privateAverageMiB, result.privatePeakMiB);
#else
    std::puts("N/A,N/A,N/A,N/A,N/A");
#endif
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString mode = argc == 6 ? QString::fromLocal8Bit(argv[5]) : QString();
    const bool realtime = mode == "--realtime";
    const bool realtimeBin = mode == "--realtime-bin";
    const bool realtimeMp3 = mode == "--realtime-mp3";
    if (argc != 5 && !(realtime || realtimeBin || realtimeMp3)) {
        std::fprintf(stderr, "usage: cd_benchmark CORE BIOS_SOURCE CUE_BIN CUE_MP3 [--realtime|--realtime-bin|--realtime-mp3]\n");
        return 2;
    }
    try {
        const QString corePath = QString::fromLocal8Bit(argv[1]);
        const QString biosSource = QString::fromLocal8Bit(argv[2]);
        const QString binCue = QString::fromLocal8Bit(argv[3]);
        const QString mp3Cue = QString::fromLocal8Bit(argv[4]);
        if (realtimeBin || realtimeMp3) {
            const Result result = measureRealtime(corePath, biosSource,
                realtimeMp3 ? mp3Cue : binCue, realtimeMp3 ? "MP3" : "BIN");
            std::puts("phase,format,load_ms,timed_seconds,emu_fps,speed_x,mp3_pcm_bytes,first_video_sha256,last_video_sha256,video_matches_bin,cpu_percent_one_core,working_set_avg_mib,working_set_peak_mib,private_avg_mib,private_peak_mib");
            printRealtime(result, "N/A");
            return 0;
        }
        if (realtime) {
            const Result bin = measureRealtime(corePath, biosSource, binCue, "BIN");
            const Result mp3 = measureRealtime(corePath, biosSource, mp3Cue, "MP3");
            const bool videoMatches = bin.firstFrameHash == mp3.firstFrameHash &&
                                      bin.lastFrameHash == mp3.lastFrameHash;
            std::puts("phase,format,load_ms,timed_seconds,emu_fps,speed_x,mp3_pcm_bytes,first_video_sha256,last_video_sha256,video_matches_bin,cpu_percent_one_core,working_set_avg_mib,working_set_peak_mib,private_avg_mib,private_peak_mib");
            printRealtime(bin, "true");
            printRealtime(mp3, videoMatches ? "true" : "false");
            if (!videoMatches)
                std::fprintf(stderr, "Note: BIN and MP3 disc video hashes differ; verify that the game revisions match.\n");
            return 0;
        }
        // AB then BA offsets cache and thermal effects without mixing content.
        const std::array<std::array<bool, 2>, 2> order{{{{false, true}}, {{true, false}}}};
        std::array<Result, 4> runs;
        for (int pair = 0; pair < 2; ++pair)
            for (int position = 0; position < 2; ++position) {
                const bool mp3 = order[pair][position];
                runs[pair * 2 + position] = measure(corePath, biosSource,
                    mp3 ? mp3Cue : binCue, mp3 ? "MP3" : "BIN", pair + 1, position + 1);
            }

        std::puts("phase,pair,order,format,load_ms,timed_seconds,emu_fps,speed_x,mp3_pcm_bytes,first_video_sha256,last_video_sha256,video_matches_bin,paired_penalty_percent");
        double penaltySum = 0;
        bool allVideoMatches = true;
        for (int pair = 0; pair < 2; ++pair) {
            const Result& first = runs[pair * 2];
            const Result& second = runs[pair * 2 + 1];
            const Result& bin = first.format[0] == 'B' ? first : second;
            const Result& mp3 = first.format[0] == 'M' ? first : second;
            const bool firstMatch = bin.firstFrameHash == mp3.firstFrameHash;
            const bool lastMatch = bin.lastFrameHash == mp3.lastFrameHash;
            allVideoMatches &= firstMatch && lastMatch;
            printRun(first, first.format[0] == 'B' || (firstMatch && lastMatch));
            printRun(second, second.format[0] == 'B' || (firstMatch && lastMatch));
            const double penalty = 100.0 * (mp3.seconds / bin.seconds - 1.0);
            penaltySum += penalty;
            std::printf("pair_penalty,%d,,,,,,,,,,,%+.3f\n", pair + 1, penalty);
        }
        std::printf("mean_penalty,,,,,,,,,,,,%+.3f\n", penaltySum / 2.0);
        if (!allVideoMatches)
            std::fprintf(stderr, "Note: BIN and MP3 disc video hashes differ; verify that source images contain the same game revision.\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
