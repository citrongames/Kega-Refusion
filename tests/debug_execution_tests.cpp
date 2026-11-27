#include "core/LibretroHost.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>

namespace {

void require(bool condition, const QString& detail) {
    if (!condition) throw std::runtime_error(detail.toStdString());
}

struct Program {
    QByteArray rom;
    unsigned cpu = 0;
    unsigned hardware = 0;
    quint32 ram = 0;
    quint32 prefix = 0;
    quint32 afterPrefix = 0;
    quint32 firstCall = 0;
    quint32 firstReturn = 0;
    quint32 secondCall = 0;
    quint32 secondReturn = 0;
    quint32 subroutine = 0;
    quint32 loop = 0;
};

Program makeMdProgram() {
    Program fixture;
    fixture.cpu = KR_DEBUG_M68K;
    fixture.hardware = KR_HW_MD;
    fixture.ram = 0xff0020;
    fixture.subroutine = 0x300;
    auto& rom = fixture.rom;
    rom = QByteArray(64 * 1024, char(0xff));
    qToBigEndian<quint32>(0x00fffff0, rom.data());
    qToBigEndian<quint32>(0x00000200, rom.data() + 4);
    rom.replace(0x100, 16, "SEGA GENESIS    ");
    rom.replace(0x120, 16, "DEBUGGER FIXTURE");
    rom.replace(0x180, 2, "GM");
    qToBigEndian<quint32>(quint32(rom.size() - 1), rom.data() + 0x1a4);
    rom[0x1f0] = 'U';

    quint32 pc = 0x200;
    const auto word = [&](quint16 value) {
        qToBigEndian(value, rom.data() + pc);
        pc += 2;
    };
    const auto longWord = [&](quint32 value) {
        word(quint16(value >> 16)); word(quint16(value));
    };
    const auto moveWord = [&](quint16 value, quint32 address) {
        word(0x33fc); word(value); longWord(address); // MOVE.W #value,(address).L
    };
    word(0x46fc); word(0x2700); // Mask interrupts for this instruction fixture.
    word(0x7045);              // MOVEQ #$45,D0
    word(0x13c0); longWord(fixture.ram); // MOVE.B D0,(address).L
    word(0x1239); longWord(fixture.ram); // MOVE.B (address).L,D1
    fixture.firstCall = pc;
    word(0x4eb9); longWord(fixture.subroutine); // JSR (address).L
    fixture.firstReturn = pc;
    word(0x5200); // ADDQ.B #1,D0
    fixture.secondCall = pc;
    word(0x4eb9); longWord(fixture.subroutine);
    fixture.secondReturn = pc;

    moveWord(0x8004, 0xc00004);
    moveWord(0x8144, 0xc00004);
    word(0x23fc); longWord(0xc0000000); longWord(0xc00004); // Select CRAM backdrop.
    moveWord(0x000e, 0xc00000);
    for (quint16 data : {0x8d, 0x12, 0x93}) {
        word(0x13fc); word(data); longWord(0xc00011); // MOVE.B #data,(PSG).L
    }
    fixture.loop = pc;
    word(0x60fe); // BRA.S *

    pc = fixture.subroutine;
    word(0x5201); // ADDQ.B #1,D1
    word(0x4e75); // RTS
    return fixture;
}

constexpr quint32 dmaControl = 0xc00004;
constexpr quint32 dmaRomAddress = 0x4000;
constexpr quint32 dmaWords = 4096;
constexpr quint32 irqHandler = 0x300;

Program makeDmaIrqProgram() {
    Program fixture = makeMdProgram();
    auto& rom = fixture.rom;
    qToBigEndian(irqHandler, rom.data() + 0x78); // 68000 level-6 autovector.
    for (quint32 i = 0; i < 2 * dmaWords; ++i)
        rom[dmaRomAddress + i] = char((7 * i + 0x37) & 0xff);

    quint32 pc = 0x200;
    const auto word = [&](quint16 value) {
        qToBigEndian(value, rom.data() + pc);
        pc += 2;
    };
    const auto address = [&](quint32 value) {
        word(quint16(value >> 16)); word(quint16(value));
    };
    const auto moveWord = [&](quint16 value, quint32 target) {
        word(0x33fc); word(value); address(target);
    };

    word(0x46fc); word(0x2700); // Supervisor, mask IRQ during setup.
    moveWord(0, fixture.ram);
    // H32 Mode 5 display and DMA, VINT disabled until after DMA completes.
    // Transfer 4,096 words from cartridge $4000 to VRAM $0000.
    for (quint16 value : {0x8004, 0x8154, 0x8c00, 0x8f02, 0x9300, 0x9410,
                          0x9500, 0x9620, 0x9700, 0x4000, 0x0080})
        moveWord(value, dmaControl);
    for (quint16 data : {0x8d, 0x12, 0x93}) {
        word(0x13fc); word(data); address(0xc00011);
    }
    moveWord(0x8174, dmaControl); // Enable VINT.
    word(0x46fc); word(0x2000);   // Unmask IRQ while retaining supervisor mode.
    fixture.loop = pc;
    word(0x60fe);                 // BRA.S *

    pc = irqHandler;
    word(0x5279); address(fixture.ram); // ADDQ.W #1,(work RAM).L
    word(0x4e73);                      // RTE
    return fixture;
}

Program makeZ80Program(unsigned hardware) {
    Program fixture;
    fixture.cpu = KR_DEBUG_Z80;
    fixture.hardware = hardware;
    fixture.ram = 0xc020;
    fixture.subroutine = 0x100;
    auto& rom = fixture.rom;
    rom = QByteArray(32 * 1024, char(0));
    quint32 pc = 0;
    const auto emitBytes = [&](std::initializer_list<unsigned> bytes) {
        for (auto value : bytes) rom[pc++] = char(value);
    };
    const auto portWrite = [&](unsigned port, unsigned value) {
        emitBytes({0x3e, value, 0xd3, port}); // LD A,value; OUT (port),A
    };
    emitBytes({0xf3, 0x31, 0xf0, 0xc3, 0x06, 0, 0x3e, 0x45}); // DI; LD SP,$C3F0; LD B,0; LD A,$45
    emitBytes({0x32, 0x20, 0xc0, 0x3a, 0x20, 0xc0}); // LD ($C020),A; LD A,($C020)
    emitBytes({0xdd, 0x21, 0x1b, 0xc0}); // LD IX,$C01B
    fixture.prefix = pc;
    emitBytes({0xdd, 0xcb, 0x05, 0x46}); // BIT 0,(IX+5)
    fixture.afterPrefix = pc;
    fixture.firstCall = pc;
    emitBytes({0xcd, 0x00, 0x01}); // CALL $0100
    fixture.firstReturn = pc;
    emitBytes({0x3c}); // INC A
    fixture.secondCall = pc;
    emitBytes({0xcd, 0x00, 0x01});
    fixture.secondReturn = pc;

    const bool sg = hardware == KR_HW_SG;
    const bool gg = hardware == KR_HW_GG;
    portWrite(0xbf, sg ? 0 : 4); portWrite(0xbf, 0x80);
    portWrite(0xbf, sg ? 0xc0 : 0x40); portWrite(0xbf, 0x81);
    portWrite(0xbf, sg ? 3 : 0); portWrite(0xbf, 0x87);
    if (!sg) {
        portWrite(0xbf, gg ? 32 : 16); portWrite(0xbf, 0xc0);
        portWrite(0xbe, 3); // Set the backdrop palette entry.
        if (gg) portWrite(0xbe, 0);
    }
    for (unsigned data : {0x8d, 0x12, 0x93}) portWrite(0x7f, data);
    fixture.loop = pc;
    emitBytes({0x18, 0xfe}); // JR *

    pc = fixture.subroutine;
    emitBytes({0x04, 0xc9}); // INC B; RET
    if (!sg) {
        rom.replace(0x7ff0, 8, "TMR SEGA");
        rom[0x7fff] = char(gg ? 0x6c : 0x4c);
    }
    return fixture;
}

void writeFixture(const QString& path, const QByteArray& data) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(data) == data.size(),
            QStringLiteral("Create original ROM fixture"));
}

struct Frame {
    QByteArray state;
    QImage video;
    QByteArray pcm;
    LibretroHost::RunClock clock;
};

Frame completedFrame(const LibretroHost& host) {
    return {host.serialize(), host.frame(), host.audio(), host.runClock()};
}

DebuggerBreakpoint breakpoint(unsigned id, unsigned cpu, unsigned event, quint32 address,
                              const QString& condition = {}, bool oneShot = true, quint64 ignore = 0) {
    DebuggerBreakpoint result;
    result.id = id;
    result.cpu = cpu;
    result.events = event;
    result.first = result.last = address;
    result.condition = condition;
    result.oneShot = oneShot;
    result.ignoreHits = ignore;
    return result;
}

void command(LibretroHost& host, DebuggerCommand::Kind kind, unsigned cpu,
             const QVector<DebuggerBreakpoint>& breakpoints = {}) {
    DebuggerCommand request;
    request.kind = kind;
    request.cpu = cpu;
    request.breakpoints = breakpoints;
    QString error;
    const bool accepted = host.debuggerCommand(request, error);
    require(accepted, QStringLiteral("Debugger command %1: %2").arg(unsigned(kind)).arg(error));
}

const kr_debug_cpu& cpuState(const DebuggerSnapshot& snapshot, unsigned cpu) {
    const auto found = std::find_if(snapshot.cpus.begin(), snapshot.cpus.end(),
                                    [cpu](const auto& state) { return state.cpu == cpu; });
    require(found != snapshot.cpus.end(), "Expected CPU in debugger snapshot");
    return *found;
}

quint32 reg(const DebuggerSnapshot& snapshot, unsigned cpu, const char* name) {
    const auto& state = cpuState(snapshot, cpu);
    const auto* end = state.regs + state.count;
    const auto* value = std::find_if(state.regs, end, [&](const auto& item) {
        return std::strcmp(item.name, name) == 0;
    });
    require(value != end, QStringLiteral("Missing CPU register %1").arg(QString::fromLatin1(name)));
    return value->value;
}

const DebuggerBreakpoint& findBreakpoint(const DebuggerSnapshot& snapshot, unsigned id) {
    const auto found = std::find_if(snapshot.breakpoints.begin(), snapshot.breakpoints.end(),
                                    [id](const auto& item) { return item.id == id; });
    require(found != snapshot.breakpoints.end(), "Expected breakpoint in snapshot");
    return *found;
}

DebuggerSnapshot expectStop(LibretroHost& host, const Program& program, unsigned event, quint32 address) {
    const auto priorClock = host.runClock();
    const QImage priorFrame = host.frame();
    require(!host.runDebugFrame(), "Expected CPU stop before frame completion");
    const auto state = host.debuggerSnapshot();
    require(host.debuggerStopped() && host.debuggerFramePending() && state.attached &&
            state.stopped && state.partialFrame && state.selectedCpu == program.cpu &&
            state.event == event && state.address == address,
            QStringLiteral("Stop expected event %1 at %2, got event %3 at %4")
                .arg(event).arg(address, 0, 16).arg(state.event).arg(state.address, 0, 16));
    require(state.frame == priorClock.frames && host.runClock().frames == priorClock.frames &&
            host.runClock().ticks == priorClock.ticks && host.frame() == priorFrame && host.audio().isEmpty(),
            "Partial frame changed clock or published output");
    if (event == KR_DEBUG_EXEC) {
        require(cpuState(state, program.cpu).pc == address && state.accessSize == 0 &&
                !state.instructions.isEmpty() && state.instructions.front().address == address,
                "Execution stop must point to the current opcode");
    }
    return state;
}

using Peek = size_t (*)(unsigned, uint32_t, uint8_t*, uint8_t*, size_t);

void checkPeek(Peek peek, const Program& program) {
    std::array<uint8_t, 4> bytes{}, valid{};
    require(peek(program.cpu, program.ram, bytes.data(), valid.data(), 1) == 1 &&
            bytes.front() == 0x45 && valid.front(), "Read-only peek sees the completed CPU write");
    if (program.cpu == KR_DEBUG_M68K) {
        bytes.fill(0xff);
        valid.fill(0xff);
        require(peek(program.cpu, 0xc00004, bytes.data(), valid.data(), bytes.size()) == bytes.size() &&
                std::all_of(bytes.begin(), bytes.end(), [](auto byte) { return !byte; }) &&
                std::all_of(valid.begin(), valid.end(), [](auto byte) { return !byte; }),
                "Live VDP ports must be unavailable to side-effect-free peek");
    }
    const quint32 outside = program.cpu == KR_DEBUG_M68K ? 0x1000000 : 0x10000;
    require(peek(program.cpu, outside, bytes.data(), valid.data(), 1) == 0 &&
            peek(program.cpu, 0, nullptr, valid.data(), 1) == 0,
            "Peek rejects invalid address and destination");
    std::array<uint8_t, KR_DEBUG_MAX_PEEK + 1> bulk{}, validity{};
    bulk.back() = validity.back() = 0x7a;
    require(peek(program.cpu, 0, bulk.data(), validity.data(), bulk.size()) == KR_DEBUG_MAX_PEEK &&
            bulk.back() == 0x7a && validity.back() == 0x7a, "Peek caps the transfer");
}

void checkStops(LibretroHost& host, const Program& program, Peek peek) {
    const unsigned cpu = program.cpu;
    const bool md = cpu == KR_DEBUG_M68K;
    const QString callCondition = md ? "D0 & $ff == 0x45" : "AF & $ff00 == 0x4500";
    command(host, DebuggerCommand::Configure, cpu, {
        breakpoint(1, cpu, KR_DEBUG_WRITE, program.ram, "VALUE & $f0 == 64"),
        breakpoint(2, cpu, KR_DEBUG_READ, program.ram, "SIZE == 1"),
        breakpoint(3, cpu, KR_DEBUG_READ, program.ram, "SIZE == 2"),
        breakpoint(4, cpu, KR_DEBUG_EXEC, md ? program.firstCall : program.prefix,
                   md ? callCondition : QStringLiteral("ADDR == $%1").arg(program.prefix, 0, 16))
    });

    const auto write = expectStop(host, program, KR_DEBUG_WRITE, program.ram);
    require(write.value == 0x45 && write.accessSize == 1 && findBreakpoint(write, 1).hits == 1 &&
            !findBreakpoint(write, 1).enabled, "Conditional one-shot write reports completed bus data");
    checkPeek(peek, program);

    command(host, DebuggerCommand::Continue, cpu);
    const auto read = expectStop(host, program, KR_DEBUG_READ, program.ram);
    require(read.value == 0x45 && read.accessSize == 1 && findBreakpoint(read, 2).hits == 1 &&
            !findBreakpoint(read, 2).enabled && findBreakpoint(read, 3).hits == 1 &&
            findBreakpoint(read, 3).enabled, "Read event and false condition keep independent hit state");

    command(host, DebuggerCommand::Continue, cpu);
    auto call = expectStop(host, program, KR_DEBUG_EXEC, md ? program.firstCall : program.prefix);
    if (!md) {
        const auto& instruction = call.instructions.front();
        require(instruction.valid && instruction.length == 4 &&
                QByteArray(reinterpret_cast<const char*>(instruction.bytes), 4) == QByteArray::fromHex("ddcb0546"),
                "Disassemble a four-byte Z80 indexed CB instruction");
        command(host, DebuggerCommand::StepInto, cpu);
        call = expectStop(host, program, KR_DEBUG_EXEC, program.afterPrefix);
    }
    require(call.instructions.front().valid && (call.instructions.front().flags & KR_DEBUG_CALL) &&
            call.instructions.front().length == program.firstReturn - program.firstCall,
            "Disassembly identifies the full subroutine call");
    const quint32 initialSp = cpuState(call, cpu).sp;
    const auto marker = [&](const DebuggerSnapshot& snapshot) {
        return md ? reg(snapshot, cpu, "D1") & 0xff : reg(snapshot, cpu, "BC") >> 8;
    };
    const quint32 initialMarker = marker(call);

    command(host, DebuggerCommand::StepOver, cpu);
    const auto afterCall = expectStop(host, program, KR_DEBUG_EXEC, program.firstReturn);
    require(cpuState(afterCall, cpu).sp == initialSp && marker(afterCall) == initialMarker + 1,
            "Step over runs the subroutine and restores stack depth");

    command(host, DebuggerCommand::Configure, cpu, {
        breakpoint(5, cpu, KR_DEBUG_EXEC, program.secondCall, md ? "D0 == 0x46" : "AF & $ff00 == 0x4600")
    });
    command(host, DebuggerCommand::Continue, cpu);
    expectStop(host, program, KR_DEBUG_EXEC, program.secondCall);
    command(host, DebuggerCommand::StepInto, cpu);
    const auto callee = expectStop(host, program, KR_DEBUG_EXEC, program.subroutine);
    const qsizetype returnBytes = md ? 4 : 2;
    QByteArray returnAddress(returnBytes, char(0));
    if (md) qToBigEndian(program.secondReturn, returnAddress.data());
    else qToLittleEndian(quint16(program.secondReturn), returnAddress.data());
    require(cpuState(callee, cpu).sp == initialSp - returnBytes && callee.stackAddress == cpuState(callee, cpu).sp &&
            callee.stack.left(returnBytes) == returnAddress && callee.stackValid.left(returnBytes) == QByteArray(returnBytes, 1),
            "Step into exposes the guest return address on its real stack");

    command(host, DebuggerCommand::StepOut, cpu);
    const auto afterReturn = expectStop(host, program, KR_DEBUG_EXEC, program.secondReturn);
    require(cpuState(afterReturn, cpu).sp == initialSp && marker(afterReturn) == initialMarker + 2,
            "Step out completes the return instruction");

    command(host, DebuggerCommand::Configure, cpu, {
        breakpoint(6, cpu, KR_DEBUG_EXEC, program.loop, "HIT >= 2", true, 1),
        breakpoint(7, cpu, KR_DEBUG_EXEC, program.loop, "HIT == 1", true, 1)
    });
    command(host, DebuggerCommand::Continue, cpu);
    const auto loop = expectStop(host, program, KR_DEBUG_EXEC, program.loop);
    require(findBreakpoint(loop, 6).hits == 2 && !findBreakpoint(loop, 6).enabled &&
            findBreakpoint(loop, 7).hits == 2 && findBreakpoint(loop, 7).enabled,
            "Ignored hit count precedes condition and one-shot evaluation");
    command(host, DebuggerCommand::StepInto, cpu);
    const auto stepped = expectStop(host, program, KR_DEBUG_EXEC, program.loop);
    require(cpuState(stepped, cpu).cycles > cpuState(loop, cpu).cycles &&
            findBreakpoint(stepped, 6).hits == 2, "Step executes a self-branch once");


    // The UI can submit stale copies of unrelated breakpoints while editing.
    DebuggerCommand stale;
    stale.kind = DebuggerCommand::Configure;
    stale.cpu = cpu;
    stale.breakpoints = stepped.breakpoints;
    stale.breakpoints[0].enabled = true;
    stale.breakpoints[1].enabled = false;
    for (auto& item : stale.breakpoints) item.hits = 0;
    QString editError;
    require(host.debuggerCommand(stale, editError), "Apply a stale breakpoint list");
    const auto preserved = host.debuggerSnapshot();
    require(!findBreakpoint(preserved, 6).enabled && findBreakpoint(preserved, 6).hits == 2 &&
            findBreakpoint(preserved, 7).enabled &&
            findBreakpoint(preserved, 7).hits == findBreakpoint(stepped, 7).hits,
            "Unedited breakpoint list preserves counters and one-shot auto-disable");
    stale.editedBreakpointId = 7;
    require(host.debuggerCommand(stale, editError), "Apply explicit enable toggle");
    const auto edited = host.debuggerSnapshot();
    require(!findBreakpoint(edited, 6).enabled && findBreakpoint(edited, 6).hits == 2 &&
            !findBreakpoint(edited, 7).enabled &&
            findBreakpoint(edited, 7).hits == findBreakpoint(stepped, 7).hits,
            "Enable toggle preserves the edited counter and all other runtime state");

    // Rejected edits leave the current breakpoint set and suspended frame intact.
    DebuggerCommand invalid;
    invalid.kind = DebuggerCommand::Configure;
    invalid.cpu = cpu;
    invalid.breakpoints = {breakpoint(8, cpu, KR_DEBUG_EXEC, program.loop, "UNKNOWN == 0")};
    QString error;
    require(!host.debuggerCommand(invalid, error) && !error.isEmpty() && host.debuggerFramePending() &&
            host.debuggerStopped() && host.debuggerSnapshot().breakpoints.size() == 2,
            "Reject an invalid condition without advancing the suspended frame");
    command(host, DebuggerCommand::Continue, cpu);
}

void checkLifecycle(LibretroHost& host, const Program& program) {
    const auto rewind = host.captureRewindState();
    const auto checkpoint = completedFrame(host);
    require(!rewind.isEmpty(), "Capture completed frame for later restore");
    const auto armLoop = [&] {
        command(host, DebuggerCommand::Configure, program.cpu,
                {breakpoint(10, program.cpu, KR_DEBUG_EXEC, program.loop, {}, false)});
        command(host, DebuggerCommand::Continue, program.cpu);
        expectStop(host, program, KR_DEBUG_EXEC, program.loop);
    };
    const auto finish = [&] {
        const auto clock = host.runClock();
        host.finishDebugFrame();
        require(!host.debuggerFramePending() && !host.debuggerStopped() &&
                host.runClock().frames == clock.frames + 1, "Finish drains an armed breakpoint exactly once");
        host.finishDebugFrame();
        require(host.runClock().frames == clock.frames + 1, "Finishing again cannot run an extra frame");
    };

    armLoop();
    finish();
    require(host.restoreRewindState(rewind) && host.serialize() == checkpoint.state &&
            host.runClock().frames == checkpoint.clock.frames && host.runClock().ticks == checkpoint.clock.ticks,
            "Restore completed rewind state after draining the prior frame");

    armLoop();
    finish();
    const auto priorGeneration = host.inspectionGeneration();
    host.reset();
    require(host.runClock().frames == 0 && !host.debuggerFramePending() &&
            host.inspectionGeneration() > priorGeneration, "Reset invalidates the completed frame's context");
    host.runFrame();
    require(host.runClock().frames == 1 && !host.debuggerFramePending(),
            "Ordinary frame entry bypasses stop handling");

    armLoop();
    const quint64 last = host.runClock().frames;
    host.debuggerDetach();
    require(!host.debuggerAttached() && !host.debuggerFramePending() &&
            host.runClock().frames == last + 1, "Detach drains the suspended frame");
    armLoop();
    finish();
    host.unload();
    require(!host.loaded() && !host.debuggerAttached() && !host.debuggerFramePending(),
            "Unload releases debugger context");
}

void checkProgram(const QString& core, const QString& path, const QString& sandbox, const Program& program) {
    writeFixture(path, program.rom);
    std::array<Frame, 5> reference;
    {
        LibretroHost host(core, sandbox + "/uninterrupted");
        QString error;
        const bool loaded = host.load(path, error);
        require(loaded, error);
        for (auto& frame : reference) {
            host.runFrame();
            frame = completedFrame(host);
            require(!frame.state.isEmpty() && !frame.video.isNull() && !frame.pcm.isEmpty(),
                    "Uninterrupted frame baseline includes native state, video and PCM");
        }
    }
    LibretroHost host(core, sandbox + "/debug");
    QString error;
    const bool loaded = host.load(path, error);
    require(loaded, error);
    QLibrary library(core);
    const auto peek = reinterpret_cast<Peek>(library.resolve("retro_kegarefusion_debug_peek"));
    require(peek != nullptr, "Locate native side-effect-free debug peek");
    DebugInspectionRequest hardwareRequest;
    hardwareRequest.parts = KR_INSPECT_HARDWARE;
    const auto inspected = host.inspect(hardwareRequest);
    require(inspected.available() && inspected.info.hardware == program.hardware,
            QStringLiteral("Fixture selected wrong hardware for %1 (reported %2, expected %3)")
                .arg(path, QString::number(inspected.info.hardware), QString::number(program.hardware)));
    const auto before = host.serialize();
    command(host, DebuggerCommand::Attach, program.cpu);
    const auto initial = host.debuggerSnapshot();
    require(initial.attached && !initial.stopped && !initial.partialFrame &&
            initial.cpus.size() == (program.cpu == KR_DEBUG_M68K ? 2 : 1) &&
            host.serialize() == before, "Attaching and reading CPU state preserves the machine");

    checkStops(host, program, peek);
    for (const auto& expected : reference) {
        require(host.runDebugFrame(), "All resumed frames complete after one-shot conditions");
        const auto actual = completedFrame(host);
        require(!host.debuggerFramePending() && actual.clock.frames == expected.clock.frames &&
                actual.clock.ticks == expected.clock.ticks && actual.state == expected.state &&
                actual.video == expected.video && actual.pcm == expected.pcm,
                QStringLiteral("State, video, PCM and clock match uninterrupted frame %1").arg(expected.clock.frames));
    }
    checkLifecycle(host, program);
}

void checkAccessObserver(const QString& core, const QString& path, const QString& sandbox) {
    const auto program = makeMdProgram();
    Frame reference;
    QString error;
    {
        LibretroHost host(core, sandbox + "/reference");
        require(host.load(path, error), error);
        host.runFrame();
        reference = completedFrame(host);
    }

    LibretroHost host(core, sandbox + "/observed");
    require(host.load(path, error), error);
    struct Capture {
        LibretroHost& host;
        QVector<DebugAccessEvent> events;
        quint8 byte = 0;
        bool guarded = false;
    } capture{host, {}};
    const auto observe = +[](void* opaque, const DebugAccessEvent& event) {
        auto& result = *static_cast<Capture*>(opaque);
        result.events.append(event);
        if (event.event == KR_DEBUG_WRITE) {
            const quint8 replacement = 0x88;
            result.guarded = result.host.inspectionRead(event.instance, KR_MEM_RAM, 0x20, &result.byte, 1) &&
                !result.host.inspectionWrite(event.instance, KR_MEM_RAM, 0x20, &replacement, 1) &&
                !result.host.inspectionWriteRegister(event.instance, event.cpu, "D0", 0x88) &&
                !result.host.setDebugAccessObserver({});
        }
    };
    const QVector<DebugAccessSubscription> subscriptions{
        {0, program.cpu, KR_DEBUG_EXEC, program.firstCall, program.firstCall},
        {0, program.cpu, KR_DEBUG_READ, program.ram, program.ram},
        {0, program.cpu, KR_DEBUG_WRITE, program.ram - 1, program.ram},
        {0, program.cpu, KR_DEBUG_WRITE, program.ram, program.ram + 1}
    };

    require(host.setDebugAccessObserver(subscriptions, observe, &capture) && !host.debuggerAttached(),
            "Observation does not attach the debugger");
    require(!host.setDebugAccessObserver({{0, program.cpu, KR_DEBUG_READ, 1, 0}}, observe, &capture),
            "A rejected range leaves the running observer in place");
    require(host.runDebugFrame(), "Observation alone runs a complete frame");
    require(capture.events.size() == 3 && capture.events[0].event == KR_DEBUG_WRITE &&
            capture.events[0].address == program.ram && capture.events[0].value == 0x45 &&
            capture.events[0].size == 1 && capture.guarded && capture.byte == 0x45 &&
            capture.events[1].event == KR_DEBUG_READ && capture.events[1].address == program.ram &&
            capture.events[1].value == 0x45 && capture.events[1].size == 1 &&
            capture.events[2].event == KR_DEBUG_EXEC && capture.events[2].address == program.firstCall &&
            capture.events[2].value == 0 && capture.events[2].size == 0,
            "Filtered write, read and pre-instruction events retain order and native values");
    const auto actual = completedFrame(host);
    require(actual.state == reference.state && actual.video == reference.video &&
            actual.pcm == reference.pcm && actual.clock.frames == reference.clock.frames &&
            actual.clock.ticks == reference.clock.ticks && !host.debuggerAttached(),
            "Observation and callback inspection preserve state, video, PCM and frame clock");

    capture.events.clear();
    host.reset();
    host.runRewindPreviewFrame();
    require(capture.events.isEmpty(), "Reset and hidden rewind previews do not dispatch observers");
    host.reset();
    command(host, DebuggerCommand::Configure, program.cpu, {
        breakpoint(1, program.cpu, KR_DEBUG_WRITE, program.ram)
    });
    expectStop(host, program, KR_DEBUG_WRITE, program.ram);
    require(capture.events.size() == 1 && capture.events.front().event == KR_DEBUG_WRITE,
            "The same access reaches the observer before the debugger suspends the frame");
    const quint8 edited = 0x91;
    require(!host.inspectionWrite(0, KR_MEM_RAM, 0x20, &edited, 1) &&
            !host.inspectionWriteRegister(0, program.cpu, "D0", edited),
            "A suspended frame rejects standalone inspection edits");
    require(host.setDebugAccessObserver({}) && host.debuggerFramePending(),
            "Clearing observers leaves the debugger stopped");
    command(host, DebuggerCommand::Continue, program.cpu);
    require(host.runDebugFrame() && capture.events.size() == 1,
            "The debugger resumes without dispatching the detached observer");

    capture.events.clear();
    host.reset();
    require(host.setDebugAccessObserver(subscriptions, observe, &capture), "Restore observer after detach");
    host.debuggerDetach();
    require(host.runDebugFrame() && capture.events.size() == 3 && !host.debuggerAttached(),
            "Detaching the debugger leaves the observer active");
    capture.events.clear();
    require(host.hardReset(error), error);
    require(capture.events.isEmpty() && host.runDebugFrame() && capture.events.size() == 3,
            "A power reset rebinds observers for the newly loaded core");

    capture.events.clear();
    host.unload();
    require(host.load(path, error), error);
    host.runFrame();
    require(capture.events.isEmpty(), "Unloading drops observer callbacks");

    const auto generation = host.inspectionGeneration();
    kr_cov_location location{};
    require(host.inspectionDomainWritable(0, KR_MEM_RAM) && !host.inspectionDomainWritable(0, KR_MEM_ROM) &&
            host.inspectionRegisterWritable(0, program.cpu, "D0") &&
            !host.inspectionRegisterWritable(0, program.cpu, "INVALID") &&
            host.inspectionCodeLocation(0, program.cpu, program.firstCall, location) &&
            location.source && location.offset == program.firstCall &&
            host.inspectionGeneration() == generation, "Discovery is independent of the debugger and edits");
    require(!host.inspectionWrite(0, KR_MEM_ROM, 0, &edited, 1) &&
            !host.inspectionWrite(0, KR_MEM_RAM, 0x10000, &edited, 1) &&
            !host.inspectionWriteRegister(0, program.cpu, "PC", 3) &&
            host.inspectionGeneration() == generation,
            "Invalid edits reject without advancing the inspection generation");
    quint8 actualByte = 0;
    kr_debug_cpu registers{};
    require(host.inspectionWrite(0, KR_MEM_RAM, 0x20, &edited, 1) &&
            host.inspectionRead(0, KR_MEM_RAM, 0x20, &actualByte, 1) && actualByte == edited &&
            host.inspectionWriteRegister(0, program.cpu, "D0", 0x12345678) &&
            host.inspectionRegisters(0, program.cpu, registers) &&
            std::any_of(registers.regs, registers.regs + registers.count, [](const kr_debug_reg& item) {
                return std::strcmp(item.name, "D0") == 0 && item.value == 0x12345678;
            }) &&
            host.inspectionGeneration() == generation + 2 && !host.debuggerAttached(),
            "Full-boundary RAM and register edits reach the machine without attaching a debugger");

    const QFileInfo coreFile(core);
    const auto peer = coreFile.path() + '/' + coreFile.completeBaseName() + "_linkpeer." + coreFile.suffix();
    if (!QFileInfo::exists(peer)) return;
    require(host.setLinkCableEnabled(true, error), error);
    require(host.setDebugAccessObserver({{1, program.cpu, KR_DEBUG_WRITE, dmaControl + 1, dmaControl + 1}},
                                        observe, &capture),
            "Subscribe only to a byte inside a linked peer's word accesses");
    host.reset();
    command(host, DebuggerCommand::Configure, program.cpu, {
        breakpoint(2, program.cpu, KR_DEBUG_WRITE, program.ram)
    });
    expectStop(host, program, KR_DEBUG_WRITE, program.ram);
    command(host, DebuggerCommand::Continue, program.cpu);
    require(host.runDebugFrame() && !capture.events.isEmpty() &&
            std::all_of(capture.events.cbegin(), capture.events.cend(), [&](const DebugAccessEvent& event) {
                return event.instance == 1 && event.cpu == program.cpu && event.event == KR_DEBUG_WRITE &&
                       event.address == dmaControl && event.size >= 2;
            }),
            "Peer word overlap is observed while a primary CPU breakpoint can suspend the frame");
    capture.events.clear();
    require(host.setLinkCableEnabled(false, error) && host.setLinkCableEnabled(true, error), error);
    require(host.inspectionInstanceCount() == 2 && host.runDebugFrame() && capture.events.isEmpty(),
            "Removing a peer disconnects its observer before a new peer is created");
}

void checkDmaAndIrq(const QString& core, const QString& path, const QString& sandbox) {
    const auto program = makeDmaIrqProgram();
    writeFixture(path, program.rom);
    std::array<Frame, 4> reference;
    {
        LibretroHost host(core, sandbox + "/uninterrupted");
        QString error;
        const bool loaded = host.load(path, error);
        require(loaded, error);
        for (auto& frame : reference) {
            host.runFrame();
            frame = completedFrame(host);
            require(!frame.state.isEmpty() && !frame.video.isNull() && !frame.pcm.isEmpty(),
                    "DMA/IRQ reference has full state, video and audio");
        }
    }

    LibretroHost host(core, sandbox + "/debug");
    QString error;
    const bool loaded = host.load(path, error);
    require(loaded, error);
    command(host, DebuggerCommand::Configure, program.cpu, {
        breakpoint(1, program.cpu, KR_DEBUG_WRITE, dmaControl, "VALUE == $80"),
        breakpoint(2, program.cpu, KR_DEBUG_EXEC, irqHandler)
    });
    const auto dma = expectStop(host, program, KR_DEBUG_WRITE, dmaControl);
    require(dma.value == 0x0080 && dma.accessSize == 2, "DMA stop follows the second control word");
    DebugInspectionRequest videoRequest;
    videoRequest.parts = KR_INSPECT_VIDEO;
    const auto video = host.inspect(videoRequest);
    const auto* values = video.info.values;
    const auto* valueEnd = values + video.info.value_count;
    const auto* remaining = std::find_if(values, valueEnd, [](const auto& value) {
        return std::strcmp(value.device, "VDP") == 0 && std::strcmp(value.name, "DMA remaining") == 0;
    });
    require(video.available() && remaining != valueEnd && remaining->value > 0 &&
            remaining->value <= dmaWords, "Control-port breakpoint suspended a live transfer");
    command(host, DebuggerCommand::Continue, program.cpu);

    bool sawIrq = false;
    for (const auto& expected : reference) {
        const auto before = host.runClock();
        if (!host.runDebugFrame()) {
            const auto irq = host.debuggerSnapshot();
            require(!sawIrq && irq.partialFrame && irq.stopped && irq.event == KR_DEBUG_EXEC &&
                    irq.address == irqHandler && irq.frame == before.frames &&
                    host.runClock().ticks == before.ticks && host.audio().isEmpty(),
                    "Suspend before the first instruction of the delivered VBlank interrupt");
            const auto& cpu = cpuState(irq, program.cpu);
            QByteArray exceptionFrame = QByteArray::fromHex("200000000000");
            qToBigEndian(program.loop, exceptionFrame.data() + 2);
            require(cpu.sp == 0xffffea && (reg(irq, program.cpu, "SR") & 0x2700) == 0x2600 &&
                    irq.stack.left(6) == exceptionFrame && irq.stackValid.left(6) == QByteArray(6, 1),
                    "Level-6 interrupt stack retains the interrupted PC and SR");
            sawIrq = true;
            command(host, DebuggerCommand::Continue, program.cpu);
            require(host.runDebugFrame(), "Complete the interrupted frame after VBlank stop");
        }
        const auto actual = completedFrame(host);
        require(actual.clock.frames == expected.clock.frames && actual.clock.ticks == expected.clock.ticks &&
                actual.state == expected.state && actual.video == expected.video && actual.pcm == expected.pcm,
                "DMA/IRQ stop and resume match uninterrupted state, frame and PCM");
    }
    require(sawIrq, "Guest received a VBlank interrupt in the reference window");

    DebugInspectionRequest memory;
    memory.parts = KR_INSPECT_MEMORY;
    memory.domain = KR_MEM_VRAM;
    memory.length = 8;
    require(host.inspect(memory).memory == program.rom.mid(dmaRomAddress, 8), "DMA writes its first VRAM bytes");
    memory.offset = 2 * dmaWords - memory.length;
    require(host.inspect(memory).memory == program.rom.mid(dmaRomAddress + memory.offset, memory.length),
            "DMA reaches its final destination bytes");
    memory.domain = KR_MEM_RAM;
    memory.offset = program.ram - 0xff0000;
    memory.length = 2;
    const auto irqCount = host.inspect(memory).memory;
    require(irqCount.size() == 2 && qFromBigEndian<quint16>(irqCount.constData()) > 0,
            "Guest VBlank handler executed and returned");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        std::fprintf(stderr, "Usage: debug_execution_tests CORE\n");
        return 2;
    }
    try {
        QTemporaryDir temp;
        require(temp.isValid(), "Create isolated fixture directory");
        const auto core = app.arguments().at(1);
        checkProgram(core, temp.filePath("original-debug.md"), temp.filePath("md"), makeMdProgram());
        std::puts("PASS: 68000 breakpoints, stepping, state/output equivalence and suspended-frame lifecycle");
        checkAccessObserver(core, temp.filePath("original-debug.md"), temp.filePath("access"));
        std::puts("PASS: filtered CPU observation, debugger coexistence and full-frame inspection edits");
        struct Z80Case { const char* suffix; const char* hardwareName; unsigned hardware; };
        constexpr Z80Case z80Cases[] = {
            {"sms", "Master System", KR_HW_SMS},
            {"gg", "Game Gear", KR_HW_GG},
            {"sg", "SG-1000", KR_HW_SG}
        };
        for (const auto& fixture : z80Cases) {
            const auto suffix = QString::fromLatin1(fixture.suffix);
            checkProgram(core, temp.filePath("original-debug." + suffix), temp.filePath(suffix),
                         makeZ80Program(fixture.hardware));
            std::printf("PASS: %s Z80 breakpoints, indexed-prefix stepping, state/output equivalence and lifecycle\n",
                        fixture.hardwareName);
        }
        checkDmaAndIrq(core, temp.filePath("original-dma-irq.md"), temp.filePath("dma-irq"));
        std::puts("PASS: live DMA watchpoint and delivered VBlank IRQ resume with exact state, video and audio");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
