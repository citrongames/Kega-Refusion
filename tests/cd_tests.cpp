#include "core/LibretroHost.h"
#include "core/SegaCdBios.h"
#include "core/Sega32xBios.h"
#include "core/RewindBuffer.h"
#include "net/NetplayTimeline.h"
#include "core/CoreFileSystem.h"
#include <QCryptographicHash>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QTemporaryDir>

#include <cstdio>
#include <stdexcept>
#include <vector>
#include <array>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "cannot create fixture");
    require(file.write(bytes) == bytes.size(), "cannot write fixture");
}

// This original, synthetic CD has enough structure to test transport and BIOS
// loading. It deliberately contains no borrowed Sega security or game code,
// and is not expected to execute as a retail BIOS boot program.
QByteArray makeDisc(char marker) {
    QByteArray bytes(150 * 2048, char(0));
    bytes.replace(0, 14, "SEGADISCSYSTEM");
    bytes[0x20b] = char(0); // NTSC-U in Genesis Plus GX's CD region detector.
    bytes[0x400] = marker; // Different original disc content and identity.
    return bytes;
}

QByteArray makeCartridge() {
    QByteArray bytes(32 * 1024, char(0));
    bytes[0] = char(0x00); bytes[1] = char(0xff);
    bytes[2] = char(0xff); bytes[3] = char(0x00);
    bytes[4] = char(0x00); bytes[5] = char(0x00);
    bytes[6] = char(0x02); bytes[7] = char(0x00);
    bytes.replace(0x100, 16, "SEGA MEGA DRIVE ");
    bytes.replace(0x1f0, 3, "USA");
    bytes[0x200] = char(0x60); bytes[0x201] = char(0xfe); // BRA.S *
    return bytes;
}

void requireFrame(LibretroHost& host) {
    host.runFrame();
    require(!host.frame().isNull(), "core produced no video frame");
    require(host.frame().width() >= 160 && host.frame().height() >= 144,
            "core produced invalid video geometry");
    require(host.audio().size() % 4 == 0, "core produced incomplete stereo PCM");
}

void requireBrmPath(const QString& corePath, const QString& expected) {
    QLibrary library(corePath);
    using Getter = bool (*)(unsigned, const void**, size_t*, const char**);
    const auto get = reinterpret_cast<Getter>(library.resolve("kegarefusion_get_bram"));
    require(get, "BRAM extension missing");
    const void* memory = nullptr;
    size_t size = 0;
    const char* rawPath = nullptr;
    require(get(0, &memory, &size, &rawPath) && memory && size == 8192 && rawPath,
            "CD internal BRAM unavailable");
    require(QDir::cleanPath(QString::fromUtf8(rawPath)) == QDir::cleanPath(expected),
            "CD BRAM save path changed with the mounted disc");
}

void testBackupRewind(LibretroHost& host, const QString& corePath) {
    QLibrary library(corePath);
    using Getter = bool (*)(unsigned, const void**, size_t*, const char**);
    const auto get = reinterpret_cast<Getter>(library.resolve("retro_kegarefusion_get_bram_raw"));
    const auto formatted = reinterpret_cast<Getter>(library.resolve("kegarefusion_get_bram"));
    require(get && formatted, "raw CD backup RAM extension missing");
    std::array<char*,2> memory{};
    std::array<QByteArray,2> original, changed, diskBefore;
    std::array<QString,2> paths;
    std::array<bool,2> existed{};
    for (unsigned i = 0; i < 2; ++i) {
        const void* data = nullptr; size_t size = 0; const char* path = nullptr;
        require(get(i,&data,&size,&path) && data && size >= 8192 && size <= 1024*1024 && path,
                "internal or external CD backup RAM unavailable");
        memory[i] = const_cast<char*>(static_cast<const char*>(data));
        original[i] = QByteArray(memory[i],qsizetype(size)); paths[i] = QString::fromUtf8(path);
        existed[i] = QFile::exists(paths[i]);
        if (existed[i]) { QFile file(paths[i]); require(file.open(QIODevice::ReadOnly), "backup RAM disk reference unavailable"); diskBefore[i] = file.readAll(); }
    }
    const auto state = host.captureRewindState(), movie = host.captureMovieState(false);
    require(state.startsWith("KRREW002") && !movie.isEmpty(), "CD backup regions absent from rewind");
    for (unsigned i = 0; i < 2; ++i) {
        memory[i][0] ^= char(0x31+i); memory[i][original[i].size()-1] ^= char(1);
        changed[i] = QByteArray(memory[i],original[i].size());
        const void* data = nullptr; size_t size = 0; const char* path = nullptr;
        require(!formatted(i,&data,&size,&path), "fixture did not invalidate the backup format marker");
    }
    const auto partialFormat = host.captureRewindState(), partialMovie = host.captureMovieState(false);
    require(!partialFormat.isEmpty() && !partialMovie.isEmpty(), "partially formatted CD backup RAM could not be captured");
    for (unsigned i = 0; i < 2; ++i) memory[i][0] ^= char(0x76);
    require(host.restoreRewindState(partialFormat), "partially formatted CD backup rewind restore failed");
    for (unsigned i = 0; i < 2; ++i)
        require(QByteArray(memory[i],changed[i].size()) == changed[i], "rewind lost partially formatted CD backup RAM bytes");
    require(host.restoreRewindState(state), "CD backup rewind restore failed");
    for (unsigned i = 0; i < 2; ++i)
        require(QByteArray(memory[i],original[i].size()) == original[i], "rewind did not restore internal/external CD backup RAM");
    QString error;
    require(host.restoreMovieState(partialMovie,error,true) && host.restoreMovieState(movie,error,true),
            "movie/network rewind could not recover CD backup RAM formatting");
    for (unsigned i = 0; i < 2; ++i) {
        require(QByteArray(memory[i],original[i].size()) == original[i], "movie rewind lost CD backup RAM");
        require(QFile::exists(paths[i]) == existed[i], "CD preview wrote a backup RAM file");
        if (existed[i]) { QFile file(paths[i]); require(file.open(QIODevice::ReadOnly) && file.readAll() == diskBefore[i], "CD preview changed backup RAM on disk"); }
    }
    const auto unchanged = host.captureRewindState();
    require(!host.restoreRewindState(unchanged.chopped(1)) && host.captureRewindState() == unchanged,
            "invalid CD backup envelope mutated the current state");
    std::printf("CD backup rewind: internal %lld + external %lld bytes and partial format restored; no file writes\n",
        static_cast<long long>(original[0].size()),static_cast<long long>(original[1].size()));
}

void testCd(const QString& corePath, const QString& biosSourcePath) {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create temporary test directory");
    const QString dataPath = temp.filePath("data");
    const QString disc1 = temp.filePath("disc1.iso");
    const QString disc2 = temp.filePath("disc2.iso");
    const QString disc3 = temp.filePath(QStringLiteral("внешний диск.iso"));
    const QString cue = temp.filePath("disc1.cue");
    const QString badChd = temp.filePath("broken.chd");
    const QString badCue = temp.filePath("missing-track.cue");
    const QString playlist = temp.filePath("discs.m3u");
    const QString cartridge = temp.filePath("test.md");
    writeFile(badChd,QByteArray("MComprHD",8)+QByteArray(256,0));
    writeFile(disc1, makeDisc('1'));
    writeFile(disc2, makeDisc('2'));
    writeFile(disc3, makeDisc('3'));
    writeFile(cue, "FILE \"disc1.iso\" BINARY\r\n  TRACK 01 MODE1/2048\r\n    INDEX 01 00:00:00\r\n");
    writeFile(badCue, "FILE \"absent.iso\" BINARY\r\n  TRACK 01 MODE1/2048\r\n    INDEX 01 00:00:00\r\n");
    writeFile(playlist, "# original generated images\r\ndisc1.cue\r\ndisc2.iso\r\n");
    writeFile(cartridge, makeCartridge());

    QString error;
    {
        LibretroHost host(corePath, dataPath);
        require(!host.load(cue, error) && error.contains("BIOS", Qt::CaseInsensitive),
                "missing CD BIOS was not explained");
    }
    error.clear();
    const QStringList imported = SegaCdBios::importDirectory(biosSourcePath, dataPath, error);
    require(!imported.isEmpty(), "no user BIOS images were imported");
    require(SegaCdBios::validate(dataPath, 2, error), "user NTSC-U BIOS is invalid or missing");

    LibretroHost host(corePath, dataPath);
    require(host.load(playlist, error), "generated CD playlist did not load");
    require(host.isCd() && host.discPaths().size() == 2 && host.discIndex() == 0 &&
            !host.discEjected(), "CD playlist did not register two discs");
    const QString playlistBrm = dataPath + "/saves/discs/scd_U.brm";
    requireBrmPath(corePath, playlistBrm);
    bool receivedAudio = false;
    for (int i = 0; i < 8; ++i) {
        requireFrame(host);
        receivedAudio |= !host.audio().isEmpty();
    }
    require(receivedAudio, "CD BIOS produced no audio samples");
    testBackupRewind(host,corePath);
    require(host.saveState(0, error), "CD state save failed");
    const QByteArray rewind = host.captureRewindState();
    require(!rewind.isEmpty(), "CD rewind capture failed");
    requireFrame(host);
    require(host.restoreRewindState(rewind), "CD rewind restore failed on same disc");
    require(host.loadState(0, error), "CD state load failed on same disc");
    bool mediaChanged = true;
    const auto unchangedDiscState = host.captureRewindState();
    require(host.selectDisc(0,error,&mediaChanged) && !mediaChanged &&
            host.captureRewindState() == unchangedDiscState,
            "selecting current CD disturbed the timeline");
    require(host.setDiscEjected(false,error,&mediaChanged) && !mediaChanged &&
            host.captureRewindState() == unchangedDiscState,
            "closing an already closed CD tray disturbed the timeline");

    require(host.selectDisc(1, error,&mediaChanged) && mediaChanged && host.discIndex() == 1 && !host.discEjected(),
            "CD disc selection failed");
    require(!host.loadState(0, error) && !error.isEmpty(),
            "state from a different disc was accepted");
    error.clear();
    require(host.selectDisc(0, error) && host.loadState(0, error),
            "original CD state was not recoverable after returning to disc 1");
    require(host.setDiscEjected(true, error,&mediaChanged) && mediaChanged && host.discEjected(), "CD eject failed");
    require(host.setDiscEjected(true,error,&mediaChanged) && !mediaChanged,
            "opening an already open CD tray reported a timeline change");
    require(!host.saveState(1, error) && !error.isEmpty(),
            "state was saved while the tray was open");
    error.clear();
    requireFrame(host);
    require(host.setDiscEjected(false, error) && !host.discEjected(), "CD tray did not close");
    require(host.loadState(0, error), "CD state did not load after closing the tray");

    require(host.changeDisc(disc3, error) && host.discPaths().size() == 2 &&
            host.discPaths().at(0) == disc3, "external disc replacement failed");
    requireBrmPath(corePath, playlistBrm);
    requireFrame(host);
    require(!host.changeDisc(badCue, error,&mediaChanged) && !mediaChanged && !error.isEmpty(),
            "CUE with missing track was accepted");
    require(host.isCd() && host.discPaths().at(0) == disc3 && !host.discEjected(),
            "failed CUE replacement lost the current disc");
    requireFrame(host);
    require(!host.changeDisc(badChd,error), "core accepted broken CHD");
    require(host.isCd() && host.discPaths().at(0)==disc3 && !host.discEjected(),
            "failed core mount did not restore previous disc");
    requireFrame(host);
    error.clear();
    require(host.hardReset(error) && host.isCd() && host.discPaths().size() == 2,
            "CD hard reset lost disc registration");
    requireBrmPath(corePath, playlistBrm);
    requireFrame(host);
    host.unload();
    require(!QFile::exists(playlistBrm), "unused playlist BRAM created a save");

    require(host.load(cartridge, error) && !host.isCd() && host.discPaths().isEmpty(),
            "cartridge load retained CD controls");
    requireFrame(host);
    require(host.load(cue, error) && host.isCd() && host.discPaths().size() == 1,
            "cartridge-to-CD switch failed");
    const QString cueBrm = dataPath + "/saves/disc1/scd_U.brm";
    requireBrmPath(corePath, cueBrm);
    requireFrame(host);
    require(host.load(cartridge, error) && !host.isCd() &&
            host.load(playlist, error) && host.isCd() && host.discPaths().size() == 2,
            "CD-to-cartridge-to-CD switch failed");
    requireBrmPath(corePath, playlistBrm);
    requireFrame(host);
    host.unload();
    require(!QFile::exists(cueBrm) && !QFile::exists(playlistBrm),
            "unused CD BRAM created a save");
}

MovieInput rewindControls(int frame) {
    MovieInput controls;
    const bool start = (frame >= 240 && frame < 246) || (frame >= 900 && frame < 906) ||
        (frame >= 1800 && frame % 300 < 6);
    controls.buttons[0] = (start ? 1U << RETRO_DEVICE_ID_JOYPAD_START : 0) |
        (frame >= 1800 && frame % 37 < 12 ? 1U << RETRO_DEVICE_ID_JOYPAD_RIGHT : 0) |
        (frame >= 1800 && frame % 53 < 5 ? 1U << RETRO_DEVICE_ID_JOYPAD_Y : 0);
    return controls;
}

// The production reverse path redraws a target from its predecessor and recorded
// controls, then restores the canonical target before allowing forward play.
// Exercise actual CDDA/PCM/media decoders rather than treating a successful load
// as proof that their next frame is unchanged.
void exerciseRewindWindow(LibretroHost& host, int firstFrame) {
    constexpr int frames = 64;
    struct Reference {
        QByteArray state, audio;
        QImage image;
        double aspect;
        InputDisplayState display;
        MovieInput controls;
    };
    std::vector<Reference> reference;
    RewindBuffer pixels, replay;
    pixels.configure(true); replay.configure(true);
    int replayCaptures = 0, pictureCaptures = 0;
    auto remember = [&](const MovieInput& controls, bool retain) {
        const auto display = host.inputDisplay();
        reference.push_back({host.captureRewindState(),host.audio(),host.frame(),host.aspectRatio(),display,controls});
        require(!reference.back().state.isEmpty() && !host.frame().isNull(), "CD rewind reference capture failed");
        if (!retain) return;
        require(pixels.capture(reference.back().state,host.frame(),host.aspectRatio(),&display), "CD picture history capture failed");
        const bool canReplay = reference.size() > 1;
        require(canReplay
            ? replay.captureForReplay(reference.back().state,host.aspectRatio(),controls,&display,host.rewindPreviewNeedsPair())
            : replay.capture(reference.back().state,host.frame(),host.aspectRatio(),&display),
            "CD state/input history capture failed");
        canReplay ? ++replayCaptures : ++pictureCaptures;
    };
    remember(rewindControls(firstFrame-1),true);
    for (int frame = 0; frame <= frames; ++frame) {
        const auto controls = rewindControls(firstFrame+frame);
        NetplayTimeline::apply(host,controls); host.runFrame();
        remember(controls,frame < frames);
    }
    const auto pixelBytes = pixels.bytesUsed(), replayBytes = replay.bytesUsed();
    require(replayCaptures > 0 && replayBytes < pixelBytes, "CD production rewind did not use compact state/input history");
    int audible = 0;
    for (const auto& expected : reference)
        for (char sample : expected.audio) if (sample) { ++audible; break; }
    for (int i = frames-1; i > 0; --i) {
        QByteArray target;
        QImage image;
        double aspect = 0;
        InputDisplayState display;
        RewindBuffer::ReplayFrame preview;
        require(replay.stepBack(target,image,aspect,1,&display,&preview), "CD state/input rewind step failed");
        require(target == reference[size_t(i)].state, "CD rewind target changed");
        require((preview.previousState.isEmpty() || preview.controls == reference[size_t(i)].controls) &&
                display == reference[size_t(i)].display, "CD rewind controls/display changed");
        // Compare the canonical target and ordinary forward continuation with
        // the existing picture path, then repeat after a redraw preview.
        require(host.restoreRewindState(target), "CD picture control restore failed");
        const auto normalizedTarget = host.captureRewindState();
        require(normalizedTarget == reference[size_t(i)].state, "CD native target did not round-trip exactly");
        std::vector<Reference> continuation;
        for (int next = 0; next < 4; ++next) {
            const auto controls = rewindControls(firstFrame+i+next);
            NetplayTimeline::apply(host,controls); host.runFrame();
            continuation.push_back({host.captureRewindState(),host.audio(),host.frame(),host.aspectRatio(),host.inputDisplay(),controls});
        }
        require(continuation.front().audio == reference[size_t(i+1)].audio &&
                continuation.front().image == reference[size_t(i+1)].image &&
                continuation.front().state == reference[size_t(i+1)].state,
                "CD picture restore changed the ordinary forward video/state/PCM continuation");
        if (!preview.previousState.isEmpty()) {
            host.beginRewindPreview();
            require(host.restoreRewindState(preview.previousState), "CD preview predecessor restore failed");
            if (preview.firstFieldControls) {
                NetplayTimeline::apply(host,*preview.firstFieldControls); host.runRewindPreviewFrame(true,false,false);
            }
            NetplayTimeline::apply(host,preview.controls); host.runRewindPreviewFrame(true,preview.singleFieldFallback);
            image = host.frame();
            require(host.audio().isEmpty(), "CD preview published audio");
        }
        if (image != reference[size_t(i)].image)
            throw std::runtime_error(QStringLiteral("CD rewind picture mismatch at %1").arg(firstFrame+i-1).toStdString());
        require(host.restoreRewindState(target), "CD canonical target restore failed");
        if (!preview.previousState.isEmpty()) host.endRewindPreview();
        require(host.captureRewindState() == normalizedTarget, "CD canonical target changed after preview");
        host.restoreFramePreview(reference[size_t(i)].image,reference[size_t(i)].aspect);
        for (const auto& next : continuation) {
            NetplayTimeline::apply(host,next.controls); host.runFrame();
            if (host.audio() != next.audio)
                throw std::runtime_error(QStringLiteral("CD replay changed picture-restore PCM at %1 (%2 vs %3 bytes)")
                    .arg(firstFrame+i).arg(host.audio().size()).arg(next.audio.size()).toStdString());
            if (host.frame() != next.image || host.captureRewindState() != next.state)
                throw std::runtime_error(QStringLiteral("CD replay changed picture-restore video/native state at %1").arg(firstFrame+i).toStdString());
        }
    }
    // Continue the original canonical timeline before the next sampling window.
    require(host.restoreRewindState(reference.back().state), "CD final timeline restore failed");
    host.restoreFramePreview(reference.back().image,reference.back().aspect);
    std::printf("CD rewind window %d: %d replay/%d picture captures; exact previews + 4-frame video/state/PCM continuation passed; %d audible frames; picture/state-input %lld/%lld bytes\n",
        firstFrame,replayCaptures,pictureCaptures,audible,static_cast<long long>(pixelBytes),static_cast<long long>(replayBytes));
}

void exerciseUserRewind(const QString& corePath, const QString& biosSource, const QString& path) {
    QTemporaryDir temp;
    require(temp.isValid(), "CD rewind temporary directory failed");
    QString error;
    SegaCdBios::importDirectory(biosSource,temp.path(),error);
    Sega32xBios::importDirectory(biosSource,temp.path(),error);
    LibretroHost host(corePath,temp.path());
    require(host.load(path,error),qPrintable(error));
    require(host.isCd(), "CD rewind source is not a Sega CD image");
    testBackupRewind(host,corePath);
    int frame = 0;
    for (int checkpoint : {300,1800,3600}) {
        while (frame < checkpoint) {
            NetplayTimeline::apply(host,rewindControls(frame)); host.runFrame(); ++frame;
        }
        exerciseRewindWindow(host,frame);
        frame += 65;
    }
    require(CoreFileSystem::lastError().isEmpty(), "CD rewind reported media decoding failure");
    std::printf("CD rewind state/input passed: %s; MP3 PCM bytes %llu\n",qPrintable(path),
        static_cast<unsigned long long>(CoreFileSystem::mp3PcmBytesRead()));
}

void exerciseUserDisc(const QString& corePath, const QString& biosSource, const QString& path, const QString& output) {
    QTemporaryDir temp;
    require(temp.isValid(), "temporary game data directory failed");
    QString error;
    SegaCdBios::importDirectory(biosSource,temp.path(),error);
    LibretroHost host(corePath,temp.path());
    if (!host.load(path,error)) throw std::runtime_error(error.toStdString());
    require(host.isCd(), "user image was not recognized as Sega CD");
    int audibleFrames = 0;
    QCryptographicHash pcmHash(QCryptographicHash::Sha256);
    for (int i = 0; i < 3600; ++i) {
        host.setButtons(0, ((i >= 240 && i < 246) || (i >= 900 && i < 906) ||
                            (i >= 1800 && (i % 300) < 6)) ? (1U << RETRO_DEVICE_ID_JOYPAD_START) : 0);
        host.runFrame();
        pcmHash.addData(host.audio());
        for (char sample : host.audio()) if (sample) { ++audibleFrames; break; }
        if (i == 299 || i == 1799 || i == 3599)
            require(host.frame().save(output + QStringLiteral("-%1.png").arg(i+1)), "game screenshot failed");
    }
    require(host.saveState(0,error), "user-disc state save failed");
    const auto rewind = host.captureRewindState();
    host.runFrame();
    require(host.restoreRewindState(rewind) && host.loadState(0,error), "user-disc state restore failed");
    host.runFrame();
        std::printf("User CD: 3600 frames, %d frames with nonzero PCM, %llu MP3 PCM bytes read, audio SHA256 %s; state/rewind restore passed.\n",audibleFrames,
        static_cast<unsigned long long>(CoreFileSystem::mp3PcmBytesRead()),pcmHash.result().toHex().constData());
    require(CoreFileSystem::lastError().isEmpty(), "virtual track reported runtime decoding failure");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3 && argc != 5) {
        std::fprintf(stderr, "usage: cd_tests CORE BIOS_SOURCE_DIRECTORY [USER_DISC SCREENSHOT_PREFIX|--rewind]\n");
        return 2;
    }
    try {
        if (argc == 5) {
            if (QString::fromLocal8Bit(argv[4]) == QLatin1String("--rewind")) {
                exerciseUserRewind(QString::fromLocal8Bit(argv[1]),QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3]));
                return 0;
            }
            exerciseUserDisc(QString::fromLocal8Bit(argv[1]),QString::fromLocal8Bit(argv[2]),
                             QString::fromLocal8Bit(argv[3]),QString::fromLocal8Bit(argv[4]));
            return 0;
        }
        testCd(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]));
        std::puts("Sega CD BIOS, disc transport, states and BRAM integration passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
