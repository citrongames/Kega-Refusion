#include "core/DataFolders.h"
#include "core/EmulationSession.h"
#include "core/MovieSession.h"
#include "core/LibretroHost.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const QString& detail) {
    if (!condition) throw std::runtime_error(detail.toStdString());
}

constexpr unsigned peerCpu(unsigned cpu) { return cpu | 0x100u; }
constexpr quint32 mdEntry = 0x200;
constexpr quint32 mdMarker = 0xff0020;
constexpr quint32 z80Marker = 0xc020;
constexpr quint32 svpEntry = 0x400;
constexpr quint32 svpAfterMailbox = 0x402;
constexpr quint16 svpMarker = 0x5aa5;
constexpr quint32 svpTestIramWord = 0x112;
constexpr quint32 svpTestIram = KR_DEBUG_SSP_IRAM_BASE + svpTestIramWord * 2;
constexpr quint32 svpTestRam0 = KR_DEBUG_SSP_RAM0_BASE + 0x40;
constexpr quint32 svpTestRam1 = KR_DEBUG_SSP_RAM1_BASE;
constexpr quint32 svpTestDram = KR_DEBUG_SSP_DRAM_BASE + 0x40;
constexpr quint32 svpTestPmRom = 0x1f800;
constexpr quint32 subHandler = 0x300;
constexpr quint32 sh2Master = 0x06000000;
constexpr quint32 sh2Slave = 0x06000100;

struct MdAssembler {
    QByteArray rom = QByteArray(128 * 1024, char(0));
    quint32 pc = mdEntry;

    explicit MdAssembler(bool x32 = false) {
        qToBigEndian<quint32>(0x00fffff0, rom.data());
        qToBigEndian<quint32>(mdEntry, rom.data() + 4);
        rom.replace(0x100, 16, x32 ? "SEGA 32X        " : "SEGA GENESIS    ");
        rom.replace(0x120, 16, "DEBUG EXPANSIONS");
        rom.replace(0x180, 2, "GM");
        qToBigEndian<quint32>(quint32(rom.size() - 1), rom.data() + 0x1a4);
        qToBigEndian<quint32>(0x00ff0000, rom.data() + 0x1a8);
        qToBigEndian<quint32>(0x00ffffff, rom.data() + 0x1ac);
        rom[0x1f0] = 'U';
    }
    void word(quint16 value) {
        qToBigEndian(value, rom.data() + pc);
        pc += 2;
    }
    void address(quint32 value) {
        word(quint16(value >> 16)); word(quint16(value));
    }
    void write(quint16 opcode, quint16 value, quint32 target) {
        word(opcode); word(value); address(target);
    }
    void branch(quint32 at, quint32 target) {
        const qint32 delta = qint32(target) - qint32(at + 2);
        require(delta >= -128 && delta <= 127 && delta != 0, "68000 fixture branch displacement");
        rom[at + 1] = char(delta);
    }
    void start() {
        word(0x46fc); word(0x2700); // Supervisor, interrupts masked.
        write(0x33fc, 0x8004, 0xc00004);
        write(0x33fc, 0x8144, 0xc00004);
        for (quint16 data : {0x8d, 0x12, 0x93}) write(0x13fc, data, 0xc00011);
    }
};

QByteArray svpRom() {
    MdAssembler code;
    code.rom.replace(0x1c8, 2, "SV");
    code.start();
    const auto poll = code.pc;
    code.word(0x3039); code.address(0xa15000);     // MOVE.W (XST).L,D0
    code.word(0x0c40); code.word(svpMarker);     // CMPI.W #$5AA5,D0
    const auto retry = code.pc;
    code.word(0x6600);
    code.branch(retry, poll);
    code.word(0x33c0); code.address(mdMarker);   // MOVE.W D0,(work RAM).L
    code.word(0x60fe);

    code.pc = svpEntry * 2;                    // SSP addresses program words.
    const auto ldi = [&](unsigned reg, quint16 value) {
        code.word(0x0800 | (reg << 4));
        code.word(value);
    };
    const auto pm4 = [&](quint16 address, quint16 mode, bool write) {
        ldi(14, address);                       // PMC low, PMC high.
        ldi(14, mode);
        code.word(write ? 0x00c0 : 0x000c);    // LD PM4,- or LD -,PM4.
    };
    ldi(11, svpMarker);                         // $0400: LDI XST,$5AA5.
    pm4(0x8000 + svpTestIramWord, 0x081c, true);
    ldi(12, 0xcafe);                            // $0407: PM4 IRAM write.
    ldi(3, 0x1234);
    code.word(0x0e20);                         // $040b: LD RAM0[$20],A.
    code.word(0x1820);                         // LDI r0,$20.
    code.word(0x021c);                         // $040d: LD X,(r0+).
    code.word(0x0d03); code.word(svpTestIramWord); // $040e: LDI (r7|0),$0112.
    code.word(0x0b23);                         // $0410: LD Y,((r7|0)).
    ldi(3, svpEntry);
    code.word(0x4a30);                         // $0413: LD A,(A) reads the first opcode as data.
    pm4(svpTestPmRom / 2, 0x0800, false);
    code.word(0x003c);                         // $0419: LD A,PM4 reads cartridge ROM.
    pm4(0x20, 0x0818, true);
    ldi(12, 0x3fc5);                            // $041f: PM4 DRAM write.
    pm4(0x20, 0x4418, true);
    ldi(12, 0x0070);                            // $0426: PM4 cell/overwrite DRAM write.
    pm4(0x20, 0x0818, false);
    code.word(0x003c);                         // $042d: LD A,PM4 reads merged $3F75.
    const quint16 loop = code.pc / 2;
    code.word(0x4c00); code.word(loop);         // BRA always,loop.

    code.pc = svpTestPmRom;
    code.word(0xbeef);
    return code.rom;
}

QByteArray mdLinkRom() {
    MdAssembler code;
    code.start();
    code.word(0x1039); code.address(0xa10003);  // MOVE.B (pad A).L,D0
    code.word(0x0800); code.word(3);            // BTST #3,D0; Right selects receiver.
    const auto receiverJump = code.pc;
    code.word(0x6700);
    code.write(0x13fc, 0x21, 0xa1000b);         // Port B D0 and crossed TR output.
    code.write(0x13fc, 0, 0xa10005);            // Drive D0 and TR low to the other console.
    code.word(0x7031);                          // Sender marker.
    const auto senderJump = code.pc;
    code.word(0x6000);

    code.branch(receiverJump, code.pc);
    code.write(0x13fc, 0, 0xa1000b);            // Port B input.
    const auto poll = code.pc;
    code.word(0x1039); code.address(0xa10005);
    code.word(0x0200); code.word(1);            // ANDI.B #1,D0
    const auto retry = code.pc;
    code.word(0x6600);
    code.branch(retry, poll);
    code.word(0x7062);                          // Receiver marker after cable becomes low.
    code.branch(senderJump, code.pc);
    code.word(0x13c0); code.address(mdMarker);
    code.word(0x60fe);
    return code.rom;
}

QByteArray ggLinkRom() {
    QByteArray rom(32 * 1024, char(0));
    quint32 pc = 0;
    const auto bytes = [&](std::initializer_list<unsigned> values) {
        for (auto value : values) rom[pc++] = char(value);
    };
    const auto out = [&](unsigned port, unsigned value) { bytes({0x3e, value, 0xd3, port}); };
    const auto branch = [&](quint32 at, quint32 target) {
        const qint32 delta = qint32(target) - qint32(at + 2);
        require(delta >= -128 && delta <= 127, "Z80 fixture branch displacement");
        rom[at + 1] = char(delta);
    };
    bytes({0xf3, 0x31, 0xf0, 0xc3});            // DI; LD SP,$C3F0
    out(0xbf, 4); out(0xbf, 0x80);
    out(0xbf, 0x40); out(0xbf, 0x81);
    out(0x06, 0xff);
    for (unsigned value : {0x8d, 0x12, 0x93}) out(0x7f, value);
    bytes({0xdb, 0, 0xcb, 0x7f});               // IN A,(0); BIT 7,A: Start selects receiver.
    const auto receiverJump = pc;
    bytes({0x28, 0});                          // JR Z,receiver
    out(1, 0); out(2, 0xfe);                  // Drive PC0 low, with NMI disabled.
    bytes({0x3e, 0x31});                       // Sender marker.
    const auto senderJump = pc;
    bytes({0x18, 0});                          // JR store

    branch(receiverJump, pc);
    out(1, 0x40); out(2, 0xbf);                // Drive TH high toward A, with NMI disabled.
    bytes({0x06, 0x40, 0x10, 0xfe});           // Keep TH high across several scanlines.
    out(1, 0);                                 // Drop TH while still sampling crossed PC0.
    const auto poll = pc;
    bytes({0xdb, 1, 0xcb, 0x57});               // IN A,(1); BIT 2,A (peer PC0).
    const auto retry = pc;
    bytes({0x20, 0});
    branch(retry, poll);
    bytes({0x3e, 0x62});                       // Receiver marker after cable becomes low.
    branch(senderJump, pc);
    bytes({0x32, 0x20, 0xc0, 0x18, 0xfe});     // LD ($C020),A; JR *
    rom.replace(0x7ff0, 8, "TMR SEGA");
    rom[0x7fff] = char(0x6c);
    return rom;
}

QByteArray cdRom() {
    MdAssembler code;
    code.start();
    code.write(0x13fc, 0, 0xa12001);            // Hold S68K reset; main owns PRG RAM.
    code.word(0x3639); code.address(0x600002);  // Read physical word 1 in 2M mode.
    code.word(0x3639); code.address(0x640002);  // Same word through the main-CPU mirror.
    const auto subWord = [&](quint32 offset, quint16 value) {
        code.write(0x33fc, value, 0x420000 + offset);
    };
    subWord(0x00, 0); subWord(0x02, 0x1000);   // Reset SP.
    subWord(0x04, 0); subWord(0x06, 0x0200);   // Reset PC.
    subWord(0x68, 0); subWord(0x6a, subHandler);
    quint32 offset = 0x200;
    for (quint16 value : {0x33fc, 4, 0x00ff, 0x8032,    // Unmask level 2 in gate array.
                          0x46fc, 0x2000,               // Unmask IRQ at CPU.
                          0x33fc, 0x5aaa, 0x00ff, 0x802a,
                          0x4e71, 0x60fc}) {
        subWord(offset, value);
        offset += 2;
    }
    offset = subHandler;
    for (quint16 value : {0x33fc, 0, 0x00ff, 0x8042,    // CDD command 0 (Get Drive Status).
                          0x33fc, 0, 0x00ff, 0x804a,    // Commit command.
                          0x13fc, 4, 0x00ff, 0x8003,    // 1M mode, sub CPU gets bank 1.
                          0x3439, 0x000c, 0x0002,       // Word 1 in bank 1 is physical word 3.
                          0x1639, 0x0008, 0x0009,       // DOT view reads one physical byte.
                          0x5279, 0x00ff, 0x8028, 0x4e73}) { // Count IRQ2; RTE.
        subWord(offset, value);
        offset += 2;
    }
    code.write(0x13fc, 1, 0xa12001);             // Release S68K reset.
    const auto waitForReady = code.pc;
    code.word(0x3039); code.address(0xa1202a);
    code.word(0x0c40); code.word(0x5aaa);
    auto retry = code.pc;
    code.word(0x6600);
    code.branch(retry, waitForReady);
    code.write(0x13fc, 1, 0xa12000);             // Main requests S68K IRQ2.
    const auto waitForAck = code.pc;
    code.word(0x3039); code.address(0xa12028);
    code.word(0x4a40);
    retry = code.pc;
    code.word(0x6700);
    code.branch(retry, waitForAck);
    code.word(0x33c0); code.address(mdMarker);
    code.word(0x60fe);
    return code.rom;
}

QByteArray x32Rom() {
    MdAssembler code(true);
    code.start();
    code.write(0x33fc, 3, 0xa15100);              // Enable adapter and release both SH-2s.
    const auto poll = code.pc;
    code.word(0x4a79); code.address(0xa15128);     // Wait for built-in IPL checksum.
    const auto retry = code.pc;
    code.word(0x6700);
    code.branch(retry, poll);
    code.write(0x33fc, 0, 0xa15128);              // Acknowledge checksum to permit IPL copy.
    code.word(0x60fe);

    constexpr quint32 image = 0x1000;
    qToBigEndian<quint32>(image, code.rom.data() + 0x3d4);
    qToBigEndian<quint32>(0, code.rom.data() + 0x3d8);
    qToBigEndian<quint32>(0x200, code.rom.data() + 0x3dc);
    qToBigEndian<quint32>(sh2Master, code.rom.data() + 0x3e0);
    qToBigEndian<quint32>(sh2Slave, code.rom.data() + 0x3e4);
    qToBigEndian<quint32>(0x06000200, code.rom.data() + 0x3e8);
    qToBigEndian<quint32>(0x06000300, code.rom.data() + 0x3ec);
    for (unsigned side = 0; side < 2; ++side) {
        const quint32 initialPc = image + 0x100 * side;
        code.pc = initialPc;
        code.word(0xd104);                      // MOV.L @(4,PC),R1: SDRAM marker address.
        code.word(side ? 0xe022 : 0xe011);        // MOV #$22/$11,R0
        code.word(0xa001);                      // BRA 0x0A
        code.word(0x7001);                      // ADD #1,R0 (delay slot).
        code.word(0xe07f);                      // This opcode must be skipped.
        code.word(0x2100);                      // MOV.B R0,@R1
        code.word(0xb008);                      // 0x0C: BSR 0x20
        code.word(0x7002);                      // ADD #2,R0 in the call slot.
        code.word(0xaff7); code.word(0x0009);    // 0x10: BRA 0x02; NOP
        code.pc = initialPc + 0x14;
        code.address(0x06000500u + 2 * side);
        code.pc = initialPc + 0x20;
        code.word(0x4f22);                      // STS.L PR,@-R15
        code.word(0xb005);                      // 0x22: BSR 0x30 (nested).
        code.word(0x7004);                      // ADD #4,R0 in its call slot.
        code.word(0x4f26);                      // 0x26: LDS.L @R15+,PR
        code.word(0x000b);                      // RTS to 0x10.
        code.word(0x7008);                      // ADD #8,R0 in the outer return slot.
        code.pc = initialPc + 0x30;
        code.word(0x000b);                      // RTS to 0x26.
        code.word(0x7010);                      // ADD #16,R0 in the nested return slot.
    }
    return code.rom;
}

struct RegisterProgram {
    QByteArray rom;
    quint32 conditional, redirected, tail;
};

RegisterProgram registerRom() {
    MdAssembler code;
    code.start();
    code.word(0x7011);                         // MOVEQ #$11,D0 leaves Z clear.
    const auto conditional = code.pc;
    code.word(0x6600);                         // BNE bad
    code.word(0x7038);                         // The edited Z flag selects this path.
    const auto goodJump = code.pc;
    code.word(0x6000);
    code.branch(conditional, code.pc);
    code.word(0x7017);                         // Ordinary branch with Z clear.
    const auto badJump = code.pc;
    code.word(0x6000);
    const auto redirected = code.pc;
    code.word(0x7067);                         // Only a debugger PC edit selects this path.
    code.branch(goodJump, code.pc);
    code.branch(badJump, code.pc);
    code.word(0x13c0); code.address(mdMarker);
    const auto tail = code.pc;
    code.word(0x60fe);
    return {std::move(code.rom), conditional, redirected, tail};
}

struct Options {
    bool link = false;
    unsigned peerButtons = 0;
    ExpansionMode cd = ExpansionMode::Auto;
    ExpansionMode x32 = ExpansionMode::Auto;
    QString biosDirectory;
};

void setup(LibretroHost& host, const QString& path, const QString& data, const Options& options) {
    QString error;
    if (!options.biosDirectory.isEmpty()) {
        const bool mapped = DataFolders::save(data, {{"bios", options.biosDirectory}}, error);
        require(mapped, error);
    }
    host.setExpansionDevices(options.cd, options.x32);
    if (options.link) {
        const bool enabled = host.setLinkCableEnabled(true, error);
        require(enabled, error);
    }
    const bool loaded = host.load(path, error);
    require(loaded, QStringLiteral("Load %1 — %2").arg(path, error));
    if (options.link) {
        require(host.linkCableEnabled(), "Link Cable established an isolated peer");
        host.setButtons(0, 0);
        host.setButtons(1, options.peerButtons);
    }
}

struct Frame {
    QByteArray state, pcm;
    QImage first, second;
    LibretroHost::RunClock clock;
};

Frame completeFrame(const LibretroHost& host, bool link) {
    return {host.serialize(), host.audio(), host.frameForLinkRole(1),
            link ? host.frameForLinkRole(2) : QImage{}, host.runClock()};
}

void writeRom(const QString& path, const QByteArray& rom) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(rom) == rom.size(),
            QStringLiteral("Write fixture %1").arg(path));
}

DebuggerBreakpoint point(unsigned id, unsigned cpu, unsigned event, quint32 address,
                         bool oneShot = true) {
    DebuggerBreakpoint result;
    result.id = id;
    result.cpu = cpu;
    result.events = event;
    result.first = result.last = address;
    result.oneShot = oneShot;
    return result;
}

void command(LibretroHost& host, DebuggerCommand::Kind kind, unsigned cpu,
             const QVector<DebuggerBreakpoint>& points = {}) {
    DebuggerCommand request;
    request.kind = kind;
    request.cpu = cpu;
    request.breakpoints = points;
    QString error;
    const bool accepted = host.debuggerCommand(request, error);
    require(accepted, QStringLiteral("Debugger command %1, target %2 — %3")
                          .arg(unsigned(kind)).arg(cpu).arg(error));
}

void trace(LibretroHost& host, bool start, unsigned cpu = 0,
           unsigned filters = KR_TRACE_INSTRUCTION) {
    DebugTraceCommand request;
    request.kind = start ? DebugTraceCommand::Start : DebugTraceCommand::Stop;
    request.config.cpu = cpu;
    request.config.filters = filters;
    request.config.memoryMiB = 1;
    QString error;
    const bool accepted = host.traceCommand(request, error);
    require(accepted, error);
}

bool traced(const DebugTraceSnapshot& snapshot, unsigned cpu, unsigned kind,
            quint32 address, quint32 value, unsigned instance = 0) {
    return std::any_of(snapshot.rows.begin(), snapshot.rows.end(), [&](const auto& row) {
        const auto& item = row.record.native;
        return row.record.instance == instance && item.cpu == cpu && item.kind == kind &&
               item.address == address && item.value == value;
    });
}

void coverage(LibretroHost& host, bool start, unsigned cpu = 0) {
    DebugCoverageCommand request;
    request.kind = start ? DebugCoverageCommand::Start : DebugCoverageCommand::Stop;
    request.config.cpu = cpu;
    QString error;
    const bool accepted = host.coverageCommand(request, error);
    require(accepted, error);
}

bool covered(LibretroHost& host, const char* name, unsigned offset, unsigned count, unsigned kind) {
    const auto snapshot = host.coverageSnapshot();
    const auto source = std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
        [&](const auto& entry) { return std::strcmp(entry.native.name, name) == 0; });
    if (source == snapshot.sources.end()) return false;
    DebugCoverageCommand page;
    page.kind = DebugCoverageCommand::Refresh;
    page.source = source->native.id;
    page.offset = offset;
    QString error;
    const bool accepted = host.coverageCommand(page, error);
    require(accepted, error);
    const auto view = host.coverageSnapshot();
    if (view.offset > offset || offset - view.offset + count > unsigned(view.page.size())) return false;
    for (unsigned i = 0; i < count; ++i)
        if (!(uint8_t(view.page.at(offset - view.offset + i)) & kind)) return false;
    return true;
}

kr_debug_target target(const DebuggerSnapshot& snapshot, unsigned id) {
    const auto found = std::find_if(snapshot.targets.begin(), snapshot.targets.end(),
                                    [id](const auto& item) { return item.cpu == id; });
    require(found != snapshot.targets.end(), QStringLiteral("Enumerate debugger target %1").arg(id));
    return *found;
}

kr_debug_cpu cpu(const DebuggerSnapshot& snapshot, unsigned id) {
    const auto found = std::find_if(snapshot.cpus.begin(), snapshot.cpus.end(),
                                    [id](const auto& item) { return item.cpu == id; });
    require(found != snapshot.cpus.end(), QStringLiteral("Snapshot debugger target %1").arg(id));
    return *found;
}

quint32 reg(const DebuggerSnapshot& snapshot, unsigned id, const char* name) {
    const auto state = cpu(snapshot, id);
    const auto* end = state.regs + state.count;
    const auto* found = std::find_if(state.regs, end, [name](const auto& item) {
        return std::strcmp(item.name, name) == 0;
    });
    require(found != end, QStringLiteral("Snapshot register %1").arg(QString::fromLatin1(name)));
    return found->value;
}

// The versioned 32X state stores PPC and EA for both execution backends.
// DRC does not maintain these interpreter scratch words. Their restoration
// does not affect the next guest instruction, so every other byte must match.
bool same32xState(const QByteArray& actual, const QByteArray& expected) {
    constexpr qsizetype envelopeSize = 56, wireHeader = 24, busWire = 628, pwmWire = 45;
    constexpr qsizetype cpuWire = 4776, socWire = 591, times = 24, memoryWire = 512 * 1024;
    constexpr qsizetype wireSize = wireHeader + busWire + pwmWire + 2 * cpuWire + 2 * socWire + times + memoryWire;
    constexpr qsizetype extensionSize = envelopeSize + wireSize;
    if (actual.size() != expected.size() || actual.size() < extensionSize) return false;
    const qsizetype extension = actual.size() - extensionSize;
    const qsizetype cpuStart = extension + envelopeSize + wireHeader + busWire + pwmWire;
    const auto valid = [&](const QByteArray& bytes) {
        const char* header = bytes.constData() + extension;
        const char* wire = header + envelopeSize;
        return std::memcmp(header, "KR32X001", 8) == 0 && qFromLittleEndian<quint32>(header + 8) == 1 &&
               qFromLittleEndian<quint32>(header + 12) == extensionSize &&
               std::memcmp(wire, "K32XST01", 8) == 0 && qFromLittleEndian<quint32>(wire + 8) == 2 &&
               qFromLittleEndian<quint32>(wire + 12) == wireSize - wireHeader &&
               cpuStart + 2 * cpuWire <= bytes.size();
    };
    if (!valid(actual) || !valid(expected)) return false;
    const auto* first = actual.constData();
    const auto* second = expected.constData();
    qsizetype from = 0;
    for (qsizetype ignore : {cpuStart + 8, cpuStart + 36, cpuStart + cpuWire + 8, cpuStart + cpuWire + 36}) {
        if (std::memcmp(first + from, second + from, size_t(ignore - from)) != 0) return false;
        from = ignore + sizeof(quint32);
    }
    return std::memcmp(first + from, second + from, size_t(actual.size() - from)) == 0;
}

class Timeline {
public:
    Timeline(const QString& core, const QString& path, const QString& data, const Options& options,
             unsigned frames, const QString& diagnostics = {})
        : linked(options.link), x32(options.x32 == ExpansionMode::Connected),
          diagnosticsDirectory(diagnostics) {
        const auto root = data + "/reference";
        LibretroHost reference(core, root);
        setup(reference, path, root, options);
        expected.reserve(frames);
        for (unsigned i = 0; i < frames; ++i) {
            if (linked) reference.setLinkPresentationRole(1 + int(i % 2));
            reference.runFrame();
            auto result = completeFrame(reference, linked);
            require(!result.state.isEmpty() && !result.first.isNull() && !result.pcm.isEmpty() &&
                    (!linked || !result.second.isNull()), "Capture complete reference state, pictures and PCM");
            expected.push_back(std::move(result));
        }
    }

    DebuggerSnapshot stop(LibretroHost& host, unsigned id, unsigned event, quint32 address,
                          quint32 alternate = 0xffffffffu) {
        while (index < expected.size()) {
            const auto before = host.runClock();
            const QImage first = host.frameForLinkRole(1), second = host.frameForLinkRole(2);
            if (run(host)) continue;
            const auto snapshot = host.debuggerSnapshot();
            require(snapshot.partialFrame && snapshot.stopped && snapshot.eventCpu == id &&
                    snapshot.event == event &&
                    (snapshot.address == address || snapshot.address == alternate),
                    QStringLiteral("Stop expected target %1 event %2 at %3, got %4/%5/%6")
                        .arg(id).arg(event).arg(address, 0, 16)
                        .arg(snapshot.eventCpu).arg(snapshot.event).arg(snapshot.address, 0, 16));
            require(snapshot.frame == before.frames && host.runClock().frames == before.frames &&
                    host.runClock().ticks == before.ticks && host.audio().isEmpty() &&
                    host.frameForLinkRole(1) == first && host.frameForLinkRole(2) == second,
                    "Debugger stop leaves public frame clock, images and PCM at the completed boundary");
            return snapshot;
        }
        throw std::runtime_error("Expected debugger stop did not occur in the bounded timeline");
    }

    void finish(LibretroHost& host) {
        while (index < expected.size())
            require(run(host), "Unexpected breakpoint after the planned stops");
    }

    void detach(LibretroHost& host) {
        const bool pending = host.debuggerFramePending();
        host.debuggerDetach();
        if (pending) compare(host);
    }

    void replay(LibretroHost& host, const QByteArray& checkpoint, quint64 frame) {
        require(!host.debuggerFramePending() && !host.debuggerAttached() && frame < expected.size() &&
                host.restoreRewindState(checkpoint) && host.runClock().frames == frame,
                "Restore the completed post-debug frame before plain replay");
        index = size_t(frame);
        finish(host);
    }

private:
    bool run(LibretroHost& host) {
        if (linked && !host.debuggerFramePending()) host.setLinkPresentationRole(1 + int(index % 2));
        if (!host.runDebugFrame()) return false;
        compare(host);
        return true;
    }

    void compare(LibretroHost& host) {
        const auto actual = completeFrame(host, linked);
        const auto& reference = expected.at(index++);
        const bool clock = actual.clock.frames == reference.clock.frames &&
                           actual.clock.ticks == reference.clock.ticks;
        const bool state = actual.state == reference.state || (x32 && same32xState(actual.state, reference.state));
        const bool image = actual.first == reference.first && actual.second == reference.second;
        const bool pcm = actual.pcm == reference.pcm;
        if (!(clock && state && image && pcm) && !diagnosticsDirectory.isEmpty()) {
            QDir directory(diagnosticsDirectory);
            require(QDir().mkpath(directory.path()), "Create diagnostic directory");
            const auto write = [&](const char* name, const QByteArray& bytes) {
                QFile file(directory.filePath(QString::fromLatin1(name)));
                require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                        file.write(bytes) == bytes.size(), "Write first mismatch diagnostic");
            };
            write("baseline.state", reference.state);
            write("resumed.state", actual.state);
            write("baseline.pcm", reference.pcm);
            write("resumed.pcm", actual.pcm);
            if (!image) {
                require(reference.first.save(directory.filePath("baseline.png")) &&
                        actual.first.save(directory.filePath("resumed.png")), "Write differing screen images");
            }
        }
        require(clock && state && image && pcm,
                QStringLiteral("Stopped/resumed frame %1 differs (state=%2, image=%3, PCM=%4, clock=%5)")
                    .arg(reference.clock.frames).arg(!state).arg(!image).arg(!pcm).arg(!clock));
    }

    std::vector<Frame> expected;
    size_t index = 0;
    bool linked, x32;
    QString diagnosticsDirectory;
};

void testSvp(const QString& core, const QTemporaryDir& temp) {
    const auto path = temp.filePath("original-svp.md");
    const auto data = temp.filePath("svp");
    writeRom(path, svpRom());
    const Options options;
    Timeline timeline(core, path, data, options, 5);
    LibretroHost host(core, data + "/debug");
    setup(host, path, data + "/debug", options);

    coverage(host, true, KR_DEBUG_SSP1601);
    trace(host, true, KR_DEBUG_SSP1601);
    auto internalRead = point(4, KR_DEBUG_SSP1601, KR_DEBUG_READ, svpTestRam0);
    internalRead.condition = QStringLiteral("R0 == $21");
    auto dramRead = point(10, KR_DEBUG_SSP1601, KR_DEBUG_READ, svpTestDram);
    dramRead.condition = QStringLiteral("PMC == $08180021");
    command(host, DebuggerCommand::Configure, KR_DEBUG_SSP1601, {
        point(1, KR_DEBUG_SSP1601, KR_DEBUG_EXEC, svpEntry),
        point(2, KR_DEBUG_M68K, KR_DEBUG_WRITE, mdMarker),
        point(3, KR_DEBUG_SSP1601, KR_DEBUG_WRITE, svpTestRam0),
        internalRead,
        point(5, KR_DEBUG_SSP1601, KR_DEBUG_READ, svpTestRam1),
        point(6, KR_DEBUG_SSP1601, KR_DEBUG_WRITE, svpTestRam1, false),
        point(7, KR_DEBUG_SSP1601, KR_DEBUG_WRITE, svpTestIram),
        point(8, KR_DEBUG_SSP1601, KR_DEBUG_READ, svpTestIram),
        point(9, KR_DEBUG_SSP1601, KR_DEBUG_WRITE, svpTestDram, false),
        dramRead,
        point(11, KR_DEBUG_SSP1601, KR_DEBUG_READ, svpTestPmRom),
        point(12, KR_DEBUG_SSP1601, KR_DEBUG_READ, svpEntry * 2 + 1),
        point(13, KR_DEBUG_SSP1601, KR_DEBUG_READ, svpEntry * 2 + 2),
    });
    const auto first = timeline.stop(host, KR_DEBUG_SSP1601, KR_DEBUG_EXEC, svpEntry);
    const auto ssp = target(first, KR_DEBUG_SSP1601);
    const auto m68k = target(first, KR_DEBUG_M68K);
    constexpr unsigned capabilities = KR_DEBUG_CAP_STEP | KR_DEBUG_CAP_READ | KR_DEBUG_CAP_WRITE;
    require(ssp.address_bits == 16 && ssp.address_shift == 1 && m68k.address_shift == 0 &&
            (ssp.capabilities & capabilities) == capabilities,
            "SSP exposes its original program-word navigation and data stops");
    DebuggerCommand invalid;
    invalid.kind = DebuggerCommand::Validate;
    invalid.cpu = KR_DEBUG_SSP1601;
    invalid.breakpoints = {point(100, KR_DEBUG_SSP1601, KR_DEBUG_EXEC | KR_DEBUG_READ, svpEntry)};
    QString error;
    require(!host.debuggerCommand(invalid, error), "Reject mixed SSP program and data addresses");
    invalid.breakpoints = {point(100, KR_DEBUG_SSP1601, KR_DEBUG_EXEC, svpTestRam0)};
    require(!host.debuggerCommand(invalid, error), "Reject an SSP code address outside the word range");
    require(!first.instructions.isEmpty() && first.instructions.front().address == svpEntry &&
            first.instructions.front().length == 4, "SSP LDI disassembly includes its immediate word");
    command(host, DebuggerCommand::StepInto, KR_DEBUG_SSP1601);
    timeline.stop(host, KR_DEBUG_SSP1601, KR_DEBUG_EXEC, svpAfterMailbox);
    const auto sspTrace = host.traceSnapshot();
    require(traced(sspTrace, KR_DEBUG_SSP1601, KR_TRACE_EXEC, svpEntry, 0x08b0),
            "Trace logs the executed SSP opcode at its word address");
    const auto& lastSsp = sspTrace.rows.back().record.native;
    require((lastSsp.flags & KR_TRACE_TIME_QUANTIZED) && lastSsp.width == 2,
            "SVP trace distinguishes slice-quantized timing and fetched word width");
    require(covered(host, "SVP program ROM", 0, 4, KR_COV_EXEC) &&
            !covered(host, "SVP program ROM", 4, 1, KR_COV_EXEC),
            "SVP coverage includes the executed opcode and immediate word before the next stop");
    coverage(host, false);
    trace(host, false);
    const auto dataStop = [&](unsigned event, quint32 address, quint32 value, quint32 pc) {
        command(host, DebuggerCommand::Continue, KR_DEBUG_SSP1601);
        const auto result = timeline.stop(host, KR_DEBUG_SSP1601, event, address);
        require(result.value == value && result.accessSize == 2 &&
                cpu(result, KR_DEBUG_SSP1601).pc == pc && !result.instructions.isEmpty() &&
                result.instructions.front().address == pc,
                QStringLiteral("SSP data stop at $%1 exposes the accessed word and issuing instruction")
                    .arg(address, 0, 16));
        return result;
    };
    dataStop(KR_DEBUG_WRITE, svpTestIram, 0xcafe, 0x407);
    dataStop(KR_DEBUG_WRITE, svpTestRam0, 0x1234, 0x40b);
    const auto pointerRead = dataStop(KR_DEBUG_READ, svpTestRam0, 0x1234, 0x40d);
    require(reg(pointerRead, KR_DEBUG_SSP1601, "R0") == 0x21 &&
            reg(pointerRead, KR_DEBUG_SSP1601, "X") == 0,
            "SSP pointer advances before the read stop and destination is written on resume");
    DebuggerCommand write;
    write.kind = DebuggerCommand::WriteRegister;
    write.cpu = KR_DEBUG_SSP1601;
    write.registerName = QStringLiteral("X");
    write.registerValue = 1;
    require(!host.debuggerCommand(write, error), "Reject SSP register edits at a data stop");
    dataStop(KR_DEBUG_WRITE, svpTestRam1, svpTestIramWord, 0x40e);
    dataStop(KR_DEBUG_READ, svpTestRam1, svpTestIramWord, 0x410);
    dataStop(KR_DEBUG_WRITE, svpTestRam1, svpTestIramWord + 1, 0x410);
    dataStop(KR_DEBUG_READ, svpTestIram, 0xcafe, 0x410);
    dataStop(KR_DEBUG_READ, svpEntry * 2, 0x08b0, 0x413);
    dataStop(KR_DEBUG_READ, svpTestPmRom, 0xbeef, 0x419);
    dataStop(KR_DEBUG_WRITE, svpTestDram, 0x3fc5, 0x41f);
    dataStop(KR_DEBUG_WRITE, svpTestDram, 0x0070, 0x426);
    dataStop(KR_DEBUG_READ, svpTestDram, 0x3f75, 0x42d);

    command(host, DebuggerCommand::Continue, KR_DEBUG_SSP1601);
    const auto marker = timeline.stop(host, KR_DEBUG_M68K, KR_DEBUG_WRITE, mdMarker);
    require(marker.value == svpMarker && marker.accessSize == 2, "68000 observed the SSP mailbox write");
    command(host, DebuggerCommand::Continue, KR_DEBUG_M68K);
    timeline.finish(host);
}

void testLink(const QString& core, const QTemporaryDir& temp, bool gg) {
    const auto path = temp.filePath(gg ? "original-link.gg" : "original-link.md");
    const auto data = temp.filePath(gg ? "gg-link" : "md-link");
    writeRom(path, gg ? ggLinkRom() : mdLinkRom());
    Options options;
    options.link = true;
    options.peerButtons = 1u << (gg ? RETRO_DEVICE_ID_JOYPAD_START : RETRO_DEVICE_ID_JOYPAD_RIGHT);
    const auto native = gg ? KR_DEBUG_Z80 : KR_DEBUG_M68K;
    const auto peer = peerCpu(native);
    const auto address = gg ? z80Marker : mdMarker;

    Timeline timeline(core, path, data, options, 4);
    LibretroHost host(core, data + "/debug");
    setup(host, path, data + "/debug", options);
    trace(host, true, 0, KR_TRACE_LINK);
    command(host, DebuggerCommand::Configure, peer, {point(1, peer, KR_DEBUG_WRITE, address)});
    const auto received = timeline.stop(host, peer, KR_DEBUG_WRITE, address);
    const auto first = target(received, native);
    const auto second = target(received, peer);
    require(received.linkEnabled && first.address_bits == second.address_bits &&
            received.value == 0x62 && received.accessSize == 1,
            "Only the B receiver marker triggers after A has driven the cable low");
    require(cpu(received, peer).cpu != cpu(received, native).cpu, "Distinct linked CPU snapshots");
    const quint32 loop = cpu(received, peer).pc;
    command(host, DebuggerCommand::Continue, peer);
    timeline.finish(host);
    require(traced(host.traceSnapshot(), native, KR_TRACE_LINK_EDGE, gg ? 1 : 0xa10005, 0, gg ? 0 : 1),
            "The receiving console traces the actual falling edge on the cable");
    trace(host, false);

    // Once a paired frame is suspended, disabling the cable must complete both
    // workers and release the peer before execution can begin on A alone.
    command(host, DebuggerCommand::Configure, peer, {point(2, peer, KR_DEBUG_EXEC, loop, false)});
    const auto clock = host.runClock();
    require(!host.runDebugFrame() && host.debuggerFramePending(), "Suspend the linked peer before cable disable");
    QString error;
    const bool disabled = host.setLinkCableEnabled(false, error);
    require(disabled, error);
    require(!host.linkCableEnabled() && !host.debuggerFramePending() &&
            host.runClock().frames == clock.frames + 1, "Cable disable drains a single paired frame");
    command(host, DebuggerCommand::Attach, native);
    const auto after = host.debuggerSnapshot();
    require(!after.linkEnabled && std::none_of(after.targets.begin(), after.targets.end(),
                [](const auto& item) { return item.cpu & 0x100u; }),
            "No peer targets survive cable removal");
    host.unload();
    require(!host.loaded() && !host.debuggerFramePending(), "Unload releases the remaining console");
}

void testCd(const QString& core, const QTemporaryDir& temp, const QString& biosDirectory) {
    const auto path = temp.filePath("original-cd-mode1.md");
    const auto data = temp.filePath("cd-mode1");
    writeRom(path, cdRom());
    Options options;
    options.cd = ExpansionMode::Connected;
    options.x32 = ExpansionMode::Disconnected;
    options.biosDirectory = biosDirectory;
    Timeline timeline(core, path, data, options, 6);
    LibretroHost host(core, data + "/debug");
    setup(host, path, data + "/debug", options);
    coverage(host, true);
    trace(host, true, KR_DEBUG_SUB68K, KR_TRACE_INSTRUCTION | KR_TRACE_IRQ | KR_TRACE_DISC);
    command(host, DebuggerCommand::Configure, KR_DEBUG_SUB68K, {
        point(1, KR_DEBUG_SUB68K, KR_DEBUG_EXEC, subHandler),
        point(2, KR_DEBUG_M68K, KR_DEBUG_WRITE, mdMarker),
    });
    const auto irq = timeline.stop(host, KR_DEBUG_SUB68K, KR_DEBUG_EXEC, subHandler);
    const auto sub = target(irq, KR_DEBUG_SUB68K);
    require(sub.address_bits == 24 && cpu(irq, KR_DEBUG_SUB68K).sp == 0xffa &&
            (reg(irq, KR_DEBUG_SUB68K, "SR") & 0x2700) == 0x2200 &&
            irq.stack.startsWith(QByteArray::fromHex("2000")),
            "Sega CD IRQ2 stops on the separate S68K exception stack");
    command(host, DebuggerCommand::StepInto, KR_DEBUG_SUB68K);
    timeline.stop(host, KR_DEBUG_SUB68K, KR_DEBUG_EXEC, subHandler + 8);
    command(host, DebuggerCommand::StepInto, KR_DEBUG_SUB68K);
    timeline.stop(host, KR_DEBUG_SUB68K, KR_DEBUG_EXEC, subHandler + 16);
    const auto subTrace = host.traceSnapshot();
    require(traced(subTrace, KR_DEBUG_SUB68K, KR_TRACE_EXEC, subHandler, 0x33fc) &&
            traced(subTrace, KR_DEBUG_SUB68K, KR_TRACE_DISC_COMMAND, 0xff804a, 0),
            "Sega CD traces the sub-CPU's real opcode and committed CDD status command");
    trace(host, false);
    command(host, DebuggerCommand::Continue, KR_DEBUG_SUB68K);
    const auto ack = timeline.stop(host, KR_DEBUG_M68K, KR_DEBUG_WRITE, mdMarker);
    require(ack.value == 1 && ack.accessSize == 2, "Main 68000 sees exactly one S68K IRQ acknowledgement");
    require(covered(host, "CD Program RAM", subHandler, 16, KR_COV_EXEC) &&
            covered(host, "CD Word RAM", 2, 2, KR_COV_READ) &&
            covered(host, "CD Word RAM", 6, 2, KR_COV_READ) &&
            covered(host, "CD Word RAM", 10, 1, KR_COV_READ) &&
            !covered(host, "CD Word RAM", 11, 1, KR_COV_READ),
            "CD coverage normalizes 1M/2M Word RAM, including the dot view, and actual PRG RAM code");
    require(!host.coverageSnapshot().canNavigate,
            "Physical Word RAM offsets do not navigate to the differently arranged 1M view");
    coverage(host, false);
    command(host, DebuggerCommand::Continue, KR_DEBUG_M68K);
    timeline.finish(host);
}

void test32x(const QString& core, const QTemporaryDir& temp, const QString& diagnostics = {}) {
    const auto path = temp.filePath("original-two-sh2.32x");
    const auto data = temp.filePath("x32");
    writeRom(path, x32Rom());
    Options options;
    options.cd = ExpansionMode::Disconnected;
    options.x32 = ExpansionMode::Connected;
    Timeline timeline(core, path, data, options, 8, diagnostics);
    LibretroHost host(core, data + "/debug");
    setup(host, path, data + "/debug", options);

    // Both SH-2s can enter the program while the selected debugger target
    // is stopped. Keep both under observation from the first frame.
    coverage(host, true);
    struct Part { unsigned cpu; quint32 entry, marker, value; };
    constexpr Part parts[] = {
        {KR_DEBUG_SH2_MASTER, sh2Master, 0x06000500, 0x12},
        {KR_DEBUG_SH2_SLAVE, sh2Slave, 0x06000502, 0x23}
    };
    for (const auto& part : parts) {
        const auto branch = part.entry + 4;
        const auto delaySlot = part.entry + 6;
        const auto destination = part.entry + 10;
        trace(host, true, part.cpu);
        command(host, DebuggerCommand::Configure, part.cpu, {
            point(1, part.cpu, KR_DEBUG_EXEC, branch),
            point(2, part.cpu, KR_DEBUG_WRITE, part.marker),
        });
        command(host, DebuggerCommand::Continue, part.cpu);
        const auto branchSnapshot = timeline.stop(host, part.cpu, KR_DEBUG_EXEC, branch);
        const auto chip = target(branchSnapshot, part.cpu);
        require(chip.address_bits == 32 && !branchSnapshot.instructions.isEmpty() &&
                (branchSnapshot.instructions.front().flags & KR_DEBUG_DELAYED),
                "SH-2 disassembly identifies a delayed branch");

        command(host, DebuggerCommand::StepInto, part.cpu);
        auto stepped = timeline.stop(host, part.cpu, KR_DEBUG_EXEC, delaySlot, destination);
        if (stepped.address == delaySlot) {
            command(host, DebuggerCommand::StepInto, part.cpu);
            stepped = timeline.stop(host, part.cpu, KR_DEBUG_EXEC, destination);
        }
        require(reg(stepped, part.cpu, "R0") == part.value,
                "Branch executes the delay slot once and skips the fall-through instruction");
        const auto sh2Trace = host.traceSnapshot();
        require(traced(sh2Trace, part.cpu, KR_TRACE_EXEC, branch, 0xa001) &&
                traced(sh2Trace, part.cpu, KR_TRACE_EXEC, delaySlot, 0x7001),
                "Each SH-2 trace retains the actual branch and its separate delay-slot opcode");
        const unsigned physical = part.entry - 0x06000000;
        require(covered(host, "32X SDRAM", physical, 8, KR_COV_EXEC) &&
                covered(host, "32X SDRAM", physical + 0x14, 4, KR_COV_READ) &&
                !covered(host, "32X SDRAM", physical + 8, 1, KR_COV_EXEC),
                "Each SH-2 marks executed SDRAM bytes and the actual PC-relative data load");
        trace(host, false);
        command(host, DebuggerCommand::Continue, part.cpu);
        const auto marker = timeline.stop(host, part.cpu, KR_DEBUG_WRITE, part.marker);
        require(marker.accessSize == 1 && marker.value == part.value,
                "Each SH-2 writes its own post-delay-slot value");

        const auto call = part.entry + 0x0c;
        const auto callSlot = part.entry + 0x0e;
        const auto returning = part.entry + 0x10;
        const auto callee = part.entry + 0x20;
        constexpr unsigned slotContribution = 2 + 4 + 16 + 8;
        const auto stopAtCall = [&](unsigned id) {
            command(host, DebuggerCommand::Configure, part.cpu, {point(id, part.cpu, KR_DEBUG_EXEC, call)});
            command(host, DebuggerCommand::Continue, part.cpu);
            return timeline.stop(host, part.cpu, KR_DEBUG_EXEC, call);
        };

        const auto beforeOver = stopAtCall(3);
        require(!beforeOver.instructions.isEmpty() &&
                (beforeOver.instructions.front().flags & (KR_DEBUG_CALL | KR_DEBUG_DELAYED)) ==
                    (KR_DEBUG_CALL | KR_DEBUG_DELAYED), "SH-2 disassembly identifies a delayed subroutine call");
        const auto beforeSp = cpu(beforeOver, part.cpu).sp;
        command(host, DebuggerCommand::StepOver, part.cpu);
        const auto over = timeline.stop(host, part.cpu, KR_DEBUG_EXEC, returning);
        require(reg(over, part.cpu, "R0") == part.value + slotContribution &&
                reg(over, part.cpu, "PR") == returning && cpu(over, part.cpu).sp == beforeSp,
                "Step over crosses a nested BSR and returns after both delayed calls and returns");

        const auto beforeInto = stopAtCall(4);
        command(host, DebuggerCommand::StepInto, part.cpu);
        auto entered = timeline.stop(host, part.cpu, KR_DEBUG_EXEC, callSlot, callee);
        if (entered.address == callSlot) {
            command(host, DebuggerCommand::StepInto, part.cpu);
            entered = timeline.stop(host, part.cpu, KR_DEBUG_EXEC, callee);
        }
        require(reg(entered, part.cpu, "R0") == part.value + 2 &&
                reg(entered, part.cpu, "PR") == returning, "Step into executes the BSR slot before entering the callee");
        command(host, DebuggerCommand::StepOut, part.cpu);
        const auto out = timeline.stop(host, part.cpu, KR_DEBUG_EXEC, returning);
        require(reg(out, part.cpu, "R0") == part.value + slotContribution &&
                reg(out, part.cpu, "PR") == returning &&
                cpu(out, part.cpu).sp == cpu(beforeInto, part.cpu).sp,
                "Step out passes the nested RTS and stops after the outer return slot");
    }
    coverage(host, false);
    timeline.detach(host); // Complete and compare the pending frame before returning to plain execution.
    const auto postDebugger = host.captureRewindState();
    const auto savedFrame = host.runClock().frames;
    require(!postDebugger.isEmpty(), "Capture 32X state after detaching from a suspended frame");
    timeline.finish(host);
    timeline.replay(host, postDebugger, savedFrame);
}

DebuggerCommand registerEdit(const QString& name, quint32 value, quint32 mask = 0xffffffffu) {
    DebuggerCommand request;
    request.kind = DebuggerCommand::WriteRegister;
    request.cpu = KR_DEBUG_M68K;
    request.registerName = name;
    request.registerValue = value;
    request.registerMask = mask;
    return request;
}

DebuggerSnapshot registerStop(LibretroHost& host, unsigned event, quint32 address) {
    const auto clock = host.runClock();
    const auto picture = host.frame();
    require(!host.runDebugFrame(), "Register edit fixture stops within the first frame");
    const auto result = host.debuggerSnapshot();
    require(result.stopped && result.partialFrame && result.eventCpu == KR_DEBUG_M68K &&
            result.event == event && result.address == address && result.frame == clock.frames &&
            host.runClock().ticks == clock.ticks && host.frame() == picture && host.audio().isEmpty(),
            "Register edit fixture keeps the completed output boundary");
    return result;
}

void testRegisterEdits(const QString& core, const QTemporaryDir& temp, const RegisterProgram& program,
                       const QString& path) {
    for (bool editPc : {false, true}) {
        const auto data = temp.filePath(editPc ? "register-pc" : "register-flags");
        LibretroHost host(core, data);
        setup(host, path, data, {});
        command(host, DebuggerCommand::Configure, KR_DEBUG_M68K, {
            point(1, KR_DEBUG_M68K, KR_DEBUG_EXEC, program.conditional),
            point(2, KR_DEBUG_M68K, KR_DEBUG_WRITE, mdMarker),
        });
        const auto before = registerStop(host, KR_DEBUG_EXEC, program.conditional);
        const auto initialSr = reg(before, KR_DEBUG_M68K, "SR");
        require(before.canEdit && (initialSr & 4) == 0, "Stopped 68000 exposes an editable clear Z flag");
        const auto request = editPc ? registerEdit("PC", program.redirected) : registerEdit("SR", 4, 4);
        require(before.writableRegisters.contains(request.registerName),
                "68000 advertises its writable register");
        QString error;
        const bool edited = host.debuggerCommand(request, error);
        require(edited, QStringLiteral("Edit stopped 68000 register — %1").arg(error));
        const auto after = host.debuggerSnapshot();
        const auto state = cpu(after, KR_DEBUG_M68K);
        require(after.partialFrame && after.stopped && after.frame == before.frame &&
                state.pc == (editPc ? program.redirected : program.conditional) &&
                reg(after, KR_DEBUG_M68K, "SR") == (editPc ? initialSr : initialSr | 4u),
                "PC edit redirects the trace and masked Z edit preserves unrelated SR bits");

        command(host, DebuggerCommand::Continue, KR_DEBUG_M68K);
        const auto marker = registerStop(host, KR_DEBUG_WRITE, mdMarker);
        require(marker.value == (editPc ? 0x67u : 0x38u) && marker.accessSize == 1,
                "Next real instruction follows the edited PC or Z flag");
        require(!marker.canEdit &&
                !host.debuggerCommand(registerEdit("D0", 0x55), error) && !error.isEmpty() &&
                reg(host.debuggerSnapshot(), KR_DEBUG_M68K, "D0") == reg(marker, KR_DEBUG_M68K, "D0"),
                "Data-access stop rejects register writes without changing the CPU");
        command(host, DebuggerCommand::Continue, KR_DEBUG_M68K);
        require(host.runDebugFrame() && host.runClock().frames == 1 && !host.audio().isEmpty(),
                "Register change completes one frame with its PCM");
    }
}

template <typename Condition>
void waitFor(Condition complete, const QString& detail) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        if (complete()) return;
        QThread::msleep(2);
    } while (timer.elapsed() < 5000);
    require(false, detail);
}

void testRecordingEditPolicy(const QString& core, const QTemporaryDir& temp, const RegisterProgram& program,
                             const QString& path) {
    bool paused = false, loaded = false;
    int movieMode = MovieSession::Off;
    QString error;
    std::atomic_bool pauseOnFrame{true};
    EmulationSession session(core, temp.filePath("edit-worker"));
    QObject::connect(&session, &EmulationSession::pausedChanged, &session, [&](bool value) { paused = value; });
    QObject::connect(&session, &EmulationSession::gameLoaded, &session, [&] { loaded = true; });
    QObject::connect(&session, &EmulationSession::errorOccurred, &session, [&](const QString& value) { error = value; });
    QObject::connect(&session, &EmulationSession::movieStatusChanged, &session,
                     [&](int mode, const QString&, quint64, quint64, double) { movieMode = mode; });
    QObject::connect(&session, &EmulationSession::videoFrame, &session, [&](const QImage&, double) {
        if (pauseOnFrame.exchange(false)) session.setPaused(true);
    }, Qt::DirectConnection);
    session.loadGame(path);
    waitFor([&] { return (loaded && paused) || !error.isEmpty(); }, "Pause original register fixture");
    require(error.isEmpty(), error);

    DebuggerCommand settings;
    settings.kind = DebuggerCommand::Configure;
    settings.cpu = KR_DEBUG_M68K;
    settings.breakpoints = {point(1, KR_DEBUG_M68K, KR_DEBUG_EXEC, program.tail, false)};
    session.debuggerCommand(settings);
    DebuggerSnapshot snapshot;
    waitFor([&] { return session.takeDebuggerSnapshot(snapshot) && snapshot.attached; },
            "Arm a breakpoint before starting input recording");
    const auto movie = temp.filePath("register-policy.krm");
    session.startMovieRecording(movie);
    waitFor([&] {
        return !error.isEmpty() || (session.takeDebuggerSnapshot(snapshot) && snapshot.partialFrame &&
                                   snapshot.stopped && snapshot.address == program.tail);
    }, "Stop at opcode during the first recorded frame");
    require(error.isEmpty() && movieMode == MovieSession::Recording && !snapshot.canEdit,
            "Paused recording allows reading the CPU and blocks register edits");
    const auto before = reg(snapshot, KR_DEBUG_M68K, "D0");
    session.debuggerCommand(registerEdit("D0", before ^ 0xff));
    waitFor([&] { return session.takeDebuggerSnapshot(snapshot) && !snapshot.status.isEmpty(); },
            "Worker rejects a register edit during recording");
    require(snapshot.partialFrame && snapshot.stopped && !snapshot.canEdit &&
            reg(snapshot, KR_DEBUG_M68K, "D0") == before, "Rejected recording edit preserves suspended registers");
    DebugTraceCommand traceRequest;
    traceRequest.kind=DebugTraceCommand::Start;
    traceRequest.config.cpu=KR_DEBUG_M68K;
    traceRequest.config.filters=KR_TRACE_INSTRUCTION;
    traceRequest.config.memoryMiB=1;
    session.traceCommand(traceRequest);
    DebugTraceSnapshot traceSnapshot;
    waitFor([&] { return session.takeTraceSnapshot(traceSnapshot) && traceSnapshot.active; },
            "Observation trace is allowed during recorded input at a suspended opcode");
    session.stopMovie();
    waitFor([&] { return movieMode == MovieSession::Off || !error.isEmpty(); },
            "Finish recording after rejecting its register edit");
    require(error.isEmpty(), error);
    traceRequest.kind=DebugTraceCommand::Refresh;
    session.traceCommand(traceRequest);
    waitFor([&] { return session.takeTraceSnapshot(traceSnapshot) && traceSnapshot.total; },
            "Completing the recorded frame produces a trace without extra guest frames");
    const auto exported=temp.filePath("worker-trace.tsv");
    traceRequest.kind=DebugTraceCommand::Export;traceRequest.path=exported;
    session.traceCommand(traceRequest);
    traceRequest.kind=DebugTraceCommand::Stop;session.traceCommand(traceRequest);
    waitFor([&] {
        if(session.takeTraceSnapshot(traceSnapshot) && !traceSnapshot.active && !traceSnapshot.writing) {
            QFile result(exported);
            if(result.open(QIODevice::ReadOnly) && result.readAll().endsWith("# complete\n")) return true;
        }
        DebugTraceCommand poll;poll.kind=DebugTraceCommand::Refresh;session.traceCommand(poll);
        return false;
    }, "Closing immediately after Export preserves the queued file job");
    session.shutdown();
    MovieTape tape;
    const bool opened = tape.load(movie, error);
    require(opened && tape.size() == 1 && tape.resumeFrame() == 1,
            QStringLiteral("Recording retains one completed frame — %1").arg(error));
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const bool only32x = args.size() >= 3 && args[2] == "--32x";
    const bool onlyCd = args.size() >= 3 && args[2] == "--cd";
    if (args.size() < 2 || args.size() > ((only32x || onlyCd) ? 4 : 3) ||
        (onlyCd && args.size() != 4)) {
        std::fprintf(stderr,
            "Usage: debug_expansion_tests CORE [SEGA_CD_BIOS_DIRECTORY | --cd BIOS_DIRECTORY | --32x [DIAGNOSTICS_DIRECTORY]]\n");
        return 2;
    }
    try {
        QTemporaryDir temp;
        require(temp.isValid(), "Create temporary original ROM storage");
        if (only32x) {
            test32x(args[1], temp, args.value(3));
            std::puts("PASS: both original 32X SH-2 programs step through delay slots and nested calls, then resume exactly");
            return 0;
        }
        if (onlyCd) {
            testCd(args[1], temp, args[3]);
            std::puts("PASS: original Sega CD Mode 1 guest, crossed IRQ2 and precise resume");
            return 0;
        }
        const auto program = registerRom();
        const auto registerPath = temp.filePath("original-register-edits.md");
        writeRom(registerPath, program.rom);
        testRegisterEdits(args[1], temp, program, registerPath);
        std::puts("PASS: paused CPU flags and PC edits change execution; post-access edits are rejected");
        testRecordingEditPolicy(args[1], temp, program, registerPath);
        std::puts("PASS: recording worker rejects register edits and commits one complete movie frame");
        testSvp(args[1], temp);
        std::puts("PASS: original SSP1601 cartridge traps real data accesses and resumes exactly");
        testLink(args[1], temp, false);
        std::puts("PASS: connected Mega Drive B breakpoint, paired outputs and peer release");
        testLink(args[1], temp, true);
        std::puts("PASS: connected Game Gear B breakpoint, paired outputs and peer release");
        test32x(args[1], temp);
        std::puts("PASS: both original 32X SH-2 programs step through delay slots and nested calls, then resume exactly");
        if (args.size() == 3) {
            testCd(args[1], temp, args[2]);
            std::puts("PASS: original Sega CD Mode 1 guest, crossed IRQ2 and precise resume");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
