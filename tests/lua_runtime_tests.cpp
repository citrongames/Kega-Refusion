#include "core/LibretroHost.h"
#include "script/ScriptRuntime.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
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
    require(host.setLinkCableEnabled(false, error), "Disable GG Link: " + error);
    require(!runtime.compatible(host), "Link teardown invalidates peer catalog");
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
        if (argc == 3) {
            for (const QString& example : {QStringLiteral("memory-hud.lua"), QStringLiteral("fixture-hitbox.lua")}) {
                QFile source(QString::fromLocal8Bit(argv[2]) + QLatin1Char('/') + example);
                require(source.open(QIODevice::ReadOnly), "Open " + example);
                expectStatus(run(runtime, host, 1, source.readAll()), 1, ScriptStatus::Running);
                require(bool(frame(runtime, host)), "Run " + example);
                runtime.stopAll();
                runtime.takeUpdates();
            }
        }
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
