#include "core/LibretroHost.h"
#include "core/MovieTape.h"
#include "script/ScriptRuntime.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool condition, const QString& message) {
    if (!condition) throw std::runtime_error(message.toStdString());
}

// Original 68000 fixture that stays in a tight loop.
QString writeFixture(const QTemporaryDir& data) {
    QByteArray rom(64 * 1024, char(0));
    qToBigEndian<quint32>(0xfffff0, rom.data());
    qToBigEndian<quint32>(0x200, rom.data() + 4);
    rom.replace(0x100, 16, "SEGA GENESIS    ");
    rom[0x1f0] = 'U';
    qToBigEndian<quint16>(0x60fe, rom.data() + 0x200);
    QFile file(data.filePath("timeline.md"));
    require(file.open(QIODevice::WriteOnly) && file.write(rom) == rom.size(), file.fileName());
    return file.fileName();
}

void start(ScriptRuntime& runtime, const LibretroHost& host, quint64 id, const QByteArray& source,
           const QString& path = QStringLiteral("timeline.lua"), const QString& expectedError = {}) {
    runtime.command({ScriptAction::Run, id, path, source}, host);
    const auto updates = runtime.takeUpdates();
    require(!updates.isEmpty(), "Script startup did not report status");
    const auto& last = updates.constLast();
    require(last.id == id &&
            (expectedError.isEmpty() ? last.status == ScriptStatus::Running
                                     : last.status == ScriptStatus::Error && last.text.contains(expectedError)),
            QStringLiteral("Unexpected startup result for script %1: %2").arg(id).arg(last.text));
}

void stop(ScriptRuntime& runtime, const LibretroHost& host, quint64 id) {
    runtime.command({ScriptAction::Stop, id}, host);
    runtime.takeUpdates();
}

QString probe(ScriptRuntime& runtime, const LibretroHost& host, ScriptEvent event = ScriptEvent::Draw) {
    const auto overlay = runtime.event(event, host, 1, host.runClock().frames);
    const auto updates = runtime.takeUpdates();
    require(updates.isEmpty(), updates.isEmpty() ? QString() : updates.constLast().text);
    require(overlay && overlay->commands.size() == 1, "Expected one script observation");
    return overlay->commands.constFirst().text;
}

void checkRestoration(const LibretroHost& host) {
    ScriptRuntime runtime;
    QByteArray startupCapture;
    ScriptHostServices services;
    services.saveAllowed = true;
    services.inputAllowed = true;
    services.captureSave = [&](qsizetype, QString&) {
        startupCapture = runtime.captureTimeline();
        return startupCapture;
    };
    runtime.setHostServices(std::move(services));

    const QByteArray source = R"(
assert(timeline.get() == nil and timeline.status() == nil and timeline.set(0) == nil)
local shared = {label = "test"}
local initial = {
    n = 1, a = shared, b = shared, min = math.mininteger, max = math.maxinteger,
    whole = 1.0, zero = -0.0, fraction = 1/3, [math.mininteger] = "negative key",
    ["bin\0key"] = "\255\0data", [1] = false, flag = true,
}
assert(timeline.define("example state", 7, initial))
assert(not timeline.define("another", 1, 0))
local anchor = assert(savestate.create())
assert(savestate.save(anchor))
initial.n = 999
shared.label = "changed"
local value = timeline.get()
assert(value.n == 1 and value.a.label == "test" and value.a ~= value.b)
assert(value.min == math.mininteger and value.max == math.maxinteger)
assert(math.type(value.min) == "integer" and math.type(value.max) == "integer")
assert(value[math.mininteger] == "negative key" and value[1] == false and value.flag == true)
assert(value["bin\0key"] == "\255\0data" and value.fraction == 1/3)
assert(math.type(value.whole) == "float" and math.type(value.zero) == "float" and 1/value.zero == -math.huge)
value.n = 77
assert(timeline.get().n == 1 and timeline.status() == "current")

local invocations = 0
emu.on("beforeframe", function()
    local data = timeline.get()
    assert(timeline.set(data))
end)
emu.on("frame", function()
    invocations = invocations + 1
    local data = timeline.get()
    if invocations > 1 then data.n = data.n + 1 end
    assert(timeline.set(data))
    data.n = 999
end)
local function describe()
    assert(not timeline.set(nil))
    local data = timeline.get()
    draw.text(0, 0, timeline.status() .. ":" .. data.n .. ":" .. invocations)
end
for _, event in ipairs({"draw", "loadstate", "rewind", "reset"}) do
    emu.on(event, describe)
end
)";
    start(runtime, host, 1, source);
    const auto initial = runtime.captureTimeline();
    require(runtime.hasTimelineData() && !initial.isEmpty() &&
            initial.constData() == startupCapture.constData() && runtime.validateTimeline(initial),
            "Startup temporary save captures declared data before launch completes");

    start(runtime, host, 2, R"(
local ok, reason = timeline.define("example state", 7, 0)
assert(ok == nil and reason)
)");
    require(runtime.captureTimeline().constData() == initial.constData(), "Duplicate ID cannot dirty the snapshot");
    stop(runtime, host, 2);
    require(probe(runtime, host) == "current:1:0", "Initial value and status");

    MovieInput input;
    runtime.beforeFrame(host, 1, 1, input);
    runtime.event(ScriptEvent::Frame, host, 1, 1);
    require(runtime.captureTimeline().constData() == initial.constData(),
            "Equal values preserve cached storage across beforeframe and frame callbacks");
    runtime.event(ScriptEvent::Frame, host, 1, 2);
    const auto changed = runtime.captureTimeline();
    require(changed != initial && runtime.captureTimeline().constData() == changed.constData(),
            "Only actual mutations invalidate the cached snapshot");
    require(probe(runtime, host) == "current:2:2", "Accepted set copies the input value");

    auto malformed = initial;
    malformed.append(char(0));
    require(!runtime.validateTimeline(malformed) && !runtime.restoreTimeline(malformed) &&
            runtime.captureTimeline().constData() == changed.constData() &&
            probe(runtime, host) == "current:2:2", "Malformed snapshot leaves live data and status untouched");
    for (qsizetype size = 1; size < initial.size(); ++size)
        require(!runtime.validateTimeline(initial.first(size)), "Truncated snapshots must fail validation");
    require(!runtime.validateTimeline(QByteArray(256 * 1024 + 1, 'x')), "Reject an oversized aggregate");

    require(runtime.restoreTimeline(initial) && runtime.captureTimeline().constData() == initial.constData() &&
            probe(runtime, host, ScriptEvent::Rewind) == "restored:1:2",
            "Rewind restores registered data while Lua locals keep their values");
    require(runtime.restoreTimeline(changed) && probe(runtime, host, ScriptEvent::LoadState) == "restored:2:2",
            "Load event observes decoded data");
    require(runtime.restoreTimeline(changed) && runtime.captureTimeline().constData() == changed.constData(),
            "Restoring a shared snapshot retains cached storage");
    require(runtime.validateTimeline({}) && runtime.restoreTimeline({}) &&
            probe(runtime, host, ScriptEvent::LoadState) == "missing:1:2", "Old saves restore defaults");
    runtime.resetTimeline();
    require(probe(runtime, host, ScriptEvent::Reset) == "reset:1:2", "Power/reset restores defaults");

    start(runtime, host, 1, source, QStringLiteral("moved.lua"));
    require(runtime.restoreTimeline(changed) && probe(runtime, host) == "restored:2:0",
            "Identical content can restore data after restart and path change");
    // Snapshot header and ID precede the source digest then the schema version.
    auto schemaChanged = changed;
    const auto schemaOffset = 7 + qsizetype(schemaChanged.at(6)) + 32;
    qToLittleEndian<quint32>(8, schemaChanged.data() + schemaOffset);
    require(runtime.validateTimeline(schemaChanged) && runtime.restoreTimeline(schemaChanged) &&
            probe(runtime, host) == "incompatible:1:0", "A mismatched schema uses defaults");
    start(runtime, host, 1, source + "\n-- Changed source");
    require(runtime.restoreTimeline(changed) && probe(runtime, host) == "incompatible:1:0",
            "Changed source cannot use the old data even with the same ID and schema");
    stop(runtime, host, 1);
    require(!runtime.hasTimelineData() && runtime.captureTimeline().isEmpty() &&
            runtime.restoreTimeline(changed) && runtime.captureTimeline().isEmpty(),
            "Valid entries for absent scripts are ignored");
}

void checkCanonicalTables(const LibretroHost& host) {
    ScriptRuntime runtime;
    const QByteArray source = R"(
local t = {}
if emu.framecount() % 2 == 0 then
    t[7], t["n\0l"], t[8], t["\255"] = "foo", 23, false, 2
else
    t["\255"], t[8], t["n\0l"], t[7] = 2, false, 23, "foo"
end
assert(timeline.define("ordered", 1, t))
emu.on("frame", function()
    local copy = timeline.get()
    local different = {["\255"] = copy["\255"], [8] = copy[8], ["n\0l"] = copy["n\0l"]}
    different[7] = copy[7]
    assert(timeline.set(different))
end)
)";
    start(runtime, host, 1, source);
    const auto one = runtime.captureTimeline();
    runtime.event(ScriptEvent::Frame, host, 1, 1);
    require(runtime.captureTimeline().constData() == one.constData(), "Canonical ordering ignores table iteration order");

    const QByteArray other = "assert(timeline.define('another', 1, 42))";
    start(runtime, host, 2, other);
    const auto two = runtime.captureTimeline();
    require(two != one && runtime.validateTimeline(two), "Encode multiple scripts");
    runtime.stopAll();
    runtime.takeUpdates();
    start(runtime, host, 2, other);
    start(runtime, host, 1, source);
    require(runtime.captureTimeline() == two, "Canonical ordering ignores script launch order");
    stop(runtime, host, 2);
    require(runtime.captureTimeline() == one && runtime.restoreTimeline(two) &&
            runtime.captureTimeline() == one, "Filtered snapshots contain only running scripts");
}

void checkBoundsAndErrors(const LibretroHost& host) {
    ScriptRuntime runtime;
    for (const auto& [body, error] : {
             std::pair{"local t = {}; t[1] = t; timeline.define('x', 1, t)", "cycles"},
             {"timeline.define('x', 1, {[{}] = true})", "keys must be integers or strings"},
             {"timeline.define('x', 1, {function() end})", "Unsupported timeline value"},
             {"timeline.define('x', 1, 0/0)", "must be finite"},
             {"timeline.define('x', 1, math.huge)", "must be finite"},
             {"timeline.define('x', 1, {1, string.rep('x', 4096)..'x'})", "4096 bytes"},
             {"local t = {}; for i=1,8 do t={t} end; timeline.define('x', 1, t)", "depth 8"},
             {"local t = {}; for i=1,256 do t[i]=i end; timeline.define('x', 1, t)", "512 nodes"},
             {"local t = {}; for i=1,128 do t[i]=true end; timeline.define('x', 1, {t,t})", "512 nodes"},
             {"local s = string.rep('x', 4096); timeline.define('x', 1, {s,s,s,s})", "16 KiB"},
             {"timeline.define('x', 1.0, 0)", "integer schema version"},
             {"timeline.define('x', 0, 0)", "schema version out of range"},
             {"timeline.define('x', 2147483648, 0)", "schema version out of range"},
             {"timeline.define('bad\\0name', 1, 0)", "printable ASCII"},
             {"timeline.define('', 1, 0)", "printable ASCII"},
             {"timeline.define('x', 1)", "explicit nil"},
         }) {
        start(runtime, host, 1, body, QStringLiteral("bounds.lua"), QString::fromLatin1(error));
        require(runtime.captureTimeline().isEmpty(), "Invalid definition leaves no registered data");
    }

    start(runtime, host, 1, R"(
local t = {}
for i=1,7 do t={t} end
assert(timeline.define("depth", 2147483647, t))
local list = {}
for i=1,255 do list[i] = -i end
assert(timeline.set(list) and #timeline.get() == 255)
assert(timeline.set(nil) and timeline.get() == nil and timeline.status() == "current")
emu.on("frame", function() timeline.set(function() end) end)
)");
    require(runtime.hasTimelineData() && runtime.validateTimeline(runtime.captureTimeline()),
            "Explicit nil remains registered");
    runtime.event(ScriptEvent::Frame, host, 1, 1);
    const auto updates = runtime.takeUpdates();
    require(updates.size() == 1 && updates.first().status == ScriptStatus::Error &&
            !runtime.hasTimelineData() && runtime.captureTimeline().isEmpty(),
            "A failed callback releases the declared data");

    for (int id = 1; id <= ScriptLimits::MaxScripts; ++id) {
        const auto source = QByteArray(R"(
local s = string.rep("x", 4096)
assert(timeline.define(")") + QByteArray::number(id) + R"(", 1, {s, s, s, string.rep("x", 4045)}))
)";
        start(runtime, host, quint64(id), source);
    }
    const auto full = runtime.captureTimeline();
    require(runtime.validateTimeline(full) && full.size() ==
            6 + ScriptLimits::MaxScripts * (1 + 1 + 32 + 4 + 2 + 16 * 1024),
            "All eight entries can reach the exact per-script byte limit");
    require(runtime.restoreTimeline(full) && runtime.captureTimeline().constData() == full.constData(),
            "Largest valid unchanged restore uses the shared cache");
}
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        require(argc == 2, "usage: lua_timeline_tests CORE");
        QTemporaryDir data;
        require(data.isValid(), "Temporary data folder");
        LibretroHost host(QString::fromLocal8Bit(argv[1]), data.path());
        QString error;
        require(host.load(writeFixture(data), error), "Load timeline fixture: " + error);
        host.runFrame();
        checkRestoration(host);
        checkCanonicalTables(host);
        checkBoundsAndErrors(host);
        std::puts("PASS Lua declared timeline values, restoration, compatibility and limits");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
