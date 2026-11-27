#include "core/LibretroHost.h"
#include "core/MovieTape.h"
#include "script/ScriptRuntime.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool condition, const QString& message) {
    if (!condition) throw std::runtime_error(message.toStdString());
}

QString writeFixture(const QTemporaryDir& temp, const QString& name, const QByteArray& rom) {
    QFile file(temp.filePath(name));
    require(file.open(QIODevice::WriteOnly) && file.write(rom) == rom.size(), file.fileName());
    return file.fileName();
}

enum class MdKind { Basic, X32, Firecore };

// Original 68000 test program. Stationary sprite center (120, 80), half-size
// (12, 8), and signed 8.8 speed -0.5 in main RAM, written once before looping.
QByteArray mdFixture(MdKind kind = MdKind::Basic) {
    QByteArray rom(64 * 1024, char(0));
    qToBigEndian<quint32>(0x00fffff0, rom.data());
    qToBigEndian<quint32>(0x200, rom.data() + 4);
    rom.replace(0x100, 16, kind == MdKind::X32 ? "SEGA 32X        " : "SEGA GENESIS    ");
    rom.replace(0x120, 16, "KEGA LUA OVERLAY");
    rom.replace(0x1c0, 7, "KR-LUA1");
    rom[0x1f0] = 'U';
    int pc = 0x200;
    auto word = [&](quint16 data) {
        qToBigEndian(data, rom.data() + pc);
        pc += 2;
    };
    auto moveWord = [&](quint16 value, quint32 address) {
        word(0x33fc);
        word(value);
        word(quint16(address >> 16));
        word(quint16(address));
    };
    word(0x46fc);
    word(0x2700);
    if (kind == MdKind::X32) {
        // Palette marker from the original inspection_core_tests fixture.
        moveWord(1, 0xa15100); // Enable the adapter with its SH-2s held in reset.
        moveWord(0, 0xa15180);
        moveWord(0x83e0, 0xa15202);
    }
    moveWord(0x8004, 0xc00004);
    if (kind == MdKind::Firecore) {
        moveWord(0x8104, 0xc00004); // Enable Mode 5 before writing VRAM.
        moveWord(0x8f02, 0xc00004);
        moveWord(0x98c0, 0xc00004);
        const auto moveLong = [&](quint32 value, quint32 address) {
            word(0x23fc); word(quint16(value >> 16)); word(quint16(value));
            word(quint16(address >> 16)); word(quint16(address));
        };
        // Firecore bitmap block from the original inspection_core_tests fixture.
        moveLong(0x50880001, 0xc00004);
        moveLong(0x4523c14e, 0xc00000);
    }
    moveWord(0x8144, 0xc00004);
    moveWord(120, 0xff0020);
    moveWord(80, 0xff0022);
    moveWord(0xff80, 0xff0024);
    moveWord(12, 0xff0026);
    moveWord(8, 0xff0028);
    word(0x60fe);
    return rom;
}

QByteArray z80Fixture(bool sg, bool gg) {
    QByteArray rom(32 * 1024, char(0));
    // LD A,$BA; LD ($C020),A; JP $0005
    const auto code = QByteArray::fromHex("3eba3220c0c30500");
    rom.replace(0, code.size(), code);
    if (!sg) {
        rom.replace(0x7ff0, 8, "TMR SEGA");
        rom[0x7fff] = gg ? char(0x6c) : char(0x4c);
    }
    return rom;
}

QVector<ScriptUpdate> run(ScriptRuntime& runtime, const LibretroHost& host,
                          quint64 id, const QByteArray& source,
                          const QHash<QString, QImage>& images = {}) {
    runtime.command({ScriptAction::Run, id, QStringLiteral("fixture.lua"), source, images}, host);
    return runtime.takeUpdates();
}

void expectStatus(const QVector<ScriptUpdate>& updates, quint64 id, ScriptStatus status) {
    require(!updates.isEmpty(), "Missing script status");
    const auto& last = updates.constLast();
    require(last.id == id && last.status == status, "Unexpected status: " + last.text);
}

ScriptOverlayPtr frame(ScriptRuntime& runtime, const LibretroHost& host, bool draw = true) {
    return runtime.event(ScriptEvent::Frame, host, 45, host.runClock().frames,
                         host.inspectionPresentedInstance(), draw);
}

void checkMd(ScriptRuntime& runtime, LibretroHost& host) {
    const auto before = host.netplayDigest();
    QImage icon(4, 4, QImage::Format_ARGB32_Premultiplied);
    icon.fill(Qt::yellow);
    const auto setup = run(runtime, host, 1, R"(
assert(emu.apiVersion == 1 and emu.instances() == 1)
assert(not io and not os and not debug and not package and not coroutine)
assert(not require and not load and not loadfile and not dofile and not pcall and not xpcall)
assert(not setmetatable and not string.dump and not string.match and not table.unpack)
local test = {2, 1}
table.sort(test)
assert(table.concat(test, "-") == "1-2")
assert(string.rep("x", math.mininteger) == "")
assert(memory.domains(1) == nil)
local marker, reason = memory.range(1, 0x1c0, 7)
assert(marker == "KR-LUA1", reason or string.format("Unexpected ROM marker %q", marker))
local ram
for _, domain in ipairs(memory.domains()) do
    if domain.base == 0xff0000 then ram = domain end
end
assert(ram and ram.endian == "be")
assert(memory.read(ram.id, 0x20, 2) == 120)
assert(memory.read(ram.id, 0x24, 2, "be", true) == -128)
assert(memory.read(ram.id, 0x24, 2, "le") == 0x80ff)
assert(memory.read(ram.id, 0x24, 4) == 0xff80000c)
assert(memory.read(ram.id, ram.size - 1, 2) == nil)
assert(memory.peek(1, 0xff0020, 2) == 120)
assert(memory.peek(1, 0xffffff, 2) == nil)
assert(memory.peek(1, 0xa10001) == nil)
assert(type(memory.registers(1).values.D0) == "number")
assert(memory.registers(8) == nil)
assert(emu.screen().width > 0)
local clock = 0
local function render(event)
    if event.event == "frame" then clock = clock + 1 end
    assert(event.instance == 0)
    local label = event.event == "frame" and tostring(clock) or event.event .. ":" .. clock
    draw.text(5, 6, label, 0xff0000ff, "hud")
    draw.rect(108, 72, 24, 16, 0xffffffff, 0xff000080)
    draw.line(120, 80, 110, 70)
    draw.circle(120, 80, 2)
    assert(draw.image("icon.png", 0, 0, "hud"))
    print(event.event, event.frame)
end
for _, name in ipairs({"frame", "reset", "loadstate", "rewind", "draw"}) do emu.on(name, render) end
)", {{QStringLiteral("icon.png"), icon}});
    expectStatus(setup, 1, ScriptStatus::Running);
    expectStatus(run(runtime, host, 2, "assert(clock == nil)\nemu.on('frame', function() draw.text(0,0,'second') end)"),
                 2, ScriptStatus::Running);
    const auto first = frame(runtime, host);
    require(first && first->generation == 45 && first->frame == host.runClock().frames &&
            first->commands.size() == 6, "Overlay frame and ordering");
    require(first->commands[0].text == "1" && first->commands[0].space == ScriptSpace::Hud &&
            first->commands[0].color == Qt::red && first->commands[1].fill.alpha() == 128 &&
            first->commands[2].bounds.width() == -10 && first->commands[3].bounds.width() == 4 &&
            first->commands[4].image == icon && first->commands[5].text == "second", "All primitives");
    require(!frame(runtime, host, false), "Skipped presentation emits no overlay");
    require(frame(runtime, host)->commands.first().text == "3", "Skipped presentation still executes frame");
    runtime.takeUpdates();
    const std::pair<ScriptEvent, const char*> restoreEvents[] = {
        {ScriptEvent::Reset, "reset:3"}, {ScriptEvent::LoadState, "loadstate:3"},
        {ScriptEvent::Rewind, "rewind:3"}, {ScriptEvent::Draw, "draw:3"}
    };
    for (const auto& [event, label] : restoreEvents) {
        const auto overlay = runtime.event(event, host, 46, 2);
        require(overlay && overlay->commands.size() == 5 && overlay->commands.first().text == label,
                "Separate restore/reset/redraw events");
        const bool quiet = event == ScriptEvent::Rewind || event == ScriptEvent::Draw;
        require(runtime.takeUpdates().size() == (quiet ? 0 : 1),
                "No print output during rewind or redraw");
    }
    require(host.netplayDigest() == before && !host.debuggerAttached(),
            "Read-only script leaves machine state and debugger unchanged");
    require(runtime.compatible(host), "Unchanged machine compatible");
}

void checkLimits(ScriptRuntime& runtime, const LibretroHost& host) {
    ScriptCommand stopMissing;
    stopMissing.action = ScriptAction::Stop;
    stopMissing.id = 8;
    runtime.command(stopMissing, host);
    expectStatus(runtime.takeUpdates(), 8, ScriptStatus::Stopped);
    require(runtime.hasScripts(), "Stop for an inactive ID keeps other scripts");
    for (const auto& source : {
             QByteArray("while true do end"),
             QByteArray("local t = {}; for i=1,300 do t[i]=i end; table.sort(t)"),
             QByteArray("string.rep('x',1000000000)"),
             QByteArray("for i=1,100 do memory.range(1,0,4096) end"),
             QByteArray("\x1bLua", 4),
             QByteArray("local unfinished ="),
             QByteArray("local s = 'a'; for i=1,22 do s=s..s end")
         }) {
        const auto updates = run(runtime, host, 3, source);
        expectStatus(updates, 3, ScriptStatus::Error);
        require(runtime.hasScripts(), "Unrelated scripts still active");
    }
    expectStatus(run(runtime, host, 3, "\xef\xbb\xbf#!/bin/lua\nassert(true)"), 3, ScriptStatus::Running);
    expectStatus(run(runtime, host, 3, R"(emu.on('frame',function()
    draw.text(3,4,"must disappear")
    error("intentional")
end))"), 3, ScriptStatus::Running);
    const auto picture = frame(runtime, host);
    require(picture && picture->commands.size() == 6, "Failed script drawing rolled back");
    const auto callbackError = runtime.takeUpdates().constLast();
    require(callbackError.status == ScriptStatus::Error && callbackError.line == 3,
            "Error identifies source line");
    expectStatus(run(runtime, host, 3, "emu.on('frame',function() for i=1,10 do print('flood') end end)"),
                 3, ScriptStatus::Running);
    int emitted = 0, truncations = 0;
    for (int i = 0; i < 12; ++i) {
        frame(runtime, host);
        for (const auto& update : runtime.takeUpdates()) {
            if (update.id != 3 || update.status != ScriptStatus::Log) continue;
            if (update.text == "Script log truncated") ++truncations;
            else ++emitted;
        }
    }
    require(emitted <= 32 && truncations == 1, "Bounded log emissions");
    runtime.stopAll("test stop");
    require(!runtime.hasScripts() && !frame(runtime, host) && runtime.takeUpdates().size() == 3,
            "Stop clears all environments and drawings");
    runtime.command(stopMissing, host);
    expectStatus(runtime.takeUpdates(), 8, ScriptStatus::Stopped);
    require(!runtime.hasScripts(), "Stop does not create an environment");
}

void checkAutomation(ScriptRuntime& runtime, LibretroHost& host) {
    require(host.setControllerSixButton(0, false), "Select three-button fixture controller");
    const auto beforeFrame = [&] {
        MovieInput input;
        input.buttons[0] = 1u << 7; // Physical right.
        runtime.beforeFrame(host, 45, host.runClock().frames, input);
        return input;
    };
    const auto afterFrame = [&](const MovieInput& input) {
        return runtime.event(ScriptEvent::Frame, host, 45, host.runClock().frames, 0, true, &input);
    };
    runtime.setHostServices({true, true, true, [&](qsizetype limit, QString&) {
        const auto bytes = host.captureMovieState();
        require(bytes.size() <= limit, "Native snapshot fits Lua budget");
        return bytes;
    }});
    const auto anchor = host.runClock().frames;
    expectStatus(run(runtime, host, 10, R"(
local dev = input.devices()[1]
assert(dev.player == 1 and dev.instance == 0 and dev.kind == "md3")
assert(input.get(1) == nil and emu.lagged() == nil)
assert(emu.fps() >= 59 and emu.fps() <= 61)
local anchor = emu.framecount()
local save = assert(savestate.create())
assert(savestate.save(save))
assert(not savestate.save(save))
assert(emu.pause())
assert(input.set(1, {a = true, right = false}))
emu.on("beforeframe", function(e)
    assert(e.event == "beforeframe" and input.get(1).right)
    assert(not emu.pause())
    assert(input.set(1, {c = true}))
end)
emu.on("frame", function()
    assert(not emu.frameadvance())
    assert(emu.lagged() == true)
    draw.text(0, 0, "frame")
end)
emu.on("loadstate", function()
    assert(emu.framecount() == anchor and input.get(1) == nil)
    assert(not emu.pause() and not input.set(1, {a = true}))
    draw.text(0, 0, "restored")
end)
assert(emu.frameadvance())
local state = input.get(1)
assert(emu.framecount() == anchor + 1 and state.a and not state.right and state.c)
draw.text(0, 0, "resume")
assert(savestate.load(save))
assert(emu.frameadvance())
local state = input.get(1)
assert(emu.framecount() == anchor + 1 and not state.a and state.right and state.c)
assert(savestate.load(save))
assert(savestate.free(save))
)"), 10, ScriptStatus::Running);
    const auto first = runtime.takeControls();
    require(first.size() == 2 && first[0].type == ScriptControlType::Pause && first[0].value &&
            first[1].type == ScriptControlType::FrameAdvance && runtime.ownsControl(first[1]),
            "Pause and frame advance are deferred and owned");
    auto input = beforeFrame();
    require(input.buttons[0] == ((1u << 1) | (1u << 8)), "Sparse overrides before canonical poll");
    require(host.runClock().frames == anchor, "Lua startup and beforeframe do not run the core");
    host.runFrame();
    const auto overlay = afterFrame(input);
    require(overlay && overlay->commands.size() == 2 && overlay->commands[0].text == "frame" &&
            overlay->commands[1].text == "resume", "Callback precedes coroutine continuation");
    auto requested = runtime.takeControls();
    require(requested.size() == 2 && requested[0].type == ScriptControlType::LoadState &&
            requested[0].snapshot && requested[1].type == ScriptControlType::FrameAdvance &&
            runtime.ownsControl(requested[0]), "Restore request retains the saved bytes");
    QString error;
    require(host.restoreMovieState(*requested[0].snapshot, error), "Restore captured state: " + error);
    runtime.invalidateTimeline(false, requested[0].run);
    const auto restored = runtime.event(ScriptEvent::LoadState, host, 46, host.runClock().frames);
    require(restored && restored->commands.first().text == "restored" && runtime.ownsControl(requested[1]),
            "Own restore keeps the waiting coroutine and runs the loadstate observer");
    input = beforeFrame();
    require(input.buttons[0] == ((1u << 7) | (1u << 8)), "Restore clears prior overrides");
    host.runFrame();
    require(bool(afterFrame(input)), "Frame after restore succeeds");
    const auto again = runtime.takeControls();
    require(again.size() == 1 && again[0].type == ScriptControlType::LoadState &&
            requested[0].snapshot == again[0].snapshot, "A save is reusable and survives free while queued");

    expectStatus(run(runtime, host, 10, "assert(emu.frameadvance())\nerror('resumed failure')"),
                 10, ScriptStatus::Running);
    require(!runtime.ownsControl(again[0]), "Restart invalidates queued operations by instance");
    const auto suspended = runtime.takeControls();
    require(suspended.size() == 1 && runtime.ownsControl(suspended[0]), "New run owns its frame request");
    host.runFrame();
    afterFrame(input);
    const auto errorUpdates = runtime.takeUpdates();
    expectStatus(errorUpdates, 10, ScriptStatus::Error);
    require(errorUpdates.last().line == 2 && !runtime.ownsControl(suspended[0]),
            "Coroutine errors retain Lua source line and revoke ownership");
    expectStatus(run(runtime, host, 10, "assert(emu.frameadvance())\nerror('must never resume')"),
                 10, ScriptStatus::Running);
    runtime.invalidateTimeline(true);
    expectStatus(runtime.takeUpdates(), 10, ScriptStatus::Stopped);
    require(runtime.takeControls().isEmpty() && !runtime.hasScripts(), "External seek stops suspended code");

    expectStatus(run(runtime, host, 10, "assert(emu.pause())\nerror('startup failure')"),
                 10, ScriptStatus::Error);
    require(runtime.takeControls().isEmpty(), "Startup error revokes already queued controls");
    expectStatus(run(runtime, host, 10, "assert(input.set(1, {a = true}))"), 10, ScriptStatus::Running);
    expectStatus(run(runtime, host, 11, "assert(input.set(1, {a = false, b = true}))"),
                 11, ScriptStatus::Running);
    expectStatus(run(runtime, host, 10, "assert(input.set(1, {a = true}))"), 10, ScriptStatus::Running);
    expectStatus(run(runtime, host, 12, R"(
emu.on("beforeframe", function()
    assert(input.set(1, {right = false}))
    error("reject input from failed callback")
end)
)"), 12, ScriptStatus::Running);
    input = beforeFrame();
    require(input.buttons[0] == ((1u << 7) | 1u), "Later slot wins after restart and failed input is removed");
    expectStatus(runtime.takeUpdates(), 12, ScriptStatus::Error);
    runtime.command({ScriptAction::Stop, 11}, host);
    expectStatus(runtime.takeUpdates(), 11, ScriptStatus::Stopped);
    require(beforeFrame().buttons[0] == ((1u << 7) | (1u << 1)), "Stop releases just that writer");
    runtime.setHostPolicy(false, false, false);
    require(beforeFrame().buttons[0] == (1u << 7), "Read-only mode removes overrides");
    expectStatus(run(runtime, host, 11, R"(
assert(not input.set(1, {a = true}))
assert(not emu.reset() and not emu.frameadvance())
assert(not savestate.create())
emu.on("frame", function()
    assert(input.get(1).right)
    draw.text(0, 0, "read-only")
end)
)"), 11, ScriptStatus::Running);
    const auto observing = afterFrame(beforeFrame());
    require(observing && observing->commands.first().text == "read-only", "Read-only mode observes input");
    runtime.stopAll();
    runtime.takeUpdates();

    runtime.setHostPolicy(true, true, true);
    require(host.setControllerKind(0, ControllerType::Mouse) &&
            host.setControllerKind(1, ControllerType::Justifier) &&
            host.setControllerConnected(1, true) && host.setControllerConnected(2, true),
            "Enable mouse and chained Justifiers");
    expectStatus(run(runtime, host, 13, R"(
local devices = input.devices()
assert(devices[1].kind == "mouse" and devices[2].kind == "justifier" and devices[3].player == 3)
assert(input.set(1, {left = true, x = -32768, y = 22}))
assert(input.set(3, {trigger = false, reload = true, x = 456, offscreen = true}))
emu.on("frame", function()
    local m = input.get(1)
    local g = input.get(3)
    assert(m.left and m.x == -32768 and m.y == 22 and m.offscreen == nil)
    assert(not g.trigger and g.reload and g.x == 456 and g.y == -123 and g.offscreen)
    draw.text(0, 0, "analog")
end)
)"), 13, ScriptStatus::Running);
    input = {};
    input.buttons[2] = 1u << 1; // Live gun trigger.
    input.aim[2] = quint64(quint16(-123)) << 16;
    runtime.beforeFrame(host, 45, host.runClock().frames, input);
    require(input.buttons[0] == (1u << 1) && input.buttons[2] == (1u << 2) &&
            input.aim[0] == (quint64(22) << 16 | 0x8000) &&
            input.aim[2] == (quint64(1) << 32 | quint64(quint16(-123)) << 16 | 456),
            "Button and each analog axis override independently");
    const auto analog = afterFrame(input);
    require(analog && analog->commands.first().text == "analog", "Input API decodes full analog range");
    runtime.stopAll();
    runtime.takeUpdates();
    host.setControllerConnected(2, false);
    host.setControllerConnected(1, false);
    host.setControllerKind(1, ControllerType::Pad);
    host.setControllerKind(0, ControllerType::Pad);
    host.setControllerSixButton(0, true);
    runtime.setHostServices({});
}

void checkSaveLimits(ScriptRuntime& runtime, const LibretroHost& host) {
    int captures = 0;
    runtime.setHostServices({true, true, true, [&](qsizetype limit, QString&) {
        ++captures;
        require(limit == ScriptLimits::MaxSaveObjectBytes, "Capture size budget");
        return QByteArray(limit, Qt::Uninitialized);
    }});
    expectStatus(run(runtime, host, 20, R"(
local saves = {}
for i = 1, 8 do saves[i] = assert(savestate.create()) end
assert(not savestate.create())
assert(savestate.save(saves[1]))
assert(savestate.load(saves[1]))
assert(savestate.free(saves[1]))
local new = assert(savestate.create())
assert(new ~= saves[1] and not savestate.load(saves[1]))
)"), 20, ScriptStatus::Running);
    auto retained = runtime.takeControls();
    expectStatus(run(runtime, host, 21, R"(
local save = assert(savestate.create())
assert(not savestate.load(1))
assert(savestate.save(save))
)"), 21, ScriptStatus::Running);
    expectStatus(run(runtime, host, 22, R"(
local save = assert(savestate.create())
assert(not savestate.save(save))
emu.on("frame", function()
    assert(savestate.save(save))
    assert(savestate.load(save))
end)
)"), 22, ScriptStatus::Running);
    require(captures == 2 && retained.size() == 1, "Queued load storage counts toward aggregate byte limit");
    retained.clear();
    frame(runtime, host);
    require(captures == 3 && runtime.takeControls().size() == 1 && runtime.takeUpdates().isEmpty(),
            "Released queue storage is available for future captures");
    runtime.stopAll();
    runtime.takeUpdates();
    runtime.setHostServices({});
}

bool connectAccess(ScriptRuntime& runtime, LibretroHost& host) {
    return host.setDebugAccessObserver(runtime.accessSubscriptions(),
        [](void* opaque, const DebugAccessEvent& e) {
            static_cast<ScriptRuntime*>(opaque)->dispatchMemoryAccess(e);
        }, &runtime);
}

void checkDebuggerExample(ScriptRuntime& runtime, LibretroHost& host, const QString& directory) {
    QFile source(directory + QStringLiteral("/fixture-debugger.lua"));
    require(source.open(QIODevice::ReadOnly), "Open the debugger fixture example");
    runtime.setHostPolicy(false, false, false, true, true);
    expectStatus(run(runtime, host, 1, source.readAll()), 1, ScriptStatus::Running);
    const auto controls = runtime.takeControls();
    require(controls.size() == 1 && controls[0].type == ScriptControlType::MemoryWrite &&
            host.inspectionWrite(controls[0].instance, controls[0].target, controls[0].offset,
                                 controls[0].bytes.constData(), size_t(controls[0].bytes.size())),
            "Apply the debugger fixture's memory edit");
    const auto annotations = runtime.annotations();
    require(annotations.labels.size() == 1 && annotations.watches.size() == 1 && connectAccess(runtime, host),
            "Install example label, RAM watch and execute hook");
    for (int step = 0; step < 2; ++step) {
        runtime.beginAccessFrame(host, host.runClock().frames + 1);
        host.runFrame();
        runtime.endAccessFrame();
        const auto overlay = frame(runtime, host);
        require(overlay && overlay->commands.first().text == "Loop callbacks: 32 (budget reached)",
                "Show limited execute hooks on the example HUD");
        const auto updates = runtime.takeUpdates();
        require(updates.size() == (step == 0 ? 1 : 0),
                "Example reports truncation only once per run");
        if (!updates.isEmpty()) expectStatus(updates, 1, ScriptStatus::Log);
    }
    runtime.stopAll();
    runtime.takeUpdates();
    require(host.setDebugAccessObserver({}), "Clear the debugger example observer");
    runtime.setHostPolicy(false, false, false);
}

void checkAccessAndEdits(ScriptRuntime& runtime, LibretroHost& host, const QTemporaryDir& temp) {
    host.unload();
    auto rom = mdFixture();
    // Loop: MOVE.W ($FF0050).L,D0; MOVE.W D0,($FF0052).L; BRA.S to the read.
    const auto loop = QByteArray::fromHex("303900ff005033c000ff005260f2");
    rom.replace(0x23c, loop.size(), loop);
    QString error;
    require(host.load(writeFixture(temp, "access.md", rom), error), "Load access fixture: " + error);
    host.runFrame();
    runtime.setHostServices({true, true, true, [&](qsizetype, QString&) {
        return host.captureMovieState();
    }, true, true});
    expectStatus(run(runtime, host, 26, R"(
local domains = {}
for _, d in ipairs(memory.domains()) do domains[d.id] = d end
assert(domains[2].writable and not domains[1].writable)
local main = memory.cpus()[1]
assert(main.id == 1 and main.hooks.exec and main.hooks.read and main.hooks.write)
assert(memory.writableregisters(1).D1 == 8)
assert(memory.write(1, 0, 0) == nil)
assert(memory.write(2, domains[2].size - 1, 0, 2) == nil)
assert(memory.write(2, 0, 0, 1, nil, 1) == nil)
assert(memory.setregister(1, "UNKNOWN", 0) == nil)
assert(memory.on(1, "read", 0x1000000, nil, function() end) == nil)
assert(memory.watch(2, domains[2].size, 1, "outside") == nil)
local unused = assert(memory.watch(2, 0x10, 1, "unused"))
assert(memory.unwatch(unused) and not memory.unwatch(unused))
local label = assert(memory.label(1, 0x23c, "Copy word"))
local watch = assert(memory.watch(2, 0x52, 2, "Copied value"))
assert(watch >= 0x80000000 and label < 0x80000000)

local save = assert(savestate.create())
assert(savestate.save(save))
assert(memory.write(2, 0x50, 0x3412, 2, "le"))
assert(memory.write(2, 0x54, -2, 2))
assert(memory.setregister(1, "D1", 0x76543210))
assert(memory.read(2, 0x50, 2) == 0)
assert(savestate.load(save))
local count, types = 0, {}
local function access(e)
    assert(e.instance == 0 and e.cpu == 1 and e.frame == emu.framecount() + 1)
    if count == 0 then
        assert(not memory.write(2, 0x50, 0))
        assert(not memory.setregister(1, "D1", 0))
        assert(not emu.pause() and not emu.reset() and not emu.frameadvance())
        assert(not input.set(1, {start = true}))
        assert(not savestate.load(save))
        assert(not memory.label(1, 0x23c, "forbidden"))
        assert(not memory.on(1, "exec", 0x23c, nil, access))
    end
    if e.event == "exec" then
        assert(e.address == 0x23c and e.size == 0 and e.value == 0)
    else
        assert(e.size == 2 and e.value == 0x1234)
        if e.event == "read" then assert(e.address == 0xff0050) end
        if e.event == "write" then
            assert(e.address == 0xff0052 and memory.peek(1, e.address, 2) == e.value)
        end
    end
    types[e.event] = true
    count = count + 1
end
local read = assert(memory.on(1, "read", 0xff0051, nil, access))
local write = assert(memory.on(1, "write", 0xff0052, 0xff0053, access))
local exec = assert(memory.on(1, "exec", 0x23c, nil, access))
local other = assert(memory.on(1, "exec", 0x23c, nil, function() error("removed") end))
assert(memory.off(other) and not memory.off(other))
assert(emu.frameadvance())
assert(not savestate.load(save))
assert(count == 32 and types.exec and types.read and types.write, "Access callbacks missing: " .. count)
local stats = assert(memory.hookstats())
assert(stats.truncated and stats.callbacks == count)
assert(memory.registers(1).values.D1 == 0x76543210)
assert(memory.read(2, 0x54, 2, nil, true) == -2)
assert(memory.off(read) and memory.off(write) and memory.off(exec))
assert(memory.unlabel(label) and memory.unwatch(watch))
draw.text(0, 0, "access")
)"), 26, ScriptStatus::Running);

    const auto annotations = runtime.annotations();
    require(annotations.labels.size() == 1 && annotations.labels.first().address == 0x23c &&
            annotations.watches.size() == 1 && annotations.watches.first().offset == 0x52,
            "Live script annotations use CPU addresses and physical-domain offsets");
    const auto annotationRevision = runtime.annotationRevision();
    const auto controls = runtime.takeControls();
    require(controls.size() == 5 && controls[0].type == ScriptControlType::MemoryWrite &&
            controls[0].bytes == QByteArray::fromHex("1234") && controls[0].offset == 0x50 &&
            controls[1].type == ScriptControlType::MemoryWrite &&
            controls[1].bytes == QByteArray::fromHex("fffe") &&
            controls[2].type == ScriptControlType::RegisterWrite && controls[2].registerName == "D1" &&
            controls[3].type == ScriptControlType::LoadState && controls[3].snapshot &&
            controls[4].type == ScriptControlType::FrameAdvance,
            "Mutations use canonical bytes and keep control order");
    for (const auto& control : {controls[0], controls[1]}) {
        require(host.inspectionWrite(control.instance, control.target, control.offset,
                                     control.bytes.constData(), size_t(control.bytes.size())),
                "Apply deferred memory edit at the boundary");
    }
    require(host.inspectionWriteRegister(controls[2].instance, controls[2].target,
                                         controls[2].registerName.constData(), controls[2].valueBits),
            "Apply deferred register edit at the boundary");
    runtime.discardSavedStates();
    require(!runtime.ownsControl(controls[3]) && runtime.ownsControl(controls[4]),
            "Edits invalidate older state loads and keep a pending advance");
    require(runtime.accessSubscriptions().size() == 3 && connectAccess(runtime, host),
            "Connect real access observer");
    const auto accessRevision = runtime.accessRevision();
    runtime.beginAccessFrame(host, host.runClock().frames + 1);
    host.runFrame();
    runtime.endAccessFrame();
    const auto overlay = frame(runtime, host);
    const auto updates = runtime.takeUpdates();
    expectStatus(updates, 26, ScriptStatus::Log);
    require(updates.size() == 1 && overlay && overlay->commands.first().text == "access",
            "Completed Lua access verification and emitted one truncation notice");
    require(runtime.accessRevision() != accessRevision && runtime.accessSubscriptions().isEmpty() &&
            runtime.annotationRevision() != annotationRevision && runtime.annotations().labels.isEmpty() &&
            runtime.annotations().watches.isEmpty() && connectAccess(runtime, host),
            "Remove all access and annotation handles");

    expectStatus(run(runtime, host, 26, R"(
assert(memory.watch(2, 0x50, 2, "fault owned"))
assert(memory.label(1, 0x23c, "fault owned"))
assert(memory.on(1, "exec", 0x23c, nil, function() error("native hook failure") end))
)"), 26, ScriptStatus::Running);
    expectStatus(run(runtime, host, 27, R"(
local seen = false
assert(memory.on(1, "exec", 0x240, nil, function() seen = true end))
emu.on("frame", function()
    local stats = assert(memory.hookstats())
    assert(not seen and not stats.truncated and stats.callbacks == 0)
end)
)"), 27, ScriptStatus::Running);
    runtime.setHostPolicy(false, false, false, false, false);
    require(runtime.accessSubscriptions().isEmpty() && runtime.annotations().labels.size() == 1,
            "Access policy disables hooks and permits read-only annotations");
    runtime.setHostPolicy(false, false, false, false, true);
    require(connectAccess(runtime, host), "Restore hook observer after policy change");
    runtime.beginAccessFrame(host, host.runClock().frames + 1);
    host.runFrame();
    runtime.endAccessFrame();
    expectStatus(runtime.takeUpdates(), 26, ScriptStatus::Error);
    frame(runtime, host);
    require(runtime.hasScripts() && runtime.accessSubscriptions().size() == 1 &&
            runtime.annotations().labels.isEmpty() && runtime.annotations().watches.isEmpty() &&
            runtime.takeUpdates().isEmpty(), "A failed hook releases only its own script");
    runtime.stopAll();
    runtime.takeUpdates();
    require(host.setDebugAccessObserver({}), "Clear real access observer");
    runtime.setHostServices({});
}

void checkZ80(ScriptRuntime& runtime, LibretroHost& host, unsigned expectedHardware) {
    kr_inspect_info catalog{};
    require(host.inspectionCatalog(0, catalog) && (catalog.hardware & expectedHardware),
            "Z80 fixture hardware");
    const auto cpus = host.inspectionCpuTargets(0);
    require(cpus.size() == 1 && cpus.first().cpu == KR_DEBUG_Z80, "Z80 target only");
    expectStatus(run(runtime, host, 1, R"(
local cpus = memory.cpus()
assert(#cpus == 1 and cpus[1].id == 2 and cpus[1].endian == "le")
assert(memory.peek(2, 0, 2) == 0xba3e)
assert(memory.peek(2, 0xc020) == 0xba)
assert(type(memory.registers(2).values.AF) == "number")
assert(memory.peek(1, 0) == nil)
local found = false
for _, d in ipairs(memory.domains()) do
    if d.base == 0xc000 then
        assert(memory.read(d.id, 0x20) == 0xba)
        found = true
    end
end
assert(found)
)"), 1, ScriptStatus::Running);
}

// Original empty disc and minimal 68k boot ROM. The standard identification
// fields let the host recognize Sega CD without a copy of a production BIOS.
void checkExpansions(ScriptRuntime& runtime, LibretroHost& host, const QTemporaryDir& temp) {
    const auto check = [&](const QString& path, unsigned hardware, const QByteArray& source) {
        host.unload();
        QString error;
        require(host.load(path, error), "Load expansion fixture " + path + ": " + error);
        for (int i = 0; i < 3; ++i) host.runFrame();
        kr_inspect_info catalog{};
        require(host.inspectionCatalog(0, catalog) && catalog.hardware == hardware,
                "Expansion catalog: " + path + " " + QString::number(catalog.hardware));
        const auto before = host.serialize();
        require(!before.isEmpty(), "Serialize expansion fixture");
        const auto updates = run(runtime, host, 1, source);
        require(!updates.isEmpty() && updates.constLast().status == ScriptStatus::Running,
                "Script for " + path + ": " + (updates.isEmpty() ? QString() : updates.constLast().text));
        require(host.serialize() == before && !host.debuggerAttached() &&
                !host.traceActive() && !host.coverageActive(),
                "Expansion read leaves state and native observation modes unchanged");
        runtime.stopAll();
        runtime.takeUpdates();
    };

    const QByteArray x32 = R"(
local cpus = {}
for _, cpu in ipairs(memory.cpus()) do cpus[cpu.id] = cpu end
assert(cpus[8].addressBits == 32 and cpus[16].addressBits == 32)
assert(memory.registers(8).values.R0 and memory.registers(16).values.R15)
local value = assert(memory.read(13, 0, 2))
assert(memory.peek(8, 0x06000000, 2) == value and memory.peek(16, 0x06000000, 2) == value)
)";
    check(writeFixture(temp, "read.32x", mdFixture(MdKind::X32)), KR_HW_MD | KR_HW_32X,
          x32 + "\nassert(memory.read(16, 2, 2) == 0x83e0)");
    check(writeFixture(temp, "read.atf", mdFixture(MdKind::Firecore)), KR_HW_MD | KR_HW_FIRECORE, R"(
assert(memory.read(7, 24) == 0xc0)
assert(memory.read(4, 0x5088, 4) == 0x4523c14e)
assert(memory.peek(1, 0xff0020, 2) == 120)
assert(memory.read(13, 0) == nil)
)");

    QByteArray bios(128 * 1024, char(0));
    qToBigEndian<quint32>(0xfffffd00, bios.data());
    qToBigEndian<quint32>(0x200, bios.data() + 4);
    bios.replace(0x100, 16, "SEGA MEGA DRIVE ");
    bios.replace(0x110, 7, "(C)SEGA");
    bios.replace(0x120, 16, "SEGA-CD BOOT ROM");
    bios.replace(0x150, 16, "SEGA-CD BOOT ROM");
    bios.replace(0x180, 3, "BR ");
    bios[0x1f0] = 'U';
    qToBigEndian<quint16>(0x60fe, bios.data() + 0x200); // BRA.S *.
    qToBigEndian<quint16>(0x60fe, bios.data() + 0x18e);
    require(QDir(temp.path()).mkpath("bios"), "Create isolated BIOS directory");
    writeFixture(temp, "bios/bios_CD_U.bin", bios);

    QByteArray disc(150 * 2048, char(0));
    disc.replace(0, 14, "SEGADISCSYSTEM");
    const auto discPath = writeFixture(temp, "read.iso", disc);
    const QByteArray cd = R"(
local domains = {}
for _, d in ipairs(memory.domains()) do domains[d.id] = d end
for id = 9, 12 do
    assert(domains[id].size > 0)
    assert(#memory.range(id, 0, 4) == 4)
end
assert(#memory.range(1, 0x100, 16) == 16)
assert(memory.registers(4).values.D0)
assert(memory.peek(4, 0, 2) == memory.read(9, 0, 2))
)";
    check(discPath, KR_HW_MD | KR_HW_CD, cd + "\nassert(memory.read(13, 0) == nil)");
    host.setExpansionDevices(ExpansionMode::Auto, ExpansionMode::Connected);
    check(discPath, KR_HW_MD | KR_HW_CD | KR_HW_32X, cd + x32);
    host.unload();
}

void checkLinked(ScriptRuntime& runtime, LibretroHost& host) {
    QString error;
    require(host.setLinkCableEnabled(true, error), "Enable GG Link: " + error);
    require(!runtime.compatible(host), "One-console script invalidated after link change");
    runtime.stopAll();
    runtime.takeUpdates();
    host.setLinkPresentationRole(2);
    host.runFrame();
    expectStatus(run(runtime, host, 1, R"(
assert(emu.instances() == 2)
assert(memory.peek(2, 0, 2, nil, false, 0) == 0xba3e)
assert(memory.peek(2, 0, 2, nil, false, 1) == 0xba3e)
assert(memory.peek(2, 0, 2, nil, false, 2) == nil)
assert(memory.cpus(1)[1].id == 2)
emu.on("frame", function(e)
    assert(e.instance == 1 and memory.peek(2, 0, 2) == 0xba3e)
    draw.text(1, 1, "B")
end)
)"), 1, ScriptStatus::Running);
    const auto overlay = frame(runtime, host);
    require(overlay && overlay->instance == 1 && overlay->commands.first().text == "B",
            "Link B can inspect both consoles");
    runtime.setHostPolicy(false, false, false, true, true);
    expectStatus(run(runtime, host, 2, R"(
local ram
for _, d in ipairs(memory.domains()) do
    if d.base == 0xc000 then ram = d.id end
end
assert(ram and memory.write(ram, 0x20, 0x6d))
assert(memory.setregister(2, "BC", 0x1234))
assert(memory.watch(ram, 0x20, 1, "Peer RAM"))
assert(memory.label(2, 0x0005, "Peer loop"))
local seen
assert(memory.on(2, "exec", 0x0005, nil, function(e)
    assert(e.instance == 1 and e.cpu == 2 and e.address == 0x0005)
    assert(e.frame == emu.framecount() + 1)
    assert(memory.peek(2, 0xc020) == 0x6d and memory.peek(2, 0xc020, 1, nil, false, 0) == 0xba)
    assert(memory.registers(2).values.BC == 0x1234)
    seen = e.frame
end))
emu.on("frame", function(e)
    assert(e.instance == 1 and seen == e.frame)
    assert(memory.hookstats().callbacks > 0)
    draw.text(0, 0, "peer hook")
end)
)"), 2, ScriptStatus::Running);
    const auto controls = runtime.takeControls();
    require(controls.size() == 2 && controls[0].instance == 1 && controls[1].instance == 1 &&
            host.inspectionWrite(1, controls[0].target, controls[0].offset,
                                 controls[0].bytes.constData(), size_t(controls[0].bytes.size())) &&
            host.inspectionWriteRegister(1, controls[1].target, controls[1].registerName.constData(),
                                         controls[1].valueBits),
            "Apply Lua memory and register writes to the presented peer");
    const auto notes = runtime.annotations();
    require(notes.labels.size() == 1 && notes.labels.first().instance == 1 &&
            notes.watches.size() == 1 && notes.watches.first().instance == 1,
            "Presented peer is the default Lua annotation target");
    require(connectAccess(runtime, host), "Subscribe to native accesses only on the peer");
    runtime.beginAccessFrame(host, host.runClock().frames + 1);
    host.runFrame();
    runtime.endAccessFrame();
    runtime.takeUpdates();
    const auto observed = frame(runtime, host);
    require(observed && observed->commands.size() == 2 &&
            observed->commands.last().text == "peer hook" && runtime.takeUpdates().isEmpty(),
            "Observe a complete linked frame and filter events by console");
    require(host.setDebugAccessObserver({}), "Clear peer access observer");
    runtime.setHostPolicy(false, false, false);

    require(host.setLinkCableEnabled(false, error), "Disable GG Link: " + error);
    require(!runtime.compatible(host), "Link teardown invalidates peer catalog");
    runtime.stopAll();
    runtime.takeUpdates();
}

QVector<ScriptUpdate> runStorage(ScriptRuntime& runtime, const LibretroHost& host,
                                 const QString& root, const QByteArray& source,
                                 const QString& identity = QStringLiteral("scripts/capture.lua")) {
    ScriptCommand command{};
    command.id = 8;
    command.path = QDir(root).filePath(QStringLiteral("capture.lua"));
    command.source = source;
    command.storageRoot = root;
    command.storageIdentity = identity;
    runtime.command(command, host);
    return runtime.takeUpdates();
}

void awaitStorage(ScriptRuntime& runtime, const LibretroHost& host, const QString& log) {
    for (int i = 0; i < 1500; ++i) {
        frame(runtime, host);
        for (const auto& update : runtime.takeUpdates()) {
            require(update.status != ScriptStatus::Error, update.text);
            if (update.status == ScriptStatus::Log && update.text == log) return;
        }
        QThread::msleep(1);
    }
    require(false, "Timeout waiting for " + log);
}

void checkStorage(ScriptRuntime& runtime, LibretroHost& host, const QTemporaryDir& temp,
                  const QString& examples) {
    const auto root = temp.filePath("scripts-data");
    const auto start = [&](const QByteArray& source) {
        expectStatus(runStorage(runtime, host, root, source), 8, ScriptStatus::Running);
    };
    start(R"(
local bytes = string.rep("x", 4096)
for _ = 1, 5 do bytes = bytes .. bytes end
bytes = bytes .. "K\0ega"
local write = assert(storage.write("capture.csv", bytes))
local second, busy = storage.write("never.csv", "ignored")
assert(not second and busy == "Script storage transfer limit reached")
local read = assert(storage.read("capture.csv", #bytes - 5, 5))
emu.on("reset", function()
    local unavailable = storage.read("capture.csv")
    assert(not unavailable)
end)
emu.on("frame", function()
    if write then
        local done, err = storage.result(write)
        if not done then assert(err == "pending", err); return end
        assert(done == true)
        local unknown, reason = storage.result(write)
        assert(not unknown and reason == "unknown storage request")
        write = nil
    end
    local data, err = storage.result(read)
    if not data then assert(err == "pending", err); return end
    assert(data == "K\0ega")
    print("storage round trip")
    emu.on("frame", nil)
end)
)");
    runtime.event(ScriptEvent::Reset, host, 45, host.runClock().frames);
    require(runtime.takeUpdates().isEmpty(), "Storage denied during restore events");
    awaitStorage(runtime, host, QStringLiteral("storage round trip"));
    const QDir parent(root);
    const auto scopes = parent.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    require(scopes.size() == 1, "One per-script storage directory");
    const QDir scope(parent.filePath(scopes.front()));
    QFile data(scope.filePath("capture.csv"));
    require(data.open(QIODevice::ReadOnly), "Stored CSV is readable on disk");
    auto bytes = data.readAll();
    require(bytes.size() == 128 * 1024 + 5 && bytes.endsWith(QByteArray("K\0ega", 5)),
            "Store all bytes including an embedded zero");
    data.close();
    require(!scope.exists("never.csv"), "Rejected write has no disk side effects");

    start(R"(
local read = assert(storage.read("capture.csv"))
local eof = assert(storage.read("capture.csv", 1048576, 16))
emu.on("frame", function()
    if read then
        local data, err = storage.result(read)
        if not data then assert(err == "pending", err); return end
        assert(#data == 131077)
        assert(string.sub(data, 131073, 131077) == "K\0ega")
        assert(string.byte(data, -1) == 97)
        assert(string.sub(data, math.maxinteger) == "")
        assert(string.sub(data, -3, -1) == "ega")
        assert(string.sub(data, -1, math.mininteger) == "")
        read = nil
        return
    end
    local empty, err = storage.result(eof)
    if not empty then assert(err == "pending", err); return end
    assert(empty == "")
    print("storage persisted across restart")
    emu.on("frame", nil)
end)
)");
    awaitStorage(runtime, host, QStringLiteral("storage persisted across restart"));
    expectStatus(runStorage(runtime, host, root, R"(
string.sub(string.rep("x", 4096) .. "x", 1, 4097)
)"), 8, ScriptStatus::Error);

    expectStatus(runStorage(runtime, host, root, R"(
local read = assert(storage.read("capture.csv"))
emu.on("frame", function()
    local value, err = storage.result(read)
    assert(not value)
    if err == "pending" then return end
    assert(err == "file not found", err)
    print("storage isolated")
    emu.on("frame", nil)
end)
)", QStringLiteral("scripts/another/capture.lua")), 8, ScriptStatus::Running);
    awaitStorage(runtime, host, QStringLiteral("storage isolated"));

    for (const QByteArray& name : {QByteArray("../outside.txt"), QByteArray("sub/file.txt"),
                                   QByteArray("C:\\foo"), QByteArray("file.txt:stream"),
                                   QByteArray("nUl.csv"), QByteArray("a.")}) {
        const auto updates = runStorage(runtime, host, root, "storage.read('" + name + "')");
        expectStatus(updates, 8, ScriptStatus::Error);
        require(updates.last().text.contains("portable storage filename"), "Reject unsafe basename");
    }

    start(R"(
for i = 1, 8 do assert(storage.read("missing.csv", 0, 1)) end
local request, err = storage.read("missing.csv", 0, 1)
assert(not request and err == "script storage queue is full", err)
)");
    runtime.stopAll();
    runtime.takeUpdates();

    QFile large(scope.filePath("large.csv"));
    require(large.open(QIODevice::WriteOnly) && large.resize(1024 * 1024 + 1), "Oversized file");
    large.close();
    start(R"(
local read = assert(storage.read("large.csv", 0, 32))
emu.on("frame", function()
    local value, err = storage.result(read)
    assert(not value)
    if err == "pending" then return end
    assert(err == "script storage file exceeds 1 MiB", err)
    print("storage refused oversized file")
    emu.on("frame", nil)
end)
)");
    awaitStorage(runtime, host, QStringLiteral("storage refused oversized file"));

#ifdef Q_OS_UNIX
    const auto outside = writeFixture(temp, "outside.csv", "outside");
    require(QFile::link(outside, scope.filePath("link.csv")), "Create scoped symlink");
    start(R"(
local read = assert(storage.read("link.csv"))
local write = assert(storage.write("link.csv", "overwrite"))
emu.on("frame", function()
    local value, reason = storage.result(read or write)
    assert(not value)
    if reason == "pending" then return end
    assert(reason == "unsafe script storage file", reason)
    if read then read = nil; return end
    print("storage refused file symlink")
    emu.on("frame", nil)
end)
)");
    awaitStorage(runtime, host, QStringLiteral("storage refused file symlink"));
    QFile untouched(outside);
    require(untouched.open(QIODevice::ReadOnly) && untouched.readAll() == "outside", "Outside file untouched");

    const auto redirected = temp.filePath("redirected-data");
    require(QFile::link(root, redirected), "Create storage root symlink");
    expectStatus(runStorage(runtime, host, redirected, R"(
local id = assert(storage.read("capture.csv"))
emu.on("frame", function()
    local value, reason = storage.result(id)
    assert(not value)
    if reason == "pending" then return end
    assert(reason == "script storage directory is unavailable or unsafe", reason)
    print("storage refused directory symlink")
    emu.on("frame", nil)
end)
)"), 8, ScriptStatus::Running);
    awaitStorage(runtime, host, QStringLiteral("storage refused directory symlink"));
#endif

    if (!examples.isEmpty()) {
        QFile example(QDir(examples).filePath("file-roundtrip.lua"));
        require(example.open(QIODevice::ReadOnly), "Open packaged storage example");
        expectStatus(runStorage(runtime, host, root, example.readAll(),
                                QStringLiteral("scripts/file-roundtrip.lua")), 8, ScriptStatus::Running);
        awaitStorage(runtime, host, QStringLiteral("File saved and read successfully"));
    }

    runtime.stopAll();
    runtime.takeUpdates();
}
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        if (argc == 3 && QByteArray(argv[1]) == "--write-fixture") {
            QFile output(QString::fromLocal8Bit(argv[2]));
            const auto rom = mdFixture();
            require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                    output.write(rom) == rom.size(), output.fileName());
            return 0;
        }
        require(argc == 2 || argc == 3, "usage: lua_runtime_tests CORE [EXAMPLE_DIRECTORY]");
        QTemporaryDir data;
        require(data.isValid(), "Temporary data folder");
        LibretroHost host(QString::fromLocal8Bit(argv[1]), data.path());
        ScriptRuntime runtime;
        const auto md = writeFixture(data, "hud.md", mdFixture());
        QString error;
        require(host.load(md, error), "Load MD fixture: " + error);
        host.runFrame();
        checkMd(runtime, host);
        checkLimits(runtime, host);
        checkAutomation(runtime, host);
        checkSaveLimits(runtime, host);
        checkStorage(runtime, host, data, argc == 3 ? QString::fromLocal8Bit(argv[2]) : QString());
        if (argc == 3) {
            for (const QString& example : {QStringLiteral("memory-hud.lua"), QStringLiteral("fixture-hitbox.lua")}) {
                QFile source(QString::fromLocal8Bit(argv[2]) + QLatin1Char('/') + example);
                require(source.open(QIODevice::ReadOnly), "Open " + example);
                expectStatus(run(runtime, host, 1, source.readAll()), 1, ScriptStatus::Running);
                require(bool(frame(runtime, host)), "Run " + example);
                runtime.stopAll();
                runtime.takeUpdates();
            }
            checkDebuggerExample(runtime, host, QString::fromLocal8Bit(argv[2]));
        }
        checkAccessAndEdits(runtime, host, data);
        for (const auto& name : {QStringLiteral("watch.sms"), QStringLiteral("watch.sg"), QStringLiteral("watch.gg")}) {
            const bool gg = name.endsWith(".gg"), sg = name.endsWith(".sg");
            host.unload();
            const auto z80 = writeFixture(data, name, z80Fixture(sg, gg));
            require(host.load(z80, error), "Load " + name + ": " + error);
            host.runFrame();
            checkZ80(runtime, host, sg ? KR_HW_SG : gg ? KR_HW_GG : KR_HW_SMS);
            if (gg) checkLinked(runtime, host);
            else {
                host.unload();
                require(!runtime.compatible(host), "Unload invalidates hardware catalog");
                runtime.stopAll();
                runtime.takeUpdates();
            }
        }
        checkExpansions(runtime, host, data);
        std::puts("PASS Lua runtime: MD, SMS, SG-1000, GG, GG Link A/B, 32X, Firecore, Sega CD and CD+32X");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
