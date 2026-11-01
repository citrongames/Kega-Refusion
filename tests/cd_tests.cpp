#include "core/LibretroHost.h"
#include "core/SegaCdBios.h"
#include "core/CoreFileSystem.h"
#include <QCryptographicHash>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>
#include <stdexcept>

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

void requireBrm(const QString& savesPath) {
    const auto files = QDir(savesPath).entryInfoList({"*.brm"}, QDir::Files);
    for (const auto& file : files)
        if (file.size() == 8192) return;
    throw std::runtime_error("CD internal 8 KiB BRAM was not persisted on unload");
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
    bool receivedAudio = false;
    for (int i = 0; i < 8; ++i) {
        requireFrame(host);
        receivedAudio |= !host.audio().isEmpty();
    }
    require(receivedAudio, "CD BIOS produced no audio samples");
    require(host.saveState(0, error), "CD state save failed");
    const QByteArray rewind = host.captureRewindState();
    require(!rewind.isEmpty(), "CD rewind capture failed");
    requireFrame(host);
    require(host.restoreRewindState(rewind), "CD rewind restore failed on same disc");
    require(host.loadState(0, error), "CD state load failed on same disc");

    require(host.selectDisc(1, error) && host.discIndex() == 1 && !host.discEjected(),
            "CD disc selection failed");
    require(!host.loadState(0, error) && !error.isEmpty(),
            "state from a different disc was accepted");
    error.clear();
    require(host.selectDisc(0, error) && host.loadState(0, error),
            "original CD state was not recoverable after returning to disc 1");
    require(host.setDiscEjected(true, error) && host.discEjected(), "CD eject failed");
    require(!host.saveState(1, error) && !error.isEmpty(),
            "state was saved while the tray was open");
    error.clear();
    requireFrame(host);
    require(host.setDiscEjected(false, error) && !host.discEjected(), "CD tray did not close");
    require(host.loadState(0, error), "CD state did not load after closing the tray");

    require(host.changeDisc(disc3, error) && host.discPaths().size() == 2 &&
            host.discPaths().at(0) == disc3, "external disc replacement failed");
    requireFrame(host);
    require(!host.changeDisc(badCue, error) && !error.isEmpty(),
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
    requireFrame(host);
    host.unload();
    requireBrm(dataPath + "/saves");

    require(host.load(cartridge, error) && !host.isCd() && host.discPaths().isEmpty(),
            "cartridge load retained CD controls");
    requireFrame(host);
    require(host.load(cue, error) && host.isCd() && host.discPaths().size() == 1,
            "cartridge-to-CD switch failed");
    requireFrame(host);
    require(host.load(cartridge, error) && !host.isCd() &&
            host.load(playlist, error) && host.isCd() && host.discPaths().size() == 2,
            "CD-to-cartridge-to-CD switch failed");
    requireFrame(host);
    host.unload();
    requireBrm(dataPath + "/saves");
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
        std::fprintf(stderr, "usage: cd_tests CORE BIOS_SOURCE_DIRECTORY [USER_DISC SCREENSHOT_PREFIX]\n");
        return 2;
    }
    try {
        if (argc == 5) {
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
