#include <archive.h>
#include <archive_entry.h>
#include "core/RewindBuffer.h"
#include "core/LibretroHost.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "cannot create test file");
    require(file.write(bytes) == bytes.size(), "cannot write test file");
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "cannot read test file");
    return file.readAll();
}

void putLong(QByteArray& bytes, int offset, quint32 value) {
    for (int i = 0; i < 4; ++i)
        bytes[offset + i] = char((value >> (24 - 8 * i)) & 0xff);
}

// A tiny, original 68000 program. It changes CRAM colour zero once per VBlank.
// There is no external ROM fixture or copyrighted game content.
QByteArray makeTestRom() {
    QByteArray rom(64 * 1024, char(0xff));
    putLong(rom, 0x00, 0x00ff0000); // initial 68000 stack pointer
    putLong(rom, 0x04, 0x00000200); // reset vector
    rom.replace(0x100, 16, "SEGA GENESIS    ");
    rom.replace(0x120, 16, "CODEX TEST ROM  ");
    rom.replace(0x150, 16, "CODEX TEST ROM  ");
    rom.replace(0x180, 2, "GM");
    putLong(rom, 0x1a0, 0);
    putLong(rom, 0x1a4, quint32(rom.size() - 1));
    putLong(rom, 0x1a8, 0x00ff0000);
    putLong(rom, 0x1ac, 0x00ffffff);
    rom.replace(0x1f0, 1, "U");

    int pc = 0x200;
    const auto word = [&](quint16 value) {
        rom[pc++] = char(value >> 8);
        rom[pc++] = char(value & 0xff);
    };
    // MOVE.W #$8144, ($C00004).L: enable the VDP display.
    word(0x33fc); word(0x8144); word(0x00c0); word(0x0004);
    // MOVE.W #$8700, ($C00004).L: backdrop uses CRAM colour zero.
    word(0x33fc); word(0x8700); word(0x00c0); word(0x0004);
    word(0x7000); // MOVEQ #0,D0
    const int waitVBlank = pc;
    word(0x3239); word(0x00c0); word(0x0004); // MOVE.W VDP status,D1
    word(0x0241); word(0x0008);                // ANDI.W #8,D1
    const int branchOn = pc;
    word(0x6700);                              // BEQ.S waitVBlank
    // MOVE.L #$C0000000, ($C00004).L: set CRAM write address zero.
    word(0x23fc); word(0xc000); word(0x0000); word(0x00c0); word(0x0004);
    word(0x33c0); word(0x00c0); word(0x0000); // MOVE.W D0, ($C00000).L
    word(0x5440);                              // ADDQ.W #2,D0
    const int waitVBlankEnd = pc;
    word(0x3239); word(0x00c0); word(0x0004);
    word(0x0241); word(0x0008);
    const int branchOff = pc;
    word(0x6600);                              // BNE.S waitVBlankEnd
    const int branchAgain = pc;
    word(0x6000);                              // BRA.S waitVBlank
    for (const auto [at, target] : {std::pair{branchOn, waitVBlank},
                                    std::pair{branchOff, waitVBlankEnd},
                                    std::pair{branchAgain, waitVBlank}}) {
        const int displacement = target - (at + 2);
        require(displacement >= -128 && displacement <= 127 && displacement != 0,
                "test ROM branch out of range");
        rom[at + 1] = char(displacement & 0xff);
    }
    return rom;
}

QByteArray makeSaveRamRom() {
    QByteArray rom = makeTestRom();
    rom.replace(0x1b0, 2, "RA");
    rom[0x1b2] = char(0xf8);
    rom[0x1b3] = char(0x20);
    putLong(rom, 0x1b4, 0x00200000);
    putLong(rom, 0x1b8, 0x0020ffff);
    // Prefix the existing video program with MOVE.B #$42,($200000).L.
    // All branches in the copied program remain relative to each other.
    const QByteArray oldProgram = rom.mid(0x200, 0x100);
    putLong(rom, 0x04, 0x00000300);
    rom[0x300] = char(0x13); rom[0x301] = char(0xfc);
    rom[0x302] = char(0x00); rom[0x303] = char(0x42);
    rom[0x304] = char(0x00); rom[0x305] = char(0x20);
    rom[0x306] = char(0x00); rom[0x307] = char(0x00);
    rom.replace(0x308, oldProgram.size(), oldProgram);
    return rom;
}

QString stateFile(const QString& dataPath, const QByteArray& rom, int slot) {
    const auto key = QCryptographicHash::hash(rom, QCryptographicHash::Sha256).toHex();
    return dataPath + "/states/" + QString::fromLatin1(key) + QStringLiteral(".slot%1.krs").arg(slot);
}

struct Output {
    QByteArray frame;
    QByteArray audio;
    bool operator==(const Output& other) const { return frame == other.frame && audio == other.audio; }
};

Output runAndCapture(LibretroHost& host) {
    host.runFrame();
    require(!host.frame().isNull(), "core did not produce a video frame");
    const QImage image = host.frame().convertToFormat(QImage::Format_RGBA8888);
    require(!image.isNull() && image.width() >= 160 && image.height() >= 144,
            "core produced invalid video geometry");
    const QByteArray pixels(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes());
    require(host.audio().size() % 4 == 0, "core submitted incomplete stereo PCM frames");
    return {QCryptographicHash::hash(pixels, QCryptographicHash::Sha256), host.audio()};
}

void requireRejected(bool accepted, const QString& error, const char* description) {
    require(!accepted && !error.isEmpty(), description);
}

void testCore(const QString& corePath) {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create temporary test directory");
    const QString dataPath = temp.path() + "/data";
    const auto rom = makeTestRom();
    const QString romPath = temp.path() + "/test.md";
    writeFile(romPath, rom);

    Output expectedAfterSlotZero;
    QString error;
    {
    LibretroHost host(corePath, dataPath);
    require(host.serialize().isEmpty(), "unloaded host serialized a state");
    require(!host.unserialize(QByteArray(10, '\0')), "unloaded host accepted a state");
    requireRejected(host.saveState(0, error), error, "unloaded host saved a state");
    error.clear();
    requireRejected(host.load(temp.path() + "/missing.md", error), error,
                    "missing ROM was accepted or had no error");
    error.clear();
    const QString tinyPath = temp.path() + "/tiny.md";
    writeFile(tinyPath, QByteArray(24, '\0'));
    requireRejected(host.load(tinyPath, error), error, "tiny ROM was accepted or had no error");
    require(!host.loaded(), "host marked invalid ROM as loaded");

    error.clear();
    require(host.load(romPath, error), "generated Mega Drive ROM did not load");
    require(host.loaded() && host.gamePath() == romPath, "host reported wrong loaded game");
    require(std::isfinite(host.fps()) && host.fps() > 40 && host.fps() < 70,
            "invalid core frame rate");
    require(host.sampleRate() >= 8000 && host.sampleRate() <= 192000,
            "invalid core audio sample rate");

    bool heardAudio = false;
    for (int i = 0; i < 8; ++i) heardAudio |= !runAndCapture(host).audio.isEmpty();
    require(heardAudio, "core did not submit audio samples");
    const QByteArray checkpoint = host.serialize();
    require(!checkpoint.isEmpty(), "core did not serialize");
    require(!host.unserialize(checkpoint.left(checkpoint.size() - 1)),
            "core accepted truncated raw serialization");

    std::vector<Output> firstPass;
    for (int i = 0; i < 5; ++i) firstPass.push_back(runAndCapture(host));
    require(host.serialize() != checkpoint, "emulation state did not advance");
    require(host.unserialize(checkpoint), "core rejected its own serialized state");
    for (const auto& expected : firstPass) {
        const auto actual = runAndCapture(host);
        require(actual.frame == expected.frame, "restored state produced different video");
        // This core does not include the audio resampler history in disk state.
        require(!actual.audio.isEmpty(), "restored state stopped producing audio");
    }

    error.clear();
    requireRejected(host.saveState(-1, error), error, "negative save slot was accepted");
    error.clear();
    requireRejected(host.saveState(10, error), error, "save slot above 9 was accepted");
    error.clear();
    requireRejected(host.loadState(-1, error), error, "negative load slot was accepted");
    error.clear();
    requireRejected(host.loadState(10, error), error, "load slot above 9 was accepted");

    error.clear();
    require(host.saveState(0, error), "could not save slot zero");
    const QString slotZero = stateFile(dataPath, rom, 0);
    const QByteArray savedZero = readFile(slotZero);
    require(savedZero.size() > checkpoint.size(), "slot file lacks state envelope");
    const auto expectedNext = runAndCapture(host);
    expectedAfterSlotZero = expectedNext;
    for (int i = 0; i < 3; ++i) runAndCapture(host);
    error.clear();
    require(host.loadState(0, error), "could not load slot zero");
    require(runAndCapture(host).frame == expectedNext.frame, "slot zero did not restore video");

    error.clear();
    require(host.saveState(9, error), "could not save slot nine");
    const QString slotNine = stateFile(dataPath, rom, 9);
    const QByteArray savedNine = readFile(slotNine);
    require(savedNine.size() > checkpoint.size(), "slot nine file lacks state envelope");
    const auto expectedAfterSlotNine = runAndCapture(host);
    runAndCapture(host);
    error.clear();
    require(host.loadState(9, error), "could not load slot nine");
    const auto restoredNine = runAndCapture(host);
    require(restoredNine.frame == expectedAfterSlotNine.frame,
            "slot nine did not restore its independent checkpoint");
    require(!restoredNine.audio.isEmpty(), "slot nine restore stopped audio");

    writeFile(slotZero, savedZero.left(20));
    error.clear();
    requireRejected(host.loadState(0, error), error, "truncated slot state was accepted");
    writeFile(slotZero, savedZero);
    QByteArray corrupted = savedNine;
    corrupted[corrupted.size() - 1] = char(corrupted.back() ^ 1);
    writeFile(slotNine, corrupted);
    error.clear();
    requireRejected(host.loadState(9, error), error, "corrupt slot state was accepted");

    error.clear();
    require(host.flushSaveRam(error), "could not flush battery RAM");
    QByteArray otherRom = rom;
    otherRom[0x125] = char(otherRom[0x125] ^ 1); // different game identity, same program
    const QString otherPath = temp.path() + "/other.md";
    writeFile(otherPath, otherRom);
    error.clear();
    require(host.load(otherPath, error), "variant ROM did not load");
    const QString otherSlot = stateFile(dataPath, otherRom, 0);
    writeFile(otherSlot, savedZero);
    error.clear();
    requireRejected(host.loadState(0, error), error, "state from a different ROM was accepted");
    require(host.loaded(), "state rejection unloaded the game");
    host.unload();
    require(!host.loaded() && host.frame().isNull() && host.audio().isEmpty(),
            "unload left output or loaded flag behind");
    }

    // State files survive destruction of the host and a new load of the ROM.
    LibretroHost reopened(corePath, dataPath);
    error.clear();
    require(reopened.load(romPath, error), "could not reopen generated ROM");
    error.clear();
    require(reopened.loadState(0, error), "could not load slot zero in a new host");
    require(runAndCapture(reopened).frame == expectedAfterSlotZero.frame,
            "reopened host did not replay slot zero video");
}

void testSaveRam(const QString& corePath) {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create SRAM test directory");
    const QString dataPath = temp.path() + "/data";
    const QByteArray rom = makeSaveRamRom();
    const QString romPath = temp.path() + "/battery.md";
    writeFile(romPath, rom);
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(rom, QCryptographicHash::Sha256).toHex());
    const QString savePath = dataPath + "/saves/" + key + ".srm";
    QString error;

    {
        LibretroHost host(corePath, dataPath);
        require(host.load(romPath, error), "SRAM test ROM did not load");
        for (int i = 0; i < 3; ++i) runAndCapture(host);
        error.clear();
        require(host.flushSaveRam(error), "SRAM flush failed");
        const QByteArray saved = readFile(savePath);
        require(saved.size() == 0x10000, "SRAM save is not full capacity");
        require(quint8(saved[0]) == 0x42, "SRAM test program did not write backup RAM");
    }

    // Older saves can contain only bytes up to the last changed address.
    writeFile(savePath, QByteArray(1, char(0x36)));
    {
        LibretroHost host(corePath, dataPath);
        QString warning;
        host.message = [&](const QString& text) { warning = text; };
        error.clear();
        require(host.load(romPath, error), "legacy SRAM ROM did not reload");
        require(warning.isEmpty(), "valid short SRAM file was rejected");
        error.clear();
        require(host.flushSaveRam(error), "legacy SRAM normalization failed");
        const QByteArray restored = readFile(savePath);
        require(restored.size() == 0x10000, "legacy SRAM was not padded to capacity");
        require(quint8(restored[0]) == 0x36 && quint8(restored[1]) == 0xff &&
                    quint8(restored.back()) == 0xff,
                "legacy SRAM bytes were not preserved with FF padding");
    }

    const QByteArray oversized(0x10001, char(0x55));
    writeFile(savePath, oversized);
    {
        LibretroHost host(corePath, dataPath);
        QString warning;
        host.message = [&](const QString& text) { warning = text; };
        error.clear();
        require(host.load(romPath, error), "ROM with oversized SRAM did not load");
        require(!warning.isEmpty(), "oversized SRAM did not produce warning");
        for (int i = 0; i < 3; ++i) runAndCapture(host);
        error.clear();
        require(host.flushSaveRam(error), "disabled SRAM flush should be harmless");
        require(readFile(savePath) == oversized, "oversized SRAM was overwritten");
    }
    require(readFile(savePath) == oversized, "shutdown overwrote rejected SRAM");
}

QByteArray packedRom(bool sevenZip, const QByteArray& rom, bool multiple = false) {
    QByteArray output(2*1024*1024, '\0'); size_t written=0;
    archive* writer=archive_write_new();
    require(writer, "archive writer allocation failed");
    require((sevenZip ? archive_write_set_format_7zip(writer) : archive_write_set_format_zip(writer))==ARCHIVE_OK,
            "archive writer format failed");
    require(archive_write_open_memory(writer,output.data(),size_t(output.size()),&written)==ARCHIVE_OK,
            "archive writer open failed");
    for (int i=0;i<(multiple?2:1);++i) {
        QByteArray content=rom;
        if (i) content[0x125]=char(content[0x125]^1);
        auto* entry=archive_entry_new();
        archive_entry_set_pathname(entry,i ? "folder/second.md" : "folder/generated.md");
        archive_entry_set_filetype(entry,AE_IFREG); archive_entry_set_perm(entry,0644);
        archive_entry_set_size(entry,content.size());
        require(archive_write_header(writer,entry)==ARCHIVE_OK,"archive writer header failed");
        require(archive_write_data(writer,content.constData(),size_t(content.size()))==content.size(),"archive writer data failed");
        archive_entry_free(entry);
    }
    require(archive_write_close(writer)==ARCHIVE_OK,"archive writer close failed");
    archive_write_free(writer); output.resize(qsizetype(written)); return output;
}
void testArchiveCore(const QString& corePath) {
    QTemporaryDir temp; require(temp.isValid(),"archive core temp failed");
    const auto rom=makeTestRom(); const QString plain=temp.path()+"/plain.md";
    writeFile(plain,rom);
    LibretroHost host(corePath,temp.path()+"/data"); QString error;
    require(host.load(plain,error),"plain archive reference did not load");
    for (int i=0;i<8;++i) host.runFrame();
    require(host.saveState(2,error),"archive shared save fixture failed");
    host.runFrame(); const auto expected=host.frame();
    for (bool sevenZip : {false,true}) {
        const QString packed=temp.path()+(sevenZip?"/packed.7z":"/packed.zip");
        writeFile(packed,packedRom(sevenZip,rom));
        require(host.load(packed,error),"archived ROM did not load into actual core");
        require(host.contentName()=="folder/generated.md" && host.gamePath()==packed,"archived identity incorrect");
        require(host.occupiedStateSlots()==(1u<<2),"archive and plain ROM do not share state identity");
        require(host.loadState(2,error),"archive could not restore plain ROM state");
        host.runFrame(); require(host.frame()==expected,"archive restore video differs from plain ROM");
    }
    const QString multi=temp.path()+"/multiple.zip";
    writeFile(multi,packedRom(false,rom,true));
    require(!host.load(multi,error) && !error.isEmpty() && host.loaded(),"ambiguous archive silently selected ROM or destroyed active game");
    require(host.load(multi,error,1) && host.contentName()=="folder/second.md","archive explicit member index failed");
    require(host.occupiedStateSlots()==0,"different archive members shared save identity");
    host.runFrame(); require(!host.frame().isNull(),"selected archive member did not produce video");
}

void testRewind(const QString& corePath) {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create rewind test directory");
    const auto rom = makeSaveRamRom();
    const QString romPath = temp.path() + "/rewind.md";
    writeFile(romPath, rom);
    LibretroHost host(corePath, temp.path() + "/data");
    QString error;
    require(host.load(romPath, error), "rewind fixture did not load");
    require(host.occupiedStateSlots() == 0, "empty slots reported occupied");
    const auto initial = host.captureRewindState();
    require(!initial.isEmpty(), "rewind snapshot missing");
    require(!host.restoreRewindState(initial.left(initial.size()-1)), "truncated rewind snapshot accepted");
    RewindBuffer history;
    history.configure(30,30);
    std::vector<QByteArray> states;
    std::vector<QImage> frames;
    for (int i=0;i<12;++i) {
        host.runFrame();
        states.push_back(host.captureRewindState()); frames.push_back(host.frame());
        require(history.capture(states.back(),frames.back(),host.aspectRatio()), "rewind capture failed");
    }
    require(history.size()==12 && history.bytesUsed()>0, "rewind history not populated");
    require(host.saveState(3,error) && host.occupiedStateSlots()==(1u<<3), "occupied slot mask incorrect");
    require(host.saveState(9,error) && host.occupiedStateSlots()==((1u<<3)|(1u<<9)), "two slot mask incorrect");
    for (int i=10;i>=5;--i) {
        QByteArray state; QImage frame; double aspect=0;
        require(history.stepBack(state,frame,aspect), "rewind step failed");
        require(state==states[size_t(i)] && frame==frames[size_t(i)] && aspect>0, "rewind order or frame mismatch");
        require(host.restoreRewindState(state), "core rejected rewind snapshot");
    }
    // Resuming from an older snapshot must reproduce subsequent video, not retain the discarded future.
    host.runFrame();
    require(host.frame()==frames[6] && !host.audio().isEmpty(), "rewind resume did not restore video/audio production");
    require(history.capture(host.captureRewindState(),host.frame(),host.aspectRatio()), "rewind fork capture failed");
    QByteArray state; QImage frame; double aspect=0;
    require(history.stepBack(state,frame,aspect) && state==states[5], "rewind retained the discarded future");
    require(host.flushSaveRam(error), "future SRAM flush failed");
    require(host.restoreRewindState(initial), "could not rewind to original SRAM");
    require(host.flushSaveRam(error), "rewound SRAM flush failed");
    // Initial erased SRAM must replace a future save after restoring the checkpoint.
    const QString key=QString::fromLatin1(QCryptographicHash::hash(rom,QCryptographicHash::Sha256).toHex());
    const QString ram=temp.path()+"/data/saves/"+key+".srm";
    if (QFile::exists(ram)) require(quint8(readFile(ram)[0])==0xff, "rewind retained future SRAM");
    history.configure(1,2);
    for (int i=0;i<10;++i) require(history.capture(states.back(),frames.back(),4.0/3.0), "bounded capture failed");
    require(history.size()==3, "rewind duration limit failed");
    RewindBuffer small(1);
    small.configure(30,30);
    require(!small.capture(states.back(),frames.back(),4.0/3.0) && small.bytesUsed()==0, "rewind memory cap failed");
    history.clear(); require(history.size()==0 && history.bytesUsed()==0, "history clear leaked state");
    history.configure(0,30);
    require(history.capture(states.back(),frames.back(),4.0/3.0) && history.size()==0, "disabled rewind recorded history");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "usage: core_integration_tests /path/to/genesis_plus_gx_libretro.so\n");
        return 2;
    }
    try {
        testCore(QString::fromLocal8Bit(argv[1]));
        testSaveRam(QString::fromLocal8Bit(argv[1]));
        testRewind(QString::fromLocal8Bit(argv[1]));
        testArchiveCore(QString::fromLocal8Bit(argv[1]));
        std::puts("core integration tests passed");
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "core integration test failed: %s\n", exception.what());
        return 1;
    }
}
