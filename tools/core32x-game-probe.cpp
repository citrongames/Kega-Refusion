#include "core/CoreFileSystem.h"
#include "core/LibretroHost.h"
#include "core/Sega32xBios.h"
#include "core/SegaCdBios.h"
#include "core/RewindBuffer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
int usage(const QString& detail = {}) {
    if (!detail.isEmpty()) std::fprintf(stderr, "%s\n", qPrintable(detail));
    std::fprintf(stderr, "Usage: core32x_game_probe CORE BIOS_SOURCE GAME FRAMES OUTPUT.png "
                         "[--no-input|--start-pulses|--play] [--filter=0..4] [--auto-fix-checksum] [--state-check] [--reset-check] [--checkpoint=N] [--record-rewind] [--reset-at=N] [--start-at=N] [--play-at=N] [--save-state=PATH] [--load-state=PATH] [--port2-disconnected] [--buttons=FRAME:COUNT:MASK] [--p2-buttons=FRAME:COUNT:MASK]... [--gun=menacer|justifier] [--companion-pad]\n");
    return 2;
}
void require(bool ok, const QString& error) {
    if (!ok) throw std::runtime_error(error.toStdString());
}
QByteArray frameHash(const QImage& frame) {
    const QImage pixels = frame.convertToFormat(QImage::Format_RGBA8888);
    require(!pixels.isNull(), QStringLiteral("Could not convert final video frame."));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int row = 0; row < pixels.height(); ++row)
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(pixels.constScanLine(row)),
                                    qsizetype(pixels.width()) * 4));
    return hash.result().toHex();
}
void savePng(const QImage& frame, const QString& path) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()),
            QStringLiteral("Could not create output directory: %1").arg(QFileInfo(path).absolutePath()));
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly),
            QStringLiteral("Could not open output PNG %1: %2").arg(path, file.errorString()));
    QImageWriter writer(&file, "PNG");
    const bool written = writer.write(frame);
    require(written, QStringLiteral("Could not encode output PNG: %1").arg(writer.errorString()));
    const bool committed = file.commit();
    require(committed, QStringLiteral("Could not save output PNG %1: %2").arg(path, file.errorString()));
}
struct FutureHashes {
    QByteArray video, audio;
};
FutureHashes futureHashes(LibretroHost& host) {
    QCryptographicHash video(QCryptographicHash::Sha256), audio(QCryptographicHash::Sha256);
    host.setButtons(0, 0);
    host.setButtons(1, 0);
    for (int frame = 0; frame < 30; ++frame) {
        host.runFrame(true, true);
        const auto& image = host.frame();
        require(!image.isNull(), QStringLiteral("State check produced no video at future frame %1.").arg(frame));
        const QByteArray shape = QByteArray::number(image.width()) + 'x' + QByteArray::number(image.height()) + ':';
        video.addData(QByteArrayView(shape));
        const QByteArray pixels = frameHash(image);
        video.addData(QByteArrayView(pixels));
        audio.addData(QByteArrayView(host.audio()));
    }
    require(CoreFileSystem::lastError().isEmpty(),
            QStringLiteral("Disc read failed during state check: %1").arg(CoreFileSystem::lastError()));
    return {video.result().toHex(), audio.result().toHex()};
}
QJsonObject checkState(LibretroHost& host, bool has32x) {
    const QByteArray state = host.serialize();
    require(!state.isEmpty(), QStringLiteral("State check: core serialization is unavailable or failed."));
    const qsizetype magic = state.indexOf(QByteArrayLiteral("KR32X001"), 1);
    if (has32x) {
        require(magic > 0, QStringLiteral("State check: KR32X001 extension header is missing."));
        QByteArray corrupt = state;
        corrupt[magic] = char(quint8(corrupt[magic]) ^ 0x20u);
        require(!host.unserialize(corrupt), QStringLiteral("State check: corrupted 32X extension was accepted."));
        require(host.serialize() == state,
                QStringLiteral("State check: rejected extension changed the serialized machine state."));
    }
    const QByteArray rewind = host.captureRewindState();
    require(!rewind.isEmpty(), QStringLiteral("State check: rewind capture failed."));
    require(host.restoreRewindState(rewind), QStringLiteral("State check: rewind restore failed."));
    require(host.unserialize(state), QStringLiteral("State check: first restore of valid state failed."));
    const FutureHashes first = futureHashes(host);
    require(host.unserialize(state), QStringLiteral("State check: second restore of valid state failed."));
    const FutureHashes second = futureHashes(host);
    require(first.video == second.video,
            QStringLiteral("State check: future video differs after two restores (%1 vs %2).")
                .arg(QString::fromLatin1(first.video), QString::fromLatin1(second.video)));
    require(first.audio == second.audio,
            QStringLiteral("State check: future PCM differs after two restores (%1 vs %2).")
                .arg(QString::fromLatin1(first.audio), QString::fromLatin1(second.audio)));
    return {{"enabled", true}, {"serialize_bytes", qint64(state.size())},
            {"extension_magic_offset", qint64(magic)}, {"corrupt_extension_rejected", has32x},
            {"state_unchanged_after_reject", has32x}, {"rewind_roundtrip", true},
            {"rewind_bytes", qint64(rewind.size())}, {"future_frames_per_restore", 30},
            {"extra_frames", 60}, {"future_video_match", true}, {"future_audio_match", true},
            {"future_video_sha256_rgba", QString::fromLatin1(first.video)},
            {"future_audio_sha256_pcm16", QString::fromLatin1(first.audio)}};
}
QJsonObject checkReset(LibretroHost& host, const QString& output) {
    const QFileInfo info(output);
    const QString base = info.absolutePath() + QLatin1Char('/') + info.completeBaseName();
    const QString softPath = base + QStringLiteral(".soft-reset.png");
    const QString hardPath = base + QStringLiteral(".hard-reset.png");
    const auto runAfterReset = [&host](const QString& kind) {
        host.setButtons(0, 0);
        host.setButtons(1, 0);
        for (int frame = 0; frame < 600; ++frame) host.runFrame(true, true);
        require(CoreFileSystem::lastError().isEmpty(),
                QStringLiteral("Disc read failed after %1 reset: %2").arg(kind, CoreFileSystem::lastError()));
        const QImage image = host.frame();
        require(!image.isNull(), QStringLiteral("Core produced no video after %1 reset.").arg(kind));
        return image.copy();
    };
    host.setButtons(0, 0);
    host.setButtons(1, 0);
    host.reset();
    const QImage soft = runAfterReset(QStringLiteral("soft"));
    savePng(soft, softPath);
    QString error;
    const bool hardReset = host.hardReset(error);
    require(hardReset, QStringLiteral("Hard reset failed: %1").arg(error));
    const QImage hard = runAfterReset(QStringLiteral("hard"));
    savePng(hard, hardPath);
    return {{"enabled", true}, {"frames_per_reset", 600}, {"extra_frames", 1200},
            {"soft_output_png", softPath}, {"hard_output_png", hardPath},
            {"soft_video_sha256_rgba", QString::fromLatin1(frameHash(soft))},
            {"hard_video_sha256_rgba", QString::fromLatin1(frameHash(hard))}};
}
QJsonArray labels(const QStringList& list) {
    QJsonArray result;
    for (const auto& item : list) result.append(item);
    return result;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() < 6) return usage();
    bool validCount = false;
    const int frames = args[4].toInt(&validCount);
    if (!validCount || frames < 1 || frames > 36000)
        return usage(QStringLiteral("FRAMES must be an integer from 1 to 36000."));
    enum class Input { None, StartPulses, Play };
    Input input = Input::None;
    ControllerType gun = ControllerType::Pad;
    constexpr unsigned gunPlayer = 1;
    bool companionPad = false, autoFixChecksum = false;
    bool inputSpecified = false, stateCheck = false, resetCheck = false, filterSpecified = false;
    int filter = 0, checkpoint = 0, extraStart = -1, extraPlay = -1, resetAt = -1;
    QString saveRawState, loadRawState;
    int heldAt = -1, heldCount = 0;
    unsigned heldMask = 0;
    struct ButtonInterval { int at, count; unsigned mask; };
    std::vector<ButtonInterval> p2Intervals;
    bool recordRewind = false, port2Disconnected = false;
    for (int i = 6; i < args.size(); ++i) {
        const auto& option = args[i];
        if (option == QLatin1String("--no-input") || option == QLatin1String("--start-pulses") ||
            option == QLatin1String("--play")) {
            if (inputSpecified) return usage(QStringLiteral("Choose exactly one input option."));
            inputSpecified = true;
            if (option == QLatin1String("--start-pulses")) input = Input::StartPulses;
            else if (option == QLatin1String("--play")) input = Input::Play;
        } else if (option == QLatin1String("--auto-fix-checksum")) {
            autoFixChecksum = true;
        } else if (option == QLatin1String("--state-check")) {
            if (stateCheck) return usage(QStringLiteral("Duplicate --state-check option."));
            stateCheck = true;
        } else if (option == QLatin1String("--reset-check")) {
            if (resetCheck) return usage(QStringLiteral("Duplicate --reset-check option."));
            resetCheck = true;
        } else if (option.startsWith(QLatin1String("--save-state="))) {
            saveRawState = option.mid(13);
            if (saveRawState.isEmpty()) return usage(QStringLiteral("Empty state path."));
        } else if (option.startsWith(QLatin1String("--load-state="))) {
            loadRawState = option.mid(13);
            if (loadRawState.isEmpty()) return usage(QStringLiteral("Empty state path."));
        } else if (option.startsWith(QLatin1String("--reset-at="))) {
            bool valid = false;
            resetAt = option.mid(11).toInt(&valid);
            if (!valid || resetAt < 1 || resetAt >= frames)
                return usage(QStringLiteral("--reset-at requires a frame inside the run."));
        } else if (option.startsWith(QLatin1String("--play-at="))) {
            bool valid = false;
            const int value = option.mid(10).toInt(&valid);
            if (extraPlay >= 0 || !valid || value < 0 || value + 4 > frames)
                return usage(QStringLiteral("--play-at must leave four input frames."));
            extraPlay = value;
        } else if (option.startsWith(QLatin1String("--buttons="))) {
            const auto parts = option.mid(10).split(QLatin1Char(':'));
            bool atOk = false, countOk = false, maskOk = false;
            if (parts.size() != 3 || heldAt >= 0) return usage(QStringLiteral("Use --buttons=FRAME:COUNT:MASK once."));
            heldAt = parts[0].toInt(&atOk); heldCount = parts[1].toInt(&countOk);
            heldMask = parts[2].toUInt(&maskOk, 0);
            if (!atOk || !countOk || !maskOk || heldAt < 0 || heldAt >= frames ||
                heldCount < 1 || heldCount > frames - heldAt || heldMask > 0xffff)
                return usage(QStringLiteral("Button interval or libretro mask is out of range."));
        } else if (option.startsWith(QLatin1String("--p2-buttons="))) {
            const auto parts = option.mid(13).split(QLatin1Char(':'));
            bool atOk = false, countOk = false, maskOk = false;
            if (parts.size() != 3 || p2Intervals.size() >= 64)
                return usage(QStringLiteral("Use at most 64 --p2-buttons=FRAME:COUNT:MASK intervals."));
            const int at = parts[0].toInt(&atOk), count = parts[1].toInt(&countOk);
            const unsigned mask = parts[2].toUInt(&maskOk, 0);
            if (!atOk || !countOk || !maskOk || at < 0 || at >= frames ||
                count < 1 || count > frames - at || mask > 0xffff)
                return usage(QStringLiteral("P2 button interval or libretro mask is out of range."));
            p2Intervals.push_back({at, count, mask});
        } else if (option == QLatin1String("--port2-disconnected")) {
            port2Disconnected = true;
        } else if (option == QLatin1String("--record-rewind")) {
            if (recordRewind) return usage(QStringLiteral("Duplicate --record-rewind."));
            recordRewind = true;
        } else if (option.startsWith(QLatin1String("--start-at="))) {
            bool valid = false;
            const int value = option.mid(11).toInt(&valid);
            if (extraStart >= 0 || !valid || value < 0 || value + 4 > frames)
                return usage(QStringLiteral("--start-at must leave four input frames."));
            extraStart = value;
        } else if (option.startsWith(QLatin1String("--checkpoint="))) {
            bool valid = false;
            const int value = option.mid(13).toInt(&valid);
            if (checkpoint || !valid || value < 1 || value > frames || frames / value > 20)
                return usage(QStringLiteral("--checkpoint requires a frame interval producing at most 20 images."));
            checkpoint = value;
        } else if (option.startsWith(QLatin1String("--filter="))) {
            bool valid = false;
            const int value = option.mid(9).toInt(&valid);
            if (filterSpecified || !valid || value < 0 || value > 4)
                return usage(QStringLiteral("--filter requires one value from 0 to 4."));
            filterSpecified = true;
            filter = value;
        } else if (option == QLatin1String("--gun=menacer")) gun = ControllerType::Menacer;
        else if (option == QLatin1String("--gun=justifier")) gun = ControllerType::Justifier;
        else if (option == QLatin1String("--companion-pad")) companionPad = true;
        else return usage(QStringLiteral("Unknown option: %1").arg(option));
    }
    if (!p2Intervals.empty() && (port2Disconnected || gun != ControllerType::Pad))
        return usage(QStringLiteral("--p2-buttons requires an enabled P2 pad."));
    try {
        const QString core = QFileInfo(args[1]).absoluteFilePath();
        const QString source = QFileInfo(args[2]).absoluteFilePath();
        const QString game = QFileInfo(args[3]).absoluteFilePath();
        const QString output = QFileInfo(args[5]).absoluteFilePath();
        require(QFileInfo(core).isFile(), QStringLiteral("Core file not found: %1").arg(core));
        require(QFileInfo(game).isFile(), QStringLiteral("Game file not found: %1").arg(game));
        QTemporaryDir temp;
        require(temp.isValid(), QStringLiteral("Could not create isolated probe data directory."));
        QString error;
        // Explicitly authorized imports into a fresh temporary directory only.
        // ROMs and source BIOS files are opened read-only by the existing helpers.
        const auto x32Imported = Sega32xBios::importDirectory(source, temp.path(), error);
        const bool validBios = Sega32xBios::validate(temp.path(), error);
        require(validBios, QStringLiteral("32X BIOS import failed: %1").arg(error));
        QString cdImportError;
        const auto cdImported = SegaCdBios::importDirectory(source, temp.path(), cdImportError);
        LibretroHost host(core, temp.path());
        host.message = [](const QString& text) { std::fprintf(stderr, "[core] %s\n", qPrintable(text)); };
        require(host.setAutoFixChecksum(autoFixChecksum, error), QStringLiteral("Could not configure checksum correction."));
        require(host.setBlarggFilter(filter, error), QStringLiteral("Could not set filter: %1").arg(error));
        if (gun != ControllerType::Pad) {
            host.setControllerKind(gunPlayer,gun);
            host.setControllerConnected(gunPlayer,true);
            host.setControllerConnected(gunPlayer^1u,companionPad);
            host.setGunAim(gunPlayer,0,0,false);
        }
        QElapsedTimer loadClock;
        loadClock.start();
        const bool loaded = host.load(game, error);
        if (!loaded && !cdImportError.isEmpty()) error += QLatin1Char('\n') + cdImportError;
        require(loaded, QStringLiteral("Game load failed: %1").arg(error));
        const double loadMs = loadClock.nsecsElapsed() / 1.0e6;
        QLibrary diagnostics(core);
        using Features = unsigned (*)();
        const auto features = reinterpret_cast<Features>(diagnostics.resolve("retro_kegarefusion_core_features"));
        const unsigned initialFeatures = features ? features() : 0;
        using Active = int (*)();
        const auto papriumCdActive = reinterpret_cast<Active>(
            diagnostics.resolve("retro_kegarefusion_paprium_cd_active"));
        const bool initialPapriumCdActive = papriumCdActive && papriumCdActive() != 0;
        using Debug = int (*)(std::uint64_t*, size_t);
        const auto debug = reinterpret_cast<Debug>(diagnostics.resolve("retro_kegarefusion_debug"));
        if (!loadRawState.isEmpty()) {
            QFile file(loadRawState);
            require(file.open(QIODevice::ReadOnly) && file.size() > 0 && file.size() <= 4*1024*1024,
                    QStringLiteral("Could not read bounded diagnostic state."));
            require(host.unserialize(file.readAll()), QStringLiteral("Diagnostic state rejected."));
        }

        if (port2Disconnected) {
            using SetDevice = void (*)(unsigned, unsigned);
            const auto setDevice = reinterpret_cast<SetDevice>(diagnostics.resolve("retro_set_controller_port_device"));
            require(setDevice != nullptr, QStringLiteral("Controller device API missing."));
            setDevice(1, RETRO_DEVICE_NONE);
        }

        if (!p2Intervals.empty()) host.setControllerConnected(1, true);

        qint64 videoUpdates = 0, framesWithVideo = 0, audioChunks = 0, audioBytes = 0;
        qsizetype minAudioBytes = std::numeric_limits<qsizetype>::max(), maxAudioBytes = 0;
        int maxWidth = 0, maxHeight = 0;
        QCryptographicHash audioHash(QCryptographicHash::Sha256);
        RewindBuffer history;
        if (recordRewind) history.configure(true);
        QElapsedTimer clock;
        clock.start();
        for (int frame = 0; frame < frames; ++frame) {
            if (frame == resetAt) host.reset();
            const bool start = input != Input::None && ((frame >= 120 && frame < 124) ||
                                  (frame >= 240 && frame < 244) || (frame >= 480 && frame < 484));
            const bool extra = extraStart >= 0 && frame >= extraStart && frame < extraStart + 4;
            unsigned buttons = (start || extra) ? (1u << RETRO_DEVICE_ID_JOYPAD_START) : 0u;
            if ((input == Input::Play && ((frame >= 600 && frame < 604) || (frame >= 720 && frame < 724))) ||
                (extraPlay >= 0 && frame >= extraPlay && frame < extraPlay + 4))
                // GPX maps Mega Drive A/B/C to libretro Y/B/A respectively.
                buttons |= (1u << RETRO_DEVICE_ID_JOYPAD_Y) | (1u << RETRO_DEVICE_ID_JOYPAD_B) |
                           (1u << RETRO_DEVICE_ID_JOYPAD_A);
            if (heldAt >= 0 && frame >= heldAt && frame < heldAt + heldCount) buttons |= heldMask;
            host.setButtons(gun == ControllerType::Pad ? 0 : gunPlayer, buttons);
            if (companionPad) host.setButtons(gunPlayer^1u,buttons);
            if (!p2Intervals.empty()) {
                unsigned p2Buttons = 0;
                for (const auto& interval : p2Intervals)
                    if (frame >= interval.at && frame < interval.at + interval.count)
                        p2Buttons |= interval.mask;
                host.setButtons(1, p2Buttons);
            }
            const auto previous = host.frame().cacheKey();
            host.runFrame(true, true);
            if (!host.frame().isNull()) {
                ++framesWithVideo;
                if (host.frame().cacheKey() != previous) ++videoUpdates;
                maxWidth = std::max(maxWidth, host.frame().width());
                maxHeight = std::max(maxHeight, host.frame().height());
            }
            if (recordRewind && (frame + 1) % 2 == 0)
                require(history.capture(host.captureRewindState(), host.frame(), host.aspectRatio()),
                        QStringLiteral("Rewind recording failed at frame %1.").arg(frame));
            if (checkpoint && (frame + 1) % checkpoint == 0) {
                const QFileInfo target(output);
                const QString path = target.absolutePath() + QLatin1Char('/') +
                    target.completeBaseName() + QStringLiteral(".f%1.png").arg(frame + 1);
                savePng(host.frame(), path);
                std::fprintf(stderr, "checkpoint frame=%d video=%s\n", frame + 1,
                    frameHash(host.frame()).constData());
                std::uint64_t raw[112]{};
                if (debug && debug(raw, 112)) {
                    QJsonArray values;
                    for (auto value : raw) values.append(QString::number(value, 16));
                    std::fprintf(stderr, "debug %s\n",
                        QJsonDocument(values).toJson(QJsonDocument::Compact).constData());
                }
            }
            const auto& pcm = host.audio();
            if (!pcm.isEmpty()) {
                ++audioChunks; audioBytes += pcm.size();
                minAudioBytes = std::min(minAudioBytes, pcm.size());
                maxAudioBytes = std::max(maxAudioBytes, pcm.size());
                audioHash.addData(QByteArrayView(pcm));
            }
        }
        const double elapsedMs = clock.nsecsElapsed() / 1.0e6;
        if (!saveRawState.isEmpty()) {
            const QByteArray raw = host.serialize();
            QSaveFile file(saveRawState);
            require(!raw.isEmpty() && file.open(QIODevice::WriteOnly) &&
                    file.write(raw) == raw.size() && file.commit(),
                    QStringLiteral("Could not save diagnostic state."));
        }
        const unsigned benchmarkFeatures = features ? features() : 0;
        using Stats = int (*)(std::uint64_t*, size_t);
        const auto stats = reinterpret_cast<Stats>(diagnostics.resolve("retro_kegarefusion_core_stats"));
        std::uint64_t counters[42]{};
        // Capture benchmark counters before state/reset checks run extra frames
        // or recreate a machine and clear its diagnostic counters.
        const bool haveStats = stats && stats(counters, 42);
        host.setButtons(0, 0);
        require(CoreFileSystem::lastError().isEmpty(),
                QStringLiteral("Disc read failed: %1").arg(CoreFileSystem::lastError()));
        const QImage image = host.frame().copy();
        require(!image.isNull(), QStringLiteral("Core produced no video after %1 frames.").arg(frames));
        savePng(image, output);
        QElapsedTimer stateClock;
        stateClock.start();
        QJsonObject stateChecks = stateCheck ? checkState(host, (benchmarkFeatures & 2u) != 0) : QJsonObject{{"enabled", false}, {"extra_frames", 0}};
        if (stateCheck) stateChecks.insert("elapsed_ms", stateClock.nsecsElapsed() / 1.0e6);
        QElapsedTimer resetClock;
        resetClock.start();
        QJsonObject resetChecks = resetCheck ? checkReset(host, output) : QJsonObject{{"enabled", false}, {"extra_frames", 0}};
        if (resetCheck) resetChecks.insert("elapsed_ms", resetClock.nsecsElapsed() / 1.0e6);
        QJsonObject result{
            {"core", core}, {"game", game}, {"output_png", output},
            {"isolated_data", true}, {"is_cd", host.isCd()},
            {"paprium_cd_active", initialPapriumCdActive},
            {"initial_core_features", int(initialFeatures)}, {"core_features", int(benchmarkFeatures)},
            {"bios_32x_imported", labels(x32Imported)}, {"bios_cd_imported", labels(cdImported)},
            {"input", input == Input::Play ? "start_pulses_120_123_240_243_480_483_abc_600_603_720_723" :
                      input == Input::StartPulses ? "start_pulses_120_123_240_243_480_483" : "none"},
            {"filter", filter}, {"reset_at", resetAt}, {"checkpoint_interval", checkpoint}, {"extra_start_frame", extraStart}, {"extra_play_frame", extraPlay},
            {"held_buttons_frame", heldAt}, {"held_buttons_count", heldCount}, {"held_buttons_mask", qint64(heldMask)},
            {"rewind_recording", recordRewind}, {"rewind_bytes", qint64(history.bytesUsed())}, {"state_checks", stateChecks}, {"reset_checks", resetChecks},
            {"frames_requested", frames}, {"frames_run", frames},
            {"load_ms", loadMs}, {"elapsed_ms", elapsedMs},
            {"emulated_frames_per_second", elapsedMs > 0 ? frames * 1000.0 / elapsedMs : 0.0},
            {"nominal_fps", host.fps()}, {"sample_rate", host.sampleRate()},
            // Host exposes the latest video image and per-run aggregated PCM.
            // These observations do not pretend to count raw libretro callbacks.
            {"frames_with_video", framesWithVideo}, {"video_frame_updates", videoUpdates},
            {"video_width", image.width()}, {"video_height", image.height()},
            {"max_video_width", maxWidth}, {"max_video_height", maxHeight},
            {"aspect_ratio", host.aspectRatio()},
            {"video_sha256_rgba", QString::fromLatin1(frameHash(image))},
            {"audio_output_chunks", audioChunks}, {"audio_bytes", audioBytes},
            {"audio_stereo_frames", audioBytes / 4},
            {"audio_min_chunk_bytes", audioChunks ? qint64(minAudioBytes) : 0},
            {"audio_max_chunk_bytes", qint64(maxAudioBytes)},
            {"audio_sha256_pcm16", QString::fromLatin1(audioHash.result().toHex())}
        };
        if (haveStats) {
            QJsonObject values;
            const char* names[] = {"jit_calls", "jit_cycles", "nested_calls", "nested_cycles", "fallback_calls", "fallback_cycles"};
            for (int i = 0; i < 6; ++i)
                values[names[i]] = QJsonArray{qint64(counters[2*i]), qint64(counters[2*i+1])};
            values["cache_misses"] = qint64(counters[12]); values["cache_evictions"] = qint64(counters[13]);
            values["requested_cycles"] = QJsonArray{qint64(counters[16]), qint64(counters[17])};
            values["min_budget"] = QJsonArray{qint64(counters[18]), qint64(counters[19])};
            values["max_budget"] = QJsonArray{qint64(counters[20]), qint64(counters[21])};
            for (int cpu = 0; cpu < 2; ++cpu) {
                QJsonArray bins;
                for (int bin = 0; bin < 10; ++bin) bins.append(qint64(counters[22+cpu*10+bin]));
                values[cpu ? "slave_budget_bins" : "master_budget_bins"] = bins;
            }
            result["drc_stats"] = values;
        }
        const auto json = QJsonDocument(result).toJson(QJsonDocument::Compact);
        std::printf("%s\n", json.constData());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "32X game probe failed: %s\n", error.what());
        return 1;
    }
}
