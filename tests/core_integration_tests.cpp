#include <archive.h>
#include <zlib.h>
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
#include <QSysInfo>

#include <cmath>
#include <cstring>
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

// Original port-ID loop: TH low forces D2/D3 low on a gamepad; an empty
// controller socket floats them high. Runs on real GPX, without game ROMs.
QByteArray makePortProbeRom() {
    auto rom=makeTestRom();int pc=0x200;
    const auto word=[&](quint16 value){rom[pc++]=char(value>>8);rom[pc++]=char(value);};
    const int loop=pc;
    word(0x13fc);word(0x40);word(0xa1);word(0x000b);
    word(0x13fc);word(0);word(0xa1);word(0x0005);
    word(0x1039);word(0xa1);word(0x0005);
    word(0x0240);word(0x000c);
    word(0x33c0);word(0x00ff);word(0);
    word(quint16(0x6000|((loop-(pc+2))&255)));
    return rom;
}
void testControllerPorts(const QString& corePath) {
    QTemporaryDir temp;require(temp.isValid(),"port test directory");
    const QString path=temp.filePath("original-ports.md");
    writeFile(path,makePortProbeRom());
    LibretroHost host(corePath,temp.path());
    require(host.controllerConnected(0) && !host.controllerConnected(1),"host port defaults");
    require(host.setControllerConnected(1,true),"preload connection change");
    QString error;require(host.load(path,error),"port probe ROM load");
    QLibrary inspect(corePath);
    using Memory=void*(*)(unsigned);
    const auto memory=reinterpret_cast<Memory>(inspect.resolve("retro_get_memory_data"));
    require(memory!=nullptr,"port test memory API");
    const auto expect=[&](bool connected){
        for(int i=0;i<2;++i)host.runFrame();
        const void* ram=memory(RETRO_MEMORY_SYSTEM_RAM);require(ram!=nullptr,"port probe RAM");
        quint16 answer;std::memcpy(&answer,ram,sizeof(answer));
        require(answer==(connected?0:12),"real core reported wrong controller port electrical ID");
    };
    expect(true);
    require(!host.setControllerConnected(1,true),"unchanged connection was treated as changed");
    const auto withPad=host.serialize();require(!withPad.isEmpty(),"port state capture");
    host.setControllerConnected(1,false);expect(false);
    require(host.unserialize(withPad),"port state restore");expect(false);
    require(!host.controllerConnected(1),"saved state changed requested port topology");
    require(host.hardReset(error),"empty port hard reset");expect(false);
    host.setControllerConnected(1,true);expect(true);
    const auto rewind=host.captureRewindState();require(!rewind.isEmpty(),"port rewind capture");
    host.setControllerConnected(1,false);
    require(host.restoreRewindState(rewind),"port rewind restore");expect(false);
    host.setControllerConnected(1,true);
    require(host.hardReset(error),"connected port hard reset");expect(true);
    host.unload();require(host.load(path,error),"port reload");expect(true);
    host.setControllerConnected(0,false);require(!host.controllerConnected(0),"player one disconnect");
}

// Original 68000 I/O handshake for both MD gun protocols; no game ROM needed.
void testLightguns(const QString& corePath) {
    QTemporaryDir temp; require(temp.isValid(),"gun probe directory");
    auto rom=makeTestRom(); int pc=0x200;
    const auto word=[&](quint16 v){rom[pc++]=char(v>>8);rom[pc++]=char(v);};
    const auto write=[&](quint16 v,quint16 address){word(0x13fc);word(v);word(0xa1);word(address);};
    const auto sample=[&](quint16 offset){word(0x1039);word(0xa1);word(5);word(0x0240);word(0x3f);word(0x33c0);word(0xff);word(offset);};
    const int loop=pc;
    write(0x40,0xb);write(0x40,5);sample(0); // Justifier detection phase
    write(0x60,0xb);write(0,5);sample(2);    // first gun buttons
    write(0x20,5);sample(4);                // second Justifier buttons
    word(quint16(0x6000|((loop-(pc+2))&255)));
    const QString path=temp.filePath("original-guns.md");writeFile(path,rom);
    LibretroHost host(corePath,temp.path());
    QLibrary inspect(corePath);
    using Memory=void*(*)(unsigned);
    const auto memory=reinterpret_cast<Memory>(inspect.resolve("retro_get_memory_data"));
    require(memory!=nullptr,"gun probe memory API");
    const auto read=[&](int offset){quint16 value=0;std::memcpy(&value,static_cast<const char*>(memory(RETRO_MEMORY_SYSTEM_RAM))+offset,2);return value;};
    const auto step=[&]{for(int i=0;i<2;++i)host.runFrame(false,false);};
    QString error;
    for (auto kind : {ControllerType::Menacer,ControllerType::Justifier}) {
        require(!host.setControllerKind(0,kind),"MD gun accepted on port A");
        for (bool withPad : {false,true}) {
            constexpr unsigned owner = 1;
            host.setControllerKind(owner,kind);
            host.setControllerConnected(owner,true);
            host.setControllerConnected(0,withPad);
            host.setControllerConnected(2,false);
            require(host.load(path,error),"gun probe load");
            host.setButtons(0,(1u<<1)|(1u<<3));
            host.setButtons(owner,0);step();
            if(kind==ControllerType::Justifier) require(read(0)==0x30 && (read(2)&3)==3,"Justifier detection/idle");
            else require((read(2)&15)==0,"Menacer idle");
            host.setButtons(owner,1u<<1);step();
            if(kind==ControllerType::Justifier) require((read(2)&3)==2,"Justifier trigger route");
            else require((read(2)&15)==2,"Menacer trigger route");
            host.setButtons(owner,1u<<3);step();
            if(kind==ControllerType::Justifier) require((read(2)&3)==1,"Justifier Start route");
            else require((read(2)&15)==8,"Menacer Start route");
            if(kind==ControllerType::Justifier) {
                host.setControllerKind(2,kind);host.setControllerConnected(2,true);
                host.setButtons(1,1u<<1);host.setButtons(2,1u<<3);step();
                require((read(2)&3)==2 && (read(4)&3)==1,"dual Justifier independent input");
                host.setControllerConnected(2,false);step();
                require((read(4)&3)==3,"disconnected second Justifier still pressed");
            }
            host.unload();
        }
    }
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

QString stateFile(const QString& dataPath, const QString& name, int slot) {
    return dataPath + "/states/" + QFileInfo(name).completeBaseName() + QStringLiteral(".slot%1.krs").arg(slot);
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
    const QString slotZero = stateFile(dataPath, romPath, 0);
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
    const QString slotNine = stateFile(dataPath, romPath, 9);
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
    const QString otherSlot = stateFile(dataPath, otherPath, 0);
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
    const QString savePath = dataPath + "/saves/battery.srm";
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
    const auto rom=makeTestRom(); const QString plain=temp.path()+"/generated.md";
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
    const QString save = temp.path() + "/data/saves/second.srm";
    const QByteArray oversized(0x10001, char(0x55));
    host.unload(); writeFile(save, oversized);
    require(host.load(multi, error, 1) && host.hardReset(error), "protected SRAM hard reset failed");
    require(readFile(save) == oversized, "hard reset overwrote rejected user SRAM");
}

void setPrefixCrc(QByteArray& rom, quint32 target) {
    const auto crc=[&](){return quint32(crc32(0,reinterpret_cast<const Bytef*>(rom.constData()),0x10000));};
    constexpr int fix=0xfffc;
    for(int i=0;i<4;++i)rom[fix+i]=0;
    const quint32 base=crc();quint32 basis[32]{}, combinations[32]{};
    for(unsigned bit=0;bit<32;++bit){
        rom[fix+int(bit/8)]=char(1u<<(bit%8));
        quint32 value=crc()^base, combination=quint32(1)<<bit;
        rom[fix+int(bit/8)]=0;
        for(int pivot=31;pivot>=0;--pivot)if(value&(quint32(1)<<pivot)){
            if(basis[pivot]){value^=basis[pivot];combination^=combinations[pivot];}
            else{basis[pivot]=value;combinations[pivot]=combination;break;}
        }
    }
    quint32 delta=target^base, solution=0;
    for(int pivot=31;pivot>=0;--pivot)if(delta&(quint32(1)<<pivot)){
        require(basis[pivot]!=0,"CRC fixup matrix is singular");
        delta^=basis[pivot];solution^=combinations[pivot];
    }
    for(int i=0;i<4;++i)rom[fix+i]=char(solution>>(8*i));
    require(crc()==target,"generated compatibility fingerprint is wrong");
}

// Original diagnostic ROM; a four-byte CRC fixup selects the same compatibility
// rule as an identified cartridge without embedding any game code or data.
QByteArray makeVmvBusRom(bool identified) {
    auto rom = makeTestRom(); rom.append(QByteArray(0x200000-rom.size(), char(0)));
    putLong(rom,0x1a4,quint32(rom.size()-1));
    rom[0x18e]=rom[0x18f]=0;
    int pc=0x200;
    const auto word=[&](quint16 v){rom[pc++]=char(v>>8);rom[pc++]=char(v);};
    const auto writeWord=[&](quint16 value,quint32 address){
        word(0x33fc);word(value);word(quint16(address>>16));word(quint16(address));};
    const auto writeByte=[&](quint16 value,quint32 address){
        word(0x13fc);word(value);word(quint16(address>>16));word(quint16(address));};
    writeWord(0x100,0xa11100); // Request Z80 bus, then release reset with bus held.
    writeWord(0x100,0xa11200);
    writeByte(0x18,0xa00000); writeByte(0xfe,0xa00001); // Z80: JR $.
    writeWord(0,0xa11100); // Z80 runs; a normal cartridge cannot access its bus.
    const int loop=pc;
    writeByte(0x5a,0xa01000);
    word(0x1039);word(0xa0);word(0x1000); // MOVE.B ZRAM,D0.
    word(0x13c0);word(0xff);word(0); // Store observed bus byte in work RAM.
    word(quint16(0x6000|((loop-(pc+2))&0xff)));
    setPrefixCrc(rom, 0xe302585a);
    if(!identified)rom[0xfff0]=char(quint8(rom[0xfff0])^1); // Same program, ordinary bus policy.
    return rom;
}

void testVmvBusCompatibility(const QString& corePath) {
    QTemporaryDir temp;require(temp.isValid(),"VMV temp failed");
    LibretroHost host(corePath,temp.path()+"/data");QString error;
    QLibrary inspect(corePath);require(inspect.load(),"VMV core inspection failed");
    using Memory=void* (*)(unsigned);
    const auto memory=reinterpret_cast<Memory>(inspect.resolve("retro_get_memory_data"));
    require(memory!=nullptr,"VMV memory API missing");
    for(bool identified:{true,false,true}){
        const auto rom=makeVmvBusRom(identified);
        const QString path=temp.path()+"/bus.md";writeFile(path,rom);
        require(host.setAutoFixChecksum(true,error) && host.load(path,error),"VMV fixture load failed");
        const auto check=[&](){
            for(int i=0;i<3;++i)host.runFrame();
            const auto* ram=static_cast<const unsigned char*>(memory(RETRO_MEMORY_SYSTEM_RAM));
            require(ram!=nullptr,"VMV RAM missing");
            const unsigned byte=ram[QSysInfo::ByteOrder==QSysInfo::LittleEndian?1:0];
            require((byte==0x5a)==identified,"Z80 bus compatibility leaked or disappeared");
        };
        check();
        const auto state=host.serialize();require(!state.isEmpty(),"VMV state missing");
        host.runFrame();require(host.unserialize(state),"VMV state restore failed");check();
        host.reset();check();require(host.hardReset(error),"VMV power reset failed");check();
        require(readFile(path)==rom,"VMV fixture source was modified");
    }
}

// Generated four-copy cartridge exercises the same detection gates and existing
// protection handlers as Super Poker, without embedding game bytes.
QByteArray makePokerRom(int variant) {
    auto rom = makeTestRom(); rom.append(QByteArray(0x100000-rom.size(), char(0)));
    putLong(rom,0x1a4,0x3fffff);
    rom[0x18e]=rom[0x18f]=char(0xff);
    int pc=0x200;
    const auto word=[&](quint16 v){rom[pc++]=char(v>>8);rom[pc++]=char(v);};
    for(int i=0;i<4;++i){
        word(0x1039);word(0x40);word(quint16(i*2)); // MOVE.B protection,D0
        word(0x13c0);word(0xff);word(quint16(i)); // MOVE.B D0,work RAM
    }
    word(0x60fe);
    const auto sum=[&](int begin,int end){
        quint32 result=0;
        for(int i=begin;i<end;i+=2)result+=(quint8(rom[i])<<8)|quint8(rom[i+1]);
        return quint16(result);
    };
    const auto adjust=[&](int offset,quint16 delta){
        const quint16 value=quint16((quint8(rom[offset])<<8)|quint8(rom[offset+1]))+delta;
        rom[offset]=char(value>>8);rom[offset+1]=char(value);
    };
    // Full repeated checksum is 4 * body + 3 * header (mod 65536).
    constexpr quint16 headerSum=quint16((0x6f41u-4u*0xd7b0u)*0xaaabu);
    adjust(0x1f0,quint16(headerSum-sum(0,0x200)));
    setPrefixCrc(rom,0xdd02797c);
    adjust(0xffffe,quint16(0xd7b0-sum(0x200,rom.size())));
    if(variant==2){ // Same sum and repeats, wrong prefix fingerprint.
        adjust(0x8000,1);adjust(0x8002,quint16(-1));
    }
    const auto quarter=rom;rom+=quarter;rom+=quarter;rom+=quarter;
    if(variant==1){ // Same checksum/prefix, but not identical repeated banks.
        adjust(0x180000,1);adjust(0x180002,quint16(-1));
    }
    require(sum(0x200,rom.size())==0x6f41,"Poker diagnostic checksum mismatch");
    return rom;
}

void testPokerOverdump(const QString& corePath) {
    QTemporaryDir temp;require(temp.isValid(),"Poker temp failed");
    LibretroHost host(corePath,temp.path()+"/data");QString error;
    QLibrary inspect(corePath);require(inspect.load(),"Poker core inspection failed");
    using Memory=void* (*)(unsigned);
    using Preserve=int (*)(const unsigned char*,size_t,unsigned);
    const auto memory=reinterpret_cast<Memory>(inspect.resolve("retro_get_memory_data"));
    const auto preserve=reinterpret_cast<Preserve>(inspect.resolve("retro_kegarefusion_preserve_header_checksum"));
    require(memory && preserve,"Poker core APIs missing");
    for(int variant:{0,1,2}){
        const auto rom=makePokerRom(variant);
        require(bool(preserve(reinterpret_cast<const unsigned char*>(rom.constData()),size_t(rom.size()),0x6f41))==(variant==0),"Poker checksum guard scope mismatch");
        const QString path=temp.path()+"/poker.md";writeFile(path,rom);
        for(bool autofix:{false,true}){
            require(host.setAutoFixChecksum(autofix,error) && host.load(path,error),"Poker load failed");
            const auto check=[&](){
                for(int i=0;i<2;++i)host.runFrame();
                const auto* ram=static_cast<const unsigned char*>(memory(RETRO_MEMORY_SYSTEM_RAM));
                require(ram,"Poker RAM missing");
                const unsigned expected[4]={0x55,0x0f,0xaa,0xf0};bool match=true;
                for(unsigned i=0;i<4;++i)match &= ram[i^(QSysInfo::ByteOrder==QSysInfo::LittleEndian?1:0)]==expected[i];
                require(match==(variant==0),"Poker original protection missing or leaked");
            };
            check();
            if(variant==0){
                const auto state=host.serialize();require(!state.isEmpty() && host.unserialize(state),"Poker state restore failed");check();
                host.reset();check();require(host.hardReset(error),"Poker power reset failed");check();
            }
            host.unload();
        }
        require(readFile(path)==rom,"Poker ROM modified");
    }
}

void testJiujiProtection(const QString& corePath) {
    QTemporaryDir temp;require(temp.isValid(),"Jiuji temp failed");
    LibretroHost host(corePath,temp.path()+"/data");QString error;
    QLibrary inspect(corePath);require(inspect.load(),"Jiuji core inspection failed");
    using Memory=void* (*)(unsigned);
    const auto memory=reinterpret_cast<Memory>(inspect.resolve("retro_get_memory_data"));
    require(memory,"Jiuji memory API missing");
    for(bool original:{true,false}){
        auto rom=makeTestRom();rom.append(QByteArray(0x100000-rom.size(),char(0)));
        putLong(rom,0x1a4,quint32(rom.size()-1));
        rom[0x18e]=0x0c;rom[0x18f]=0x44;
        const auto program=QByteArray::fromHex("30390040000633c000ff000060fe");
        std::memcpy(rom.data()+0x200,program.constData(),size_t(program.size()));
        setPrefixCrc(rom,original?0x12f4d88d:0xd27b6ce1);
        quint32 sum=0;
        for(int i=0x200;i<rom.size();i+=2)sum+=(quint8(rom[i])<<8)|quint8(rom[i+1]);
        const quint16 correction=quint16(0xba81-sum);
        rom[0xffffe]=char(correction>>8);rom[0xfffff]=char(correction);
        const QString path=temp.path()+"/jiuji.md";writeFile(path,rom);
        require(host.setAutoFixChecksum(true,error) && host.load(path,error),"Jiuji load failed");
        for(int i=0;i<2;++i)host.runFrame();
        const auto* ram=static_cast<const unsigned char*>(memory(RETRO_MEMORY_SYSTEM_RAM));
        require(ram,"Jiuji RAM missing");
        const int hi=QSysInfo::ByteOrder==QSysInfo::LittleEndian?1:0;
        require(ram[hi]==(original?0xf0:0) && ram[hi^1]==0,"Jiuji protection revisions confused");
        host.unload();
    }
}

// Original 68000 mailbox: execute batched serial line writes/reads requested by
// the test through work RAM, exercising the real CPU memory map.
QByteArray makeAqRom() {
    auto rom=makeTestRom();rom.append(QByteArray(0x100000-rom.size(),char(0)));
    putLong(rom,0x1a4,quint32(rom.size()-1));rom[0x18e]=char(0x81);rom[0x18f]=4;
    int pc=0x200;
    const auto word=[&](quint16 v){rom[pc++]=char(v>>8);rom[pc++]=char(v);};
    const auto branch=[&](quint16 op){word(op);const int at=pc;word(0);return at;};
    const auto target=[&](int at,int to){const quint16 d=quint16(to-at);rom[at]=char(d>>8);rom[at+1]=char(d);};
    word(0x46fc);word(0x2700);
    word(0x33fc);word(1);word(0xa1);word(0x30f0); // Reject word SRAM enable.
    word(0x13fc);word(1);word(0xa1);word(0x3060); // Reject its generic alias.
    word(0x13f9);word(0x20);word(0);word(0xff);word(0x80);
    const int idle=pc;
    word(0x4a79);word(0xff);word(0);const int wait=branch(0x6700);target(wait,idle);
    word(0x3239);word(0xff);word(0); // MOVE.W count,D1
    word(0x4279);word(0xff);word(0); // CLR.W count
    word(0x41f9);word(0xff);word(0x100);
    word(0x43f9);word(0xff);word(0x1000);
    const int next=pc;
    word(0x1018);word(0x4a00);const int read=branch(0x6700);
    word(0x13d8);word(0x40);word(1);const int advance=branch(0x6000);
    target(read,pc);word(0x5288);word(0x12f9);word(0x40);word(1);
    target(advance,pc);word(0x5341);const int more=branch(0x6600);target(more,next);
    const int again=branch(0x6000);target(again,idle);
    setPrefixCrc(rom,0x34e04945);
    quint32 sum=0;for(int i=0x200;i<rom.size();i+=2)sum+=(quint8(rom[i])<<8)|quint8(rom[i+1]);
    const quint16 fix=quint16(0x0517-sum);rom[0xffffe]=char(fix>>8);rom[0xfffff]=char(fix);
    return rom;
}

void testAqMicrowire(const QString& corePath) {
    QTemporaryDir temp;require(temp.isValid(),"Aq temp failed");
    LibretroHost host(corePath,temp.path()+"/data");QString error;
    QLibrary inspect(corePath);require(inspect.load(),"Aq core inspection failed");
    using Memory=void* (*)(unsigned);
    const auto memory=reinterpret_cast<Memory>(inspect.resolve("retro_get_memory_data"));
    require(memory,"Aq memory API missing");
    const auto rom=makeAqRom();const QString path=temp.path()+"/aq.md";writeFile(path,rom);
    require(host.setAutoFixChecksum(true,error) && host.load(path,error),"Aq fixture load failed");
    const auto aqIdentity=host.movieCoreIdentity();host.runFrame();
    constexpr int swap=QSysInfo::ByteOrder==QSysInfo::LittleEndian?1:0;
    auto* ram=static_cast<unsigned char*>(memory(RETRO_MEMORY_SYSTEM_RAM));
    auto* battery=static_cast<unsigned char*>(memory(RETRO_MEMORY_SAVE_RAM));
    require(ram && battery && ram[0x80^swap]==0,"Aq incorrectly mapped parallel SRAM");
    QByteArray script;
    const auto line=[&](unsigned value){script.append(char(1));script.append(char(value));};
    const auto sample=[&](){script.append(char(0));script.append(char(0));};
    const auto bits=[&](unsigned value,int count){for(int b=count-1;b>=0;--b){const unsigned v=4|(((value>>b)&1)<<3);line(v);line(v|2);}};
    const auto command=[&](unsigned value){line(0);line(4);bits(1,1);bits(value,8);};
    const auto run=[&](){
        require(script.size()/2<0x10000 && script.size()<0xe00,"Aq script too large");
        for(int i=0;i<script.size();++i)ram[(0x100+i)^swap]=quint8(script[i]);
        const unsigned count=unsigned(script.size()/2);ram[0^swap]=count>>8;ram[1^swap]=count;
        script.clear();host.runFrame();
        require(!ram[0^swap] && !ram[1^swap],"Aq mailbox not consumed");
    };
    const auto enable=[&](){command(0x30);line(0);run();};
    const auto write=[&](unsigned address,unsigned value){command(0x40|address);bits(value,16);line(0);run();};
    const auto read=[&](unsigned address){
        command(0x80|address);sample();
        for(int i=0;i<16;++i){line(4);line(6);sample();}
        line(0);run();require(ram[0x1000^swap]==0,"Aq READ dummy bit missing");
        unsigned value=0;for(int i=1;i<=16;++i)value=(value<<1)|(ram[(0x1000+i)^swap]&1);return value;
    };
    require(read(0)==0xffff,"Aq initial battery not erased");
    write(0,0);require(read(0)==0xffff,"Aq WRITE ignored disabled latch");
    enable();write(0,0xa55a);write(63,0x1234);
    require(read(0)==0xa55a && read(63)==0x1234,"Aq serial write/read mismatch");
    // Sequential word63 wraps to word0, without a second command.
    command(0xbf);for(int i=0;i<32;++i){line(4);line(6);sample();}line(0);run();
    quint32 pair=0;for(int i=0;i<32;++i)pair=(pair<<1)|(ram[(0x1000+i)^swap]&1);
    require(pair==0x1234a55a,"Aq sequential wrap failed");
    // Restore in the middle of a WRITE and then in the middle of a READ.
    command(0x41);bits(0xabcd>>9,7);run();const auto partial=host.serialize();
    bits(0xabcd&0x1ff,9);line(0);run();require(read(1)==0xabcd,"Aq first partial write failed");
    require(host.unserialize(partial),"Aq partial WRITE restore failed");
    bits(0xabcd&0x1ff,9);line(0);run();require(read(1)==0xabcd,"Aq restored write shifted");
    command(0x81);for(int i=0;i<5;++i){line(4);line(6);}run();const auto reading=host.serialize();
    const auto tail=[&](){for(int i=0;i<11;++i){line(4);line(6);sample();}line(0);run();unsigned v=0;for(int i=0;i<11;++i)v=(v<<1)|(ram[(0x1000+i)^swap]&1);return v;};
    require(tail()==(0xabcd&0x7ff),"Aq READ tail failed");
    require(host.unserialize(reading) && tail()==(0xabcd&0x7ff),"Aq partial READ restore shifted");
    const auto before=host.serialize();auto invalid=before;invalid[invalid.size()-160+8]=7;
    require(!host.unserialize(invalid) && host.serialize()==before,"Aq invalid trailer changed machine");
    auto legacy=before;std::memset(legacy.data()+legacy.size()-160,0,160);
    require(host.unserialize(legacy) && read(1)==0xabcd,"Aq legacy state lost battery");
    write(1,0);require(read(1)==0xabcd,"Aq legacy state kept stale write-enable");
    enable();host.reset();host.runFrame();write(2,0x2468);require(read(2)==0x2468,"Aq soft reset changed write latch");
    require(host.flushSaveRam(error) && host.hardReset(error),"Aq battery power reset failed");
    ram=static_cast<unsigned char*>(memory(RETRO_MEMORY_SYSTEM_RAM));host.runFrame();
    require(read(0)==0xa55a && read(2)==0x2468,"Aq power reset lost saved data");
    write(2,0);require(read(2)==0x2468,"Aq power reset kept write enable");
    enable();command(0x00);line(0);run();write(0,0);require(read(0)==0xa55a,"Aq EWDS ignored");
    require(readFile(path)==rom,"Aq source ROM changed");
    const QString other=temp.path()+"/ordinary.md";writeFile(other,makeTestRom());
    require(host.load(other,error) && host.movieCoreIdentity()!=aqIdentity,"Aq identity leaked to next game");
    const auto ordinary=host.serialize();require(ordinary.mid(ordinary.size()-160,8)!="KRAQ93C!","Aq state leaked to next game");
}

// Generated, original diagnostic for the identified Sonic Jam 6 bank board.
// Uses the existing makeTestRom(), setPrefixCrc(), putLong() and file helpers.
// No game instructions or data are included.
QByteArray makeSonicMapperRom() {
    auto rom=makeTestRom();rom.append(QByteArray(0x200000-rom.size(),char(0)));
    putLong(rom,0,0x00fff000);putLong(rom,4,0x001f0000);
    putLong(rom,0x1a4,quint32(rom.size()-1));rom[0x18e]=rom[0x18f]=0;
    // Address-dependent words distinguish an entire 8 KiB window from both
    // neighbors and its physical source, including offsets beyond the helper.
    for(int at=0x200;at<rom.size();at+=2) {
        const quint32 x=quint32(at);
        const quint16 value=quint16((x>>1)^(x>>11)^((x>>17)*0x9e37u)^0xa55au);
        rom[at]=char(value>>8);rom[at+1]=char(value);
    }
    const auto helper=[&](quint32 at) {
        // MOVE.L #distinct physical address marker,D0; RTS.
        rom[int(at)]=char(0x20);rom[int(at)+1]=char(0x3c);
        putLong(rom,int(at)+2,0x51000000u|at);
        rom[int(at)+6]=char(0x4e);rom[int(at)+7]=char(0x75);
    };
    for(const quint32 at:{0x002000u,0x008000u,0x010000u,0x020000u,
                          0x040000u,0x096000u,0x0ca000u,0x0f0000u,
                          0x1a2000u,0x1aa000u,0x1c4000u,0x1c6000u}) helper(at);
    // Execute the mailbox from a page outside every observed bank window.
    // Records are [operation:u16,address:u32,value:u16], all in 68000 order.
    // Operations: 1=WRITE.W, 2=READ.W, 3=READ.B, 4=JSR (helper returns D0).
    int pc=0x1f0000;
    const auto word=[&](quint16 v){rom[pc++]=char(v>>8);rom[pc++]=char(v);};
    const auto branch=[&](quint16 op){word(op);const int at=pc;word(0);return at;};
    const auto target=[&](int at,int to){const quint16 d=quint16(to-at);rom[at]=char(d>>8);rom[at+1]=char(d);};
    word(0x46fc);word(0x2700); // Mask interrupts; no VDP setup is required.
    const int idle=pc;
    word(0x4a79);word(0xff);word(0);const int wait=branch(0x6700);target(wait,idle);
    word(0x3e39);word(0xff);word(0); // MOVE.W count,D7.
    word(0x4279);word(0xff);word(0); // Consume pending count.
    word(0x41f9);word(0xff);word(0x100);
    word(0x43f9);word(0xff);word(0x1000);
    const int next=pc;
    word(0x3218);word(0x2458);word(0x3418); // Read op,D1; address,A2; value,D2.
    word(0x0c41);word(1);const int toWrite=branch(0x6700);
    word(0x0c41);word(2);const int toReadWord=branch(0x6700);
    word(0x0c41);word(3);const int toReadByte=branch(0x6700);
    word(0x0c41);word(4);const int toCall=branch(0x6700);
    word(0x70ff);const int bad=branch(0x6000);
    target(toWrite,pc);word(0x3482);word(0x7000);const int wrote=branch(0x6000);
    target(toReadWord,pc);word(0x7000);word(0x3012);const int readWord=branch(0x6000);
    target(toReadByte,pc);word(0x7000);word(0x1012);const int readByte=branch(0x6000);
    target(toCall,pc);word(0x7000);word(0x4e92);
    const int record=pc;
    for(const int at:{bad,wrote,readWord,readByte})target(at,record);
    word(0x22c0);word(0x5347);const int more=branch(0x6600);target(more,next);
    word(0x33fc);word(0xc0de);word(0xff);word(2); // Completion, after all results.
    const int again=branch(0x6000);target(again,idle);
    setPrefixCrc(rom,0x8122ac8a);
    rom[0x1ffffe]=rom[0x1fffff]=0;
    quint32 sum=0;for(int i=0x200;i<rom.size();i+=2)sum+=(quint8(rom[i])<<8)|quint8(rom[i+1]);
    const quint16 fix=quint16(0xf456-sum);
    rom[0x1ffffe]=char(fix>>8);rom[0x1fffff]=char(fix);
    return rom;
}

void testSonicMapper(const QString& corePath) {
    QTemporaryDir temp;require(temp.isValid(),"Sonic mapper temp failed");
    LibretroHost host(corePath,temp.path()+"/data");QString error;
    QLibrary inspect(corePath);require(inspect.load(),"Sonic mapper core inspection failed");
    using Memory=void* (*)(unsigned);
    using Preserve=int (*)(const unsigned char*,size_t,unsigned);
    const auto memory=reinterpret_cast<Memory>(inspect.resolve("retro_get_memory_data"));
    const auto preserve=reinterpret_cast<Preserve>(inspect.resolve("retro_kegarefusion_preserve_header_checksum"));
    require(memory && preserve,"Sonic mapper inspection API missing");
    const QString ordinaryPath=temp.path()+"/ordinary.md";
    writeFile(ordinaryPath,makeTestRom());
    require(host.setAutoFixChecksum(true,error) && host.load(ordinaryPath,error),"Sonic ordinary baseline load failed");
    const auto ordinaryIdentity=host.movieCoreIdentity();
    const auto ordinaryNetwork=host.networkCompatibility();
    const auto rom=makeSonicMapperRom();
    const auto sum=[](const QByteArray& bytes){quint32 v=0;for(int i=0x200;i<bytes.size();i+=2)v+=(quint8(bytes[i])<<8)|quint8(bytes[i+1]);return quint16(v);};
    require(sum(rom)==0xf456 && quint32(crc32(0,reinterpret_cast<const Bytef*>(rom.constData()),0x10000))==0x8122ac8a,"Sonic generated fingerprint mismatch");
    require(preserve(reinterpret_cast<const unsigned char*>(rom.constData()),size_t(rom.size()),sum(rom)),"Sonic auto-fix did not preserve mapper key");
    const QString path=temp.path()+"/sonic-mapper.md";writeFile(path,rom);
    require(host.load(path,error),"Sonic mapper fixture load failed");
    const auto sonicIdentity=host.movieCoreIdentity();
    require(sonicIdentity!=ordinaryIdentity,"Sonic mapper recording contract not distinguished");
    if(!ordinaryNetwork.isEmpty())require(host.networkCompatibility()!=ordinaryNetwork,"Sonic mapper network contract not distinguished");
    host.runFrame();
    constexpr int swap=QSysInfo::ByteOrder==QSysInfo::LittleEndian?1:0;
    unsigned char* ram=nullptr;
    const auto refreshRam=[&](){ram=static_cast<unsigned char*>(memory(RETRO_MEMORY_SYSTEM_RAM));require(ram,"Sonic work RAM missing");};
    refreshRam();
    const auto ramWord=[&](int at,unsigned value){ram[at^swap]=quint8(value>>8);ram[(at+1)^swap]=quint8(value);};
    const auto romWord=[&](quint32 at){return (quint32(quint8(rom[int(at)]))<<8)|quint8(rom[int(at)+1]);};
    struct Check{quint16 operation;quint32 address,expected;};
    QByteArray script;std::vector<Check> checks;
    const auto queue=[&](quint16 operation,quint32 address,quint16 value,quint32 expected){
        const int at=int(script.size());script.resize(at+8);
        script[at]=char(operation>>8);script[at+1]=char(operation);
        putLong(script,at+2,address);script[at+6]=char(value>>8);script[at+7]=char(value);
        checks.push_back({operation,address,expected});
    };
    const auto write=[&](quint32 address,quint16 value){queue(1,address,value,0);};
    const auto read=[&](quint32 address,quint32 physical){
        queue(2,address,0,romWord(physical));
        queue(3,address,0,quint8(rom[int(physical)]));
        queue(3,address+1,0,quint8(rom[int(physical)+1]));
    };
    const auto call=[&](quint32 address,quint32 physical){queue(4,address,0,0x51000000u|physical);};
    const auto window=[&](quint32 address,quint32 physical,quint32 size){
        for(const quint32 offset:{0u,2u,4u,size-2})read(address+offset,physical+offset);
        read(address-2,address-2);read(address+size,address+size);
    };
    const auto page1a=[&](unsigned mode){
        // Only the selected 8 KiB code window moves. All other bytes in
        // page1A, including the other mode's helper, retain their ROM address.
        window(0x1a2000,mode==1?0x1c4000:0x1a2000,0x2000);
        window(0x1aa000,mode==2?0x1c6000:0x1aa000,0x2000);
        call(0x1a2000,mode==1?0x1c4000:0x1a2000);
        call(0x1aa000,mode==2?0x1c6000:0x1aa000);
        read(0x1a0000,0x1a0000);read(0x1a6000,0x1a6000);
        read(0x1afffe,0x1afffe);read(0x1b0000,0x1b0000);
    };
    const auto all=[&](unsigned mask,unsigned mode){
        const std::pair<quint32,quint32> windows[]={{0x008000,0x002000},
                {0x010000,0x020000},{0x040000,0x020000},
                {0x096000,0x0f0000},{0x0ca000,0x0f0000}};
        unsigned index=0;
        for(const auto [address,physical]:windows){
            const bool mapped=bool(mask&(1u<<index++));
            window(address,mapped?physical:address,0x2000);
            call(address,mapped?physical:address);
        }
        page1a(mode);
    };
    const auto run=[&](){
        require(!script.isEmpty() && script.size()%8==0 && script.size()<0xe00,"Sonic mailbox script size invalid");
        for(int i=0;i<script.size();++i)ram[(0x100+i)^swap]=quint8(script[i]);
        ramWord(2,0);ramWord(0,unsigned(checks.size()));host.runFrame();
        require(ram[0^swap]==0 && ram[1^swap]==0 && ram[2^swap]==0xc0 && ram[3^swap]==0xde,"Sonic mailbox did not finish");
        for(size_t i=0;i<checks.size();++i){
            quint32 observed=0;for(int b=0;b<4;++b)observed=(observed<<8)|ram[(0x1000+int(i)*4+b)^swap];
            const auto& check=checks[i];
            const auto detail=QStringLiteral("Sonic op%1 at%2 expected%3 got%4").arg(check.operation).arg(check.address,6,16,QLatin1Char('0')).arg(check.expected,8,16,QLatin1Char('0')).arg(observed,8,16,QLatin1Char('0')).toLocal8Bit();
            require(observed==check.expected,detail.constData());
        }
        script.clear();checks.clear();
    };
    all(0,0);run();
    write(0x408000,0x0100);window(0x008000,0x002000,0x2000);call(0x008000,0x002000);run();
    const auto partial=host.serialize();require(!partial.isEmpty(),"Sonic partial mapper state missing");
    write(0x4080f0,0x0100);page1a(1);call(0x008000,0x002000);run();
    write(0x408600,0x0100);window(0x010000,0x020000,0x2000);call(0x010000,0x020000);page1a(2);run();
    write(0x4088f0,0x0300);window(0x040000,0x020000,0x2000);call(0x040000,0x020000);run();
    write(0x409f00,0x0200);window(0x096000,0x0f0000,0x2000);call(0x096000,0x0f0000);run();
    write(0x40a000,0x0200);all(0x1f,2);run();
    const auto alternate=host.serialize();require(!alternate.isEmpty(),"Sonic alternate code-bank state missing");
    write(0x40a000,0x0100);all(0x1f,1);run();
    const auto active=host.serialize();require(!active.isEmpty(),"Sonic active mapper state missing");
    // A partial-mask restore must remove banks active only in the future.
    require(host.unserialize(partial),"Sonic partial mapper state restore failed");refreshRam();all(1,0);run();
    require(host.unserialize(alternate),"Sonic alternate code-bank restore failed");refreshRam();all(0x1f,2);run();
    require(host.unserialize(active),"Sonic active mapper state restore failed");refreshRam();all(0x1f,1);run();
    // Both observed reset commands undo every active data and code window.
    write(0x408100,0);all(0,0);run();
    require(host.unserialize(active),"Sonic post-reset active restore failed");refreshRam();all(0x1f,1);run();
    write(0x40a100,0);all(0,0);run();
    // Reset policy follows the core's bankshift model: /VRES clears banks and
    // the original cartridge bootstrap programs them again. This is inferred.
    require(host.unserialize(active),"Sonic pre-reset state restore failed");refreshRam();
    host.reset();host.runFrame();refreshRam();all(0,0);run();
    require(host.unserialize(active),"Sonic pre-power state restore failed");refreshRam();
    require(host.hardReset(error),"Sonic mapper power reset failed");host.runFrame();refreshRam();
    all(0,0);run();
    require(host.movieCoreIdentity()==sonicIdentity && readFile(path)==rom,"Sonic reset changed contract or source ROM");
    // Keep the exact prefix and header but move only the body checksum outside
    // the prefix. Auto-fix is disabled here so this is specifically a checksum
    // negative control, rather than a second changed-prefix control.
    auto negative=rom;const quint16 tail=quint16(romWord(0x1ffffe)+1);
    negative[0x1ffffe]=char(tail>>8);negative[0x1fffff]=char(tail);
    require(sum(negative)==0xf457 && negative.left(0x10000)==rom.left(0x10000),"Sonic negative fixture scope invalid");
    require(!preserve(reinterpret_cast<const unsigned char*>(negative.constData()),size_t(negative.size()),sum(negative)),"Sonic wrong checksum incorrectly preserved");
    const QString negativePath=temp.path()+"/same-prefix-wrong-checksum.md";writeFile(negativePath,negative);
    require(host.setAutoFixChecksum(false,error) && host.load(negativePath,error),"Sonic negative fixture load failed");
    host.runFrame();refreshRam();
    require(host.movieCoreIdentity()==ordinaryIdentity,"Sonic wrong checksum selected mapper contract");
    write(0x408000,0x0100);write(0x4080f0,0x0100);write(0x408600,0x0100);
    write(0x4088f0,0x0300);write(0x409f00,0x0200);write(0x40a000,0x0100);write(0x40a000,0x0200);
    all(0,0);run();
    require(readFile(negativePath)==negative,"Sonic negative source ROM changed");
    require(host.setAutoFixChecksum(true,error) && host.load(ordinaryPath,error),"Sonic generic reload failed");
    require(host.movieCoreIdentity()==ordinaryIdentity && host.networkCompatibility()==ordinaryNetwork,"Sonic mapper ABI leaked to generic reload");
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
        const QString slot = stateFile(temp.path()+"/data",host.contentName(),4);
        require(QFile::exists(slot), "state filename did not use selected ROM name");
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
    // Match a real protection-table key using our generated diagnostic program.
    // The original header must survive both load and power reset with Auto-Fix ON.
    auto protectedRom = rom;
    protectedRom[0x18e] = protectedRom[0x18f] = 0;
    quint16 sum = 0;
    for (int i = 0x200; i < protectedRom.size(); i += 2)
        sum = quint16(sum + (quint16(quint8(protectedRom[i])) << 8) + quint8(protectedRom[i+1]));
    const int tail = protectedRom.size()-2;
    const quint16 lastWord = (quint16(quint8(protectedRom.at(tail))) << 8) |
        quint8(protectedRom.at(tail+1));
    const quint16 adjustment = quint16(0x2288 - sum + lastWord);
    protectedRom[tail] = char(adjustment >> 8); protectedRom[tail+1] = char(adjustment);
    auto specialRom = rom;
    specialRom.replace(0x180, 14, "GM T-574013-00"); // Pier Solar's product family.
    require(specialRom.size() == rom.size(), "special-cartridge header changed ROM size");
    for (const auto& [name, bytes] : std::vector<std::pair<QString,QByteArray>>{
            {"protected.md", protectedRom}, {"protected.smd", smdRom(protectedRom)},
            {"special.md", specialRom}}) {
        const QString path = temp.path()+'/'+name; writeFile(path,bytes);
        require(host.setAutoFixChecksum(false,error) && host.load(path,error), "protected fixture load failed");
        for (int i = 0; i < 8; ++i) host.runFrame();
        const auto originalFrame = host.frame();
        const QRgb pixel = originalFrame.pixel(originalFrame.width()/2,originalFrame.height()/2);
        require(qRed(pixel) > qGreen(pixel)+64, "protected diagnostic baseline did not retain original checksum");
        require(host.setAutoFixChecksum(true,error) && host.hardReset(error), "protected power reset failed");
        for (int i = 0; i < 8; ++i) host.runFrame();
        require(host.frame() == originalFrame, "Auto-Fix changed recognized cartridge header on power reset");
        require(host.load(path,error), "protected auto-fix load failed");
        for (int i = 0; i < 8; ++i) host.runFrame();
        require(host.frame() == originalFrame, "Auto-Fix changed recognized cartridge header on load");
        require(readFile(path) == bytes, "protected source ROM was modified");
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
    RewindBuffer history; history.configure(true);
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

// Original SMS program: mute all PSG channels and hold one YM2413 note.
// Muting PSG makes PCM output an independent check of the FM option.
QByteArray makeSmsFmTestRom() {
    QByteArray rom(32 * 1024, char(0));
    const auto program = QByteArray::fromHex(
        "f331f0df"              // DI; LD SP,$DFF0
        "3e9fd37f3ebfd37f3edfd37f3effd37f" // mute four PSG channels
        "3e01d3f2"          // 315-5297 audio control: output FM only
        "3e30d3f03e10d3f1"  // instrument 1, maximum volume, channel 0
        "3e10d3f03e80d3f1"  // frequency low byte
        "3e20d3f03e15d3f1"  // frequency high bits, key on
        "18fe");            // hold note
    rom.replace(0, program.size(), program);
    rom.replace(0x7ff0, 8, "TMR SEGA");
    rom[0x7fff] = char(0x4c);
    return rom;
}

QString smsFmOption(const LibretroHost& host, bool forPowerReset = false) {
    return host.movieConfiguration(forPowerReset)
        .value(QStringLiteral("options")).toObject()
        .value(QStringLiteral("genesis_plus_gx_ym2413")).toString();
}

void expectSmsFmAudio(LibretroHost& host, bool audible) {
    bool signal = false;
    for (int frame = 0; frame < 12; ++frame) {
        host.runFrame();
        const auto& pcm = host.audio();
        require(!pcm.isEmpty() && pcm.size() % 4 == 0, "SMS FM PCM stream missing");
        if (frame < 2) continue; // let the power-on PSG mute and mixer settle
        for (char sample : pcm) signal |= sample != 0;
    }
    require(signal == audible, audible ? "enabled SMS FM produced silence"
                                      : "disabled SMS FM still produced sound");
}

void testSmsFm(const QString& corePath) {
    QTemporaryDir temp; require(temp.isValid(), "SMS FM test directory");
    const QString smsPath = temp.filePath("original-fm.sms");
    const auto sms = makeSmsFmTestRom();
    writeFile(smsPath, sms);
    LibretroHost host(corePath, temp.path() + "/data");
    require(!host.isMasterSystem(), "unloaded host identified as Master System");
    QString error;
    require(host.load(smsPath, error), "SMS FM fixture load failed");
    QLibrary inspect(corePath);
    using Hardware = unsigned (*)();
    const auto hardware = reinterpret_cast<Hardware>(inspect.resolve("retro_kegarefusion_system_hw"));
    require(hardware != nullptr, "SMS hardware inspection export missing");
    require(hardware() == 0x10 || hardware() == 0x20 || hardware() == 0x21,
            "SMS FM fixture did not select Master System hardware");
    require(host.isMasterSystem(), "Master System hardware was not recognized");
    require(smsFmOption(host) == QLatin1String("enabled"), "SMS FM default was not enabled");
    expectSmsFmAudio(host, true);

    const auto recordedEnabled = host.movieConfiguration();
    host.setSmsFmEnabled(false);
    require(smsFmOption(host) == QLatin1String("enabled"),
            "pending SMS FM preference changed active recording metadata");
    require(smsFmOption(host, true) == QLatin1String("disabled"),
            "new recording did not predict the pending power-reset setting");
    require(host.setNetplayStorage(QStringLiteral("0123456789abcdef0123456789abcdef"), error),
            "network storage switch failed with pending SMS FM");
    require(smsFmOption(host) == QLatin1String("enabled"),
            "network storage switch applied pending SMS FM");
    expectSmsFmAudio(host, true);
    host.reset();
    require(smsFmOption(host) == QLatin1String("enabled"),
            "soft reset changed the active SMS FM option");
    expectSmsFmAudio(host, true);
    require(host.prepareMovie(recordedEnabled, error), "recorded FM option was not restored");
    require(smsFmOption(host) == QLatin1String("enabled"),
            "recorded FM option lost to local pending preference");
    expectSmsFmAudio(host, true);
    require(host.restoreMovieConfiguration(error, true), "SMS FM movie configuration restore failed");
    require(smsFmOption(host) == QLatin1String("enabled"),
            "ending recording hot-switched running SMS FM");
    expectSmsFmAudio(host, true);
    require(host.hardReset(error), "SMS FM disable power reset failed");
    require(smsFmOption(host) == QLatin1String("disabled"),
            "power reset did not apply disabled SMS FM");
    expectSmsFmAudio(host, false);

    auto networkDisabled = host.movieConfiguration();
    networkDisabled.insert(QStringLiteral("networkCompatibility"),
                           QString::fromLatin1(host.networkCompatibility().toHex()));
    host.setSmsFmEnabled(true);
    require(smsFmOption(host) == QLatin1String("disabled") &&
            smsFmOption(host, true) == QLatin1String("enabled"),
            "pending enable leaked into active network metadata");
    host.reset();
    require(smsFmOption(host) == QLatin1String("disabled"),
            "soft reset applied pending SMS FM enable");
    expectSmsFmAudio(host, false);
    require(host.prepareNetwork(networkDisabled, error), "network FM option was not restored");
    require(smsFmOption(host) == QLatin1String("disabled"),
            "network FM option lost to local pending preference");
    expectSmsFmAudio(host, false);
    require(host.restoreMovieConfiguration(error, true), "SMS FM network configuration restore failed");
    require(smsFmOption(host) == QLatin1String("disabled"),
            "ending network session hot-switched running SMS FM");
    expectSmsFmAudio(host, false);
    require(host.hardReset(error), "SMS FM enable power reset failed");
    require(smsFmOption(host) == QLatin1String("enabled"),
            "power reset did not apply enabled SMS FM");
    expectSmsFmAudio(host, true);

    host.setSmsFmEnabled(false);
    require(host.load(smsPath, error), "SMS FM disabled reload failed");
    require(smsFmOption(host) == QLatin1String("disabled"),
            "new load did not apply disabled SMS FM");
    expectSmsFmAudio(host, false);
    host.setSmsFmEnabled(true);
    require(host.load(smsPath, error), "SMS FM enabled reload failed");
    require(smsFmOption(host) == QLatin1String("enabled"),
            "new load did not apply enabled SMS FM");
    expectSmsFmAudio(host, true);

    auto gg = sms; gg[0x7fff] = char(0x6c);
    const QString ggPath = temp.filePath("original-fm.gg");
    writeFile(ggPath, gg);
    require(host.load(ggPath, error), "Game Gear fixture load failed");
    require(hardware() == 0x40 || hardware() == 0x41,
            "Game Gear fixture did not select handheld hardware");
    require(!host.isMasterSystem(), "Game Gear identified as Master System");

    auto sg = sms; sg.replace(0x7ff0, 8, QByteArray(8, char(0)));
    const QString sgPath = temp.filePath("original-fm.sg");
    writeFile(sgPath, sg);
    require(host.load(sgPath, error), "SG-1000 fixture load failed");
    require(hardware() == 0x01 || hardware() == 0x02 || hardware() == 0x03,
            "SG-1000 fixture did not select SG hardware");
    require(!host.isMasterSystem(), "SG-1000 identified as Master System");
}

// Two invocations of this mode verify that a Nuked OPLL snapshot can be
// restored after the core is loaded at a different process address.
void testNetworkOpll(const QString& corePath, const QString& directory, bool reader) {
    require(QDir().mkpath(directory), "network OPLL test directory");
    const QString romPath = directory + "/original-fm.sms";
    const QString snapshotPath = directory + "/network-fm.state";
    const QString expectedPath = directory + "/network-fm.expected";
    if (!reader) {
        QByteArray rom(32 * 1024, char(0));
        // Original Z80 loop: enable FM and write a simple note to YM2413.
        const auto program = QByteArray::fromHex("f331f0df3e10d3f03e20d3f13e20d3f03e11d3f118fe");
        rom.replace(0, program.size(), program);
        rom.replace(0x7ff0, 8, "TMR SEGA");
        rom[0x7fff] = char(0x4c);
        writeFile(romPath, rom);
    }
    LibretroHost host(corePath, directory + "/data");
    QString error;
    require(host.load(romPath, error), "network OPLL ROM load failed");
    const auto compatibility = host.networkCompatibility();
    require(compatibility.size() == 32, "network x64 core contract missing");
    auto configuration = host.movieConfiguration();
    auto options = configuration.value(QStringLiteral("options")).toObject();
    options.insert(QStringLiteral("genesis_plus_gx_ym2413"), QStringLiteral("enabled"));
    options.insert(QStringLiteral("genesis_plus_gx_ym2413_core"), QStringLiteral("nuked"));
    configuration.insert(QStringLiteral("options"), options);
    configuration.insert(QStringLiteral("networkCompatibility"), QString::fromLatin1(compatibility.toHex()));
    require(host.prepareNetwork(configuration, error), "network OPLL setup failed");
    if (reader) {
        require(host.restoreNetworkState(readFile(snapshotPath), error), "network OPLL state restore failed");
    } else {
        for (int i = 0; i < 8; ++i) host.runFrame();
        const auto snapshot = host.captureNetworkState();
        require(snapshot.size() > 128, "network OPLL state capture failed");
        writeFile(snapshotPath, snapshot);
    }
    QCryptographicHash fingerprint(QCryptographicHash::Sha256);
    for (int i = 0; i < 12; ++i) {
        host.runFrame();
        require(!host.audio().isEmpty(), "network OPLL audio missing");
        fingerprint.addData(host.audio());
    }
    fingerprint.addData(host.netplayDigest());
    if (reader) require(fingerprint.result() == readFile(expectedPath), "network OPLL replay diverged");
    else writeFile(expectedPath, fingerprint.result());
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
    history.configure(true);
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
    const QString ram=temp.path()+"/data/saves/rewind.srm";
    if (QFile::exists(ram)) require(quint8(readFile(ram)[0])==0xff, "rewind retained future SRAM");
    history.configure(true);
    for (int i=0;i<10;++i) require(history.capture(states.back(),frames.back(),4.0/3.0), "bounded capture failed");
    require(history.size()==10, "history was limited by checkpoint count");
    RewindBuffer small(1);
    small.configure(true);
    require(!small.capture(states.back(),frames.back(),4.0/3.0) && small.bytesUsed()==0, "rewind memory cap failed");
    history.clear(); require(history.size()==0 && history.bytesUsed()==0, "history clear leaked state");
    history.configure(false);
    require(history.capture(states.back(),frames.back(),4.0/3.0) && history.size()==0, "disabled rewind recorded history");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto mode = argc >= 3 ? QString::fromLocal8Bit(argv[2]) : QString();
    if (argc != 2 && !(argc == 3 && (mode == QLatin1String("--video-geometry") ||
                                     mode == QLatin1String("--archive-only") || mode == QLatin1String("--controller-ports") ||
                                     mode == QLatin1String("--lightguns") || mode == QLatin1String("--sms-fm") || mode == QLatin1String("--checksum-only") || mode == QLatin1String("--vmv-audio") || mode == QLatin1String("--pirate-mappers") || mode == QLatin1String("--aq-microwire") || mode == QLatin1String("--sonic-mapper"))) &&
        !(argc == 4 && (mode == QLatin1String("--network-opll-write") || mode == QLatin1String("--network-opll-read")))) {
        std::fprintf(stderr, "usage: core_integration_tests /path/to/genesis_plus_gx_libretro.so [--sonic-mapper|--aq-microwire|--pirate-mappers|--vmv-audio|--checksum-only|--video-geometry|--archive-only|--controller-ports|--lightguns|--sms-fm|--network-opll-write DIR|--network-opll-read DIR]\n");
        return 2;
    }
    try {
        if (mode == QLatin1String("--network-opll-write") || mode == QLatin1String("--network-opll-read")) {
            testNetworkOpll(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[3]),
                            mode == QLatin1String("--network-opll-read"));
            std::puts("network OPLL snapshot process passed"); return 0;
        }
        if (mode == QLatin1String("--sonic-mapper")) {
            testSonicMapper(QString::fromLocal8Bit(argv[1]));
            std::puts("Sonic original mapper data/fetch, windows, reset/state and identification passed"); return 0;
        }
        if (mode == QLatin1String("--aq-microwire")) {
            testAqMicrowire(QString::fromLocal8Bit(argv[1]));
            std::puts("Aq Microwire serial I/O, partial states, legacy/corruption, battery/reset and isolation passed"); return 0;
        }
        if (mode == QLatin1String("--pirate-mappers")) {
            testPokerOverdump(QString::fromLocal8Bit(argv[1]));
            testJiujiProtection(QString::fromLocal8Bit(argv[1]));
            std::puts("Poker recognition/negative controls/reset/state and Jiuji protected/patched distinction passed"); return 0;
        }
        if (mode == QLatin1String("--vmv-audio")) {
            testVmvBusCompatibility(QString::fromLocal8Bit(argv[1]));
            std::puts("ROM-scoped Z80 access, reset, state restore and isolation passed"); return 0;
        }
        if (mode == QLatin1String("--checksum-only")) {
            testChecksumFix(QString::fromLocal8Bit(argv[1]));
            std::puts("checksum correction and recognized-cartridge preservation passed"); return 0;
        }
        if (mode == QLatin1String("--sms-fm")) {
            testSmsFm(QString::fromLocal8Bit(argv[1]));
            std::puts("SMS FM pending settings, audio, recording/network metadata and hardware classification passed");
            return 0;
        }
        if (mode == QLatin1String("--lightguns")) {
            testLightguns(QString::fromLocal8Bit(argv[1]));
            std::puts("Menacer and Justifier electrical detection and button routing passed");return 0;
        }
        if (mode == QLatin1String("--controller-ports")) {
            testControllerPorts(QString::fromLocal8Bit(argv[1]));
            std::puts("controller port connection, live ID, state restore, hard reset and reload passed");return 0;
        }
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
        testSonicMapper(QString::fromLocal8Bit(argv[1]));
        testAqMicrowire(QString::fromLocal8Bit(argv[1]));
        testPokerOverdump(QString::fromLocal8Bit(argv[1]));
        testJiujiProtection(QString::fromLocal8Bit(argv[1]));
        testVmvBusCompatibility(QString::fromLocal8Bit(argv[1]));
        testNativeVideoGeometry(QString::fromLocal8Bit(argv[1]));
        std::puts("core integration tests passed");
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "core integration test failed: %s\n", exception.what());
        return 1;
    }
}
