#include <archive.h>
#include <archive_entry.h>
#include "core/RewindBuffer.h"
#include "core/LibretroHost.h"
#include "core/VideoFrameGeometry.h"

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
    // MOVE.W #$8004, ($C00004).L: enable colour palette output.
    word(0x33fc); word(0x8004); word(0x00c0); word(0x0004);
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

// Static original diagnostic: region register or a high-frequency red/blue tile.
QByteArray makeDiagnosticRom(bool region, bool bootCounter = false) {
    auto rom = makeTestRom();
    int pc = 0x200;
    const auto word = [&](quint16 value) { rom[pc++] = char(value >> 8); rom[pc++] = char(value); };
    const auto reg = [&](quint16 value) { word(0x33fc); word(value); word(0x00c0); word(4); };
    const auto command = [&](quint32 value) {
        word(0x23fc); word(quint16(value >> 16)); word(quint16(value)); word(0x00c0); word(4);
    };
    const auto colour = [&](quint16 value) { word(0x33fc); word(value); word(0x00c0); word(0); };
    reg(0x8004); reg(0x8144); reg(0x8700);
    if (!region && !bootCounter) { reg(0x8f02); reg(0x8230); reg(0x8407); reg(0x8c81); }
    if (bootCounter) {
        word(0x3039); word(0x00ff); word(0); // MOVE.W work RAM,D0
        word(0x5440);                      // ADDQ.W #2,D0
        word(0x33c0); word(0x00ff); word(0);
        command(0xc0000000); word(0x33c0); word(0x00c0); word(0);
        word(0x60fe);
    } else if (region) {
        const int loop = pc;
        word(0x1039); word(0x00a1); word(1); // MOVE.B region register,D0
        word(0x0240); word(0x00c0);          // ANDI.W #$C0,D0
        word(0xe848);                        // LSR.W #4,D0
        command(0xc0000000); word(0x33c0); word(0x00c0); word(0);
        const int branch = pc; word(0x6000);
        rom[branch + 1] = char(loop - (branch + 2));
    } else {
        command(0xc0000000); colour(0); colour(0x000e); colour(0x0e00);
        command(0x40000000); // VRAM tile zero, alternating palette entries 1/2.
        for (int row = 0; row < 8; ++row) {
            word(0x23fc); word(0x1212); word(0x1212); word(0x00c0); word(0);
        }
        word(0x60fe);
    }
    return rom;
}

QByteArray makeChecksumRom() {
    auto rom = makeTestRom();
    int pc = 0x200;
    const auto word = [&](quint16 value) { rom[pc++] = char(value >> 8); rom[pc++] = char(value); };
    const auto reg = [&](quint16 value) { word(0x33fc); word(value); word(0x00c0); word(4); };
    word(0x7000); // MOVEQ #0,D0: running checksum.
    word(0x207c); word(0); word(0x0200); // MOVEA.L #$200,A0.
    word(0x323c); word(quint16((rom.size()-0x200)/2-1));
    const int loop = pc;
    word(0xd058); // ADD.W (A0)+,D0.
    const int dbra = pc; word(0x51c9); word(quint16(loop-(dbra+2)));
    word(0x3439); word(0); word(0x018e); // MOVE.W header checksum,D2.
    word(0xb042); // CMP.W D2,D0.
    const int equal = pc; word(0x6700);
    word(0x303c); word(0x000e); // Failure: red.
    const int finish = pc; word(0x6000);
    const int success = pc;
    word(0x303c); word(0x00e0); // Success: green.
    const int display = pc;
    reg(0x8004); reg(0x8144); reg(0x8700);
    word(0x23fc); word(0xc000); word(0); word(0x00c0); word(4);
    word(0x33c0); word(0x00c0); word(0); word(0x60fe);
    rom[equal+1] = char(success-(equal+2));
    rom[finish+1] = char(display-(finish+2));
    quint16 checksum = 0;
    for (int i = 0x200; i < rom.size(); i += 2)
        checksum = quint16(checksum + (quint16(quint8(rom[i])) << 8) + quint8(rom[i+1]));
    checksum ^= 0xffff; // Deliberately incorrect header; program verifies the entire ROM.
    rom[0x18e] = char(checksum >> 8); rom[0x18f] = char(checksum);
    return rom;
}

QByteArray swappedRom(QByteArray rom) {
    for (qsizetype i = 0; i < rom.size(); i += 2) std::swap(rom[i],rom[i+1]);
    return rom;
}
QByteArray smdRom(const QByteArray& rom) {
    QByteArray result(rom.size()+512, char(0));
    for (qsizetype offset = 0; offset < rom.size(); offset += 0x4000)
        for (qsizetype i = 0; i < 0x2000; ++i) {
            result[512+offset+i] = rom[offset+2*i+1];
            result[512+offset+0x2000+i] = rom[offset+2*i];
        }
    return result;
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
    require(firstPass.front().frame != firstPass.back().frame, "diagnostic ROM video did not advance");
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
    LibretroHost::StateLoadError missingFailure;
    const auto beforeMissing = host.serialize();
    requireRejected(host.loadState(0, error, &missingFailure), error, "missing save was accepted");
    require(missingFailure == LibretroHost::StateLoadError::Missing, "missing save not classified");
    require(host.serialize() == beforeMissing, "missing save changed emulation state");
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
    const auto entries = RomArchive::list(multi, &error);
    require(error.isEmpty() && entries.size() == 2, "archive metadata fixture list failed");
    const auto selected = entries.at(1);
    require(host.load(multi, error, selected.index, &selected) &&
            host.contentName() == selected.name, "forwarded archive selection did not load");
    for (int field = 0; field < 3; ++field) {
        auto stale = selected;
        if (field == 0) stale.name += ".changed";
        else if (field == 1) ++stale.size;
        else stale.index = 99;
        require(!host.load(multi, error, stale.index, &stale) && !error.isEmpty() &&
                host.loaded() && host.contentName() == selected.name,
                "stale forwarded archive selection replaced the active game");
    }
}

void testCoreOptionsAndReset(const QString& corePath) {
    QTemporaryDir temp; require(temp.isValid(), "options test temp failed");
    const QString path = temp.path() + "/regions.md";
    writeFile(path, makeDiagnosticRom(true));
    LibretroHost host(corePath, temp.path() + "/data"); QString error;
    require(!host.setRegion(-1, error) && !error.isEmpty(), "invalid region accepted");
    require(!host.setBlarggFilter(5, error) && !error.isEmpty(), "invalid Blargg filter accepted");
    require(host.setRegion(3, error), "preload PAL option rejected");
    require(host.load(path, error), "region ROM did not load");
    require(host.fps() > 49 && host.fps() < 51, "preload PAL option lost to core defaults");
    QImage japan, usa, europe;
    for (int region : {1, 2, 3, 0}) {
        require(host.setRegion(region, error), "live region rejected");
        for (int i = 0; i < 8; ++i) host.runFrame();
        require(host.loaded() && host.gamePath() == path && host.contentName() == "regions.md",
                "region change altered content identity");
        const double expected = region == 3 ? 50 : 60;
        require(std::abs(host.fps() - expected) < 1, "live region did not update frame timing");
        if (region == 1) japan = host.frame();
        if (region == 2) usa = host.frame();
        if (region == 3) europe = host.frame();
        if (!region) require(host.frame() == usa, "Auto did not restore ROM's USA region");
    }
    require(japan != usa && usa != europe && japan != europe,
            "region register did not distinguish Japan, USA and Europe");
    require(host.saveState(1, error), "NTSC state save failed");
    require(host.setRegion(3, error), "PAL reselect failed");
    host.runFrame();
    require(host.loadState(1, error), "state rejected after region change");
    for (int i = 0; i < 8; ++i) host.runFrame();
    require(host.fps() > 49 && host.fps() < 51 && host.frame() == europe,
            "old state overrode manually selected PAL timing or region register");

    const QString pattern = temp.path() + "/pattern.md";
    writeFile(pattern, makeDiagnosticRom(false));
    require(host.setRegion(2, error) && host.load(pattern, error), "filter ROM load failed");
    for (int i = 0; i < 8; ++i) host.runFrame();
    const QImage raw = host.frame();
    bool red = false, blue = false;
    for (int x = 0; x < raw.width(); ++x) {
        const QRgb pixel = raw.pixel(x, raw.height()/2);
        red |= qRed(pixel) > qBlue(pixel) + 64;
        blue |= qBlue(pixel) > qRed(pixel) + 64;
    }
    require(red && blue, "filter diagnostic did not produce alternating red/blue pixels");
    std::vector<QImage> filtered;
    for (int filter : {1, 2, 3, 4}) {
        require(host.setBlarggFilter(filter, error), "live Blargg option rejected");
        for (int i = 0; i < 8; ++i) host.runFrame();
        const QImage image = host.frame();
        require(image.width() > raw.width() && image.height() == raw.height(),
                "Blargg did not change actual core output geometry");
        require(image != raw, "Blargg did not change core video");
        if (filter == 1) for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, image.height()/2);
            require(std::abs(qRed(pixel)-qGreen(pixel)) <= 8 && std::abs(qBlue(pixel)-qGreen(pixel)) <= 8,
                    "Blargg monochrome output retained colour");
        }
        for (const auto& previous : filtered)
            require(image != previous, "distinct Blargg presets produced identical diagnostic video");
        filtered.push_back(image);
    }
    require(host.setBlarggFilter(0, error), "disable Blargg failed");
    for (int i = 0; i < 8; ++i) host.runFrame();
    require(host.frame() == raw, "disabling Blargg did not restore original pixels");
    require(host.setBlarggFilter(2, error), "enable Blargg before reload failed");
    require(host.hardReset(error), "hard reset with Blargg failed");
    for (int i = 0; i < 8; ++i) host.runFrame();
    require(host.frame() == filtered[1], "hard reset lost selected Blargg preset");

    const QString boots = temp.path() + "/boots.md";
    writeFile(boots, makeDiagnosticRom(false, true));
    require(host.setBlarggFilter(0, error) && host.load(boots, error), "boot counter ROM load failed");
    for (int i = 0; i < 8; ++i) host.runFrame();
    const QImage cold = host.frame();
    host.reset();
    for (int i = 0; i < 8; ++i) host.runFrame();
    require(host.frame() != cold, "soft reset cleared work RAM boot counter");
    require(QFile::remove(boots), "cannot remove loaded diagnostic ROM");
    require(host.hardReset(error), "hard reset required original ROM file");
    for (int i = 0; i < 8; ++i) host.runFrame();
    require(host.frame() == cold && host.gamePath() == boots,
            "hard reset did not clear volatile RAM or preserve content identity");

    const QString multi = temp.path() + "/battery.zip";
    const auto battery = makeSaveRamRom();
    writeFile(multi, packedRom(false, battery, true));
    require(host.load(multi, error, 1), "multi-member battery ROM did not load");
    for (int i = 0; i < 8; ++i) host.runFrame();
    require(host.flushSaveRam(error), "pre-reset battery flush failed");
    const auto before = host.captureRewindState();
    const QString selected = host.contentName();
    require(host.hardReset(error), "archive battery hard reset failed");
    const auto after = host.captureRewindState();
    require(host.contentName() == selected && host.gamePath() == multi &&
            selected == "folder/second.md", "hard reset selected wrong archive member");
    require(before.right(0x10000) == after.right(0x10000), "hard reset lost SRAM bytes");
    auto changedRom = battery; changedRom[0x125] = char(changedRom[0x125] ^ 1);
    const auto key = QCryptographicHash::hash(changedRom, QCryptographicHash::Sha256).toHex();
    const QString save = temp.path() + "/data/saves/" + QString::fromLatin1(key) + ".srm";
    const QByteArray oversized(0x10001, char(0x55));
    host.unload(); writeFile(save, oversized);
    require(host.load(multi, error, 1) && host.hardReset(error), "protected SRAM hard reset failed");
    require(readFile(save) == oversized, "hard reset overwrote rejected user SRAM");
}

void testChecksumFix(const QString& corePath) {
    QTemporaryDir temp; require(temp.isValid(), "checksum test temp failed");
    const auto rom = makeChecksumRom();
    LibretroHost host(corePath,temp.path()+"/data"); QString error;
    const std::vector<std::pair<QString,QByteArray>> cases{
        {"checksum.md",rom}, {"checksum.smd",smdRom(rom)},
        {"swapped.bin",swappedRom(rom)}, {"checksum.zip",packedRom(false,rom)}};
    for (const auto& [name,original] : cases) {
        const QString path = temp.path()+'/'+name;
        writeFile(path,original);
        require(host.setAutoFixChecksum(false,error) && host.load(path,error), "checksum fixture load failed");
        for (int i = 0; i < 8; ++i) host.runFrame();
        const auto bad = host.frame();
        const QRgb badPixel = bad.pixel(bad.width()/2,bad.height()/2);
        require(qRed(badPixel) > qGreen(badPixel)+64, "bad checksum diagnostic did not fail in red");
        require(host.saveState(4,error), "checksum original-identity state save failed");
        const auto source = name.endsWith(".zip") ? rom : original;
        const QString slot = stateFile(temp.path()+"/data",source,4);
        require(QFile::exists(slot), "state identity did not use original ROM bytes");
        const QString identity = host.contentName();
        require(host.setAutoFixChecksum(true,error) && host.hardReset(error), "checksum fix hard reset failed");
        for (int i = 0; i < 8; ++i) host.runFrame();
        const auto good = host.frame();
        const QRgb goodPixel = good.pixel(good.width()/2,good.height()/2);
        require(qGreen(goodPixel) > qRed(goodPixel)+64, "memory checksum fix did not pass cartridge self-check");
        require(host.contentName() == identity && host.gamePath() == path && host.occupiedStateSlots() == (1u<<4),
                "checksum fix changed selected content or original save identity");
        require(readFile(path) == original, "checksum fix rewrote original ROM/archive file");
        require(host.setAutoFixChecksum(false,error) && host.hardReset(error), "checksum fix disable failed");
        for (int i = 0; i < 8; ++i) host.runFrame();
        require(host.frame() == bad, "checksum fix disable did not restore unmodified in-memory content");
        require(readFile(path) == original, "checksum fix disable rewrote source file");
    }
    // A BIN without a valid MD header must not be patched merely due to its extension.
    QByteArray ambiguous = rom; ambiguous.replace(0x100,4,"TEST");
    const QString path = temp.path()+"/ambiguous.bin"; writeFile(path,ambiguous);
    require(host.setAutoFixChecksum(true,error) && host.load(path,error), "ambiguous BIN fixture load failed");
    for (int i = 0; i < 8; ++i) host.runFrame();
    const QRgb pixel = host.frame().pixel(host.frame().width()/2,host.frame().height()/2);
    require(qRed(pixel) > qGreen(pixel)+64, "ambiguous non-MD header was modified");
}

void testNativeVideoGeometry(const QString& corePath) {
    QTemporaryDir temp; require(temp.isValid(), "native video geometry temp failed");
    auto md40 = makeDiagnosticRom(false);
    auto md32 = md40;
    const int reg12 = md32.indexOf(QByteArray::fromHex("33fc8c81"));
    require(reg12 > 0,"H40 fixture register missing"); md32[reg12+3] = 0;
    QByteArray sms(32*1024,char(0));
    // Original Z80 diagnostic: configure VDP Mode4 and enable display, then idle.
    const auto program = QByteArray::fromHex("f331f0df3e04d3bf3e80d3bf3e40d3bf3e81d3bf18fe");
    sms.replace(0,program.size(),program); sms.replace(0x7ff0,8,"TMR SEGA"); sms[0x7fff] = char(0x4c);
    auto gg = sms; gg[0x7fff] = char(0x6c);
    const std::array<std::pair<QString,QByteArray>,4> cases{{
        {"wide.md",md40},{"narrow.md",md32},{"master.sms",sms},{"handheld.gg",gg}
    }};
    LibretroHost host(corePath,temp.path()+"/data"); QString error;
    RewindBuffer history; history.configure(10,30);
    for (const auto& [name,rom] : cases) {
        const QString path = temp.path()+'/'+name; writeFile(path,rom);
        require(host.setBlarggFilter(0,error) && host.load(path,error),"geometry ROM load failed");
        for (int i = 0; i < 3; ++i) host.runFrame();
        const QSize native = host.frame().size();
        const int expectedWidth = name.endsWith(".gg") ? 160 : name.startsWith("wide") ? 320 : 256;
        require(native.width() == expectedWidth,"unexpected native diagnostic raster width");
        require(host.nativeVideoSize() == native,"unfiltered native geometry incorrect");
        for (int filter : {1,2,3,4}) {
            require(host.setBlarggFilter(filter,error),"geometry Blargg toggle failed");
            for (int i = 0; i < 3; ++i) host.runFrame();
            require(host.frame().width() > native.width() && host.frame().height() == native.height(),
                    "diagnostic Blargg did not expand horizontal sample count");
            require(host.nativeVideoSize() == native && VideoFrameGeometry::nativeSize(host.frame()) == native,
                    "Blargg metadata lost true native console geometry");
            history.clear();
            require(history.capture(host.captureRewindState(),host.frame(),host.aspectRatio()),"filtered geometry capture failed");
            host.runFrame();
            require(history.capture(host.captureRewindState(),host.frame(),host.aspectRatio()),"second geometry capture failed");
            QByteArray state; QImage frame; double aspect;
            require(history.stepBack(state,frame,aspect) && VideoFrameGeometry::nativeSize(frame) == native,
                    "rewind rebuilt filtered pixels without native geometry");
        }
        require(host.setBlarggFilter(0,error),"geometry disable filter failed");
        for (int i = 0; i < 3; ++i) host.runFrame();
        require(host.frame().size() == native && host.nativeVideoSize() == native,
                "disabling filter retained expanded sample geometry");
    }
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
    const auto mode = argc == 3 ? QString::fromLocal8Bit(argv[2]) : QString();
    if (argc != 2 && !(argc == 3 && (mode == QLatin1String("--video-geometry") ||
                                     mode == QLatin1String("--archive-only")))) {
        std::fprintf(stderr, "usage: core_integration_tests /path/to/genesis_plus_gx_libretro.so [--video-geometry|--archive-only]\n");
        return 2;
    }
    try {
        if (mode == QLatin1String("--video-geometry")) {
            testNativeVideoGeometry(QString::fromLocal8Bit(argv[1]));
            std::puts("native filtered video geometry passed"); return 0;
        }
        if (mode == QLatin1String("--archive-only")) {
            testArchiveCore(QString::fromLocal8Bit(argv[1]));
            std::puts("archive core integration passed"); return 0;
        }
        testCore(QString::fromLocal8Bit(argv[1]));
        testSaveRam(QString::fromLocal8Bit(argv[1]));
        testRewind(QString::fromLocal8Bit(argv[1]));
        testArchiveCore(QString::fromLocal8Bit(argv[1]));
        testCoreOptionsAndReset(QString::fromLocal8Bit(argv[1]));
        testChecksumFix(QString::fromLocal8Bit(argv[1]));
        testNativeVideoGeometry(QString::fromLocal8Bit(argv[1]));
        std::puts("core integration tests passed");
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "core integration test failed: %s\n", exception.what());
        return 1;
    }
}
