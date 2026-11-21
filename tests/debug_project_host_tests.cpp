#include "core/LibretroHost.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace {
constexpr quint32 Marker = 0x20;
constexpr quint32 Loop = 0x21a;

void require(bool ok, const QString& detail) {
    if (!ok) throw std::runtime_error(detail.toStdString());
}

QString saveFixture(const QString& path, const QByteArray& data) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(data) == data.size(),
            file.errorString());
    return path;
}

QByteArray mdRom() {
    QByteArray rom(64 * 1024, char(0));
    qToBigEndian<quint32>(0x00fffff0, rom.data());
    qToBigEndian<quint32>(0x200, rom.data() + 4);
    rom.replace(0x100, 16, "SEGA GENESIS    ");
    rom.replace(0x120, 15, "PROJECT FIXTURE");
    qToBigEndian<quint32>(quint32(rom.size() - 1), rom.data() + 0x1a4);
    rom[0x1f0] = 'U';
    const auto code = QByteArray::fromHex(
        "23fc812345e700ff0020"   // MOVE.L #$812345E7,$FF0020
        "33fc800400c00004"       // MOVE.W #$8004,$C00004
        "33fc814400c00004"       // MOVE.W #$8144,$C00004
        "60fe");                // BRA.S *
    rom.replace(0x200, code.size(), code);
    return rom;
}

QByteArray ggRom() {
    QByteArray rom(32 * 1024, char(0));
    rom[0] = char(0xc3);          // JP $0000
    rom.replace(0x7ff0, 8, "TMR SEGA");
    rom[0x7fff] = char(0x6c);
    return rom;
}

DebugWatchEntry watch(quint32 id, quint32 side, quint32 domain, quint32 offset, quint32 width) {
    DebugWatchEntry entry;
    entry.id = id;
    entry.instance = side;
    entry.domain = domain;
    entry.offset = offset;
    entry.width = width;
    return entry;
}

bool hasDomain(const DebugInspectionSnapshot& snapshot, quint32 side, quint32 domain) {
    return std::any_of(snapshot.watchDomains.cbegin(), snapshot.watchDomains.cend(),
        [=](const auto& item) { return item.instance == side && item.native.id == domain; });
}

DebugInspectionRequest watchRequest() {
    DebugInspectionRequest request;
    request.serial = 42;
    request.parts = KR_INSPECT_MEMORY;
    request.length = 0;
    request.watchCatalog = true;
    return request;
}

void testMdWatches(LibretroHost& host) {
    auto request = watchRequest();
    request.watches = {
        watch(1, 0, KR_MEM_RAM, Marker, 1),
        watch(2, 0, KR_MEM_RAM, Marker + 1, 2),
        watch(3, 0, KR_MEM_RAM, Marker, 4),
        watch(4, 0, KR_MEM_VDP_REGS, 0, 1),
        watch(5, 0, KR_MEM_RAM, 0xffff, 2),
        watch(6, 0, KR_MEM_RAM, 0xffffffffu, 4),
        watch(7, 0, KR_MEM_RAM, Marker, 3),
        watch(8, 1, KR_MEM_RAM, Marker, 1),
        watch(9, 0, KR_MEM_CD_PRG, 0, 1),
        watch(10, 0, 0xc00000, 0, 2), // A CPU bus address is not a physical domain.
        watch(0, 0, KR_MEM_RAM, Marker, 1),
    };
    request.watches[2].order = DebugWatchEntry::Little;
    request.watches[2].format = DebugWatchEntry::Signed;
    const auto previous = host.serialize();
    const auto clock = host.runClock();
    require(!previous.isEmpty(), "Serialize the original Mega Drive fixture");

    auto snapshot = host.inspect(request);
    require(snapshot.available() && snapshot.serial == request.serial && snapshot.memory.isEmpty() &&
            snapshot.instanceCount == 1 && snapshot.gameKey == host.debuggerSnapshot().gameKey,
            "Watch-only requests return a game-tagged snapshot without a memory page");
    require(hasDomain(snapshot, 0, KR_MEM_ROM) && hasDomain(snapshot, 0, KR_MEM_RAM) &&
            hasDomain(snapshot, 0, KR_MEM_VDP_REGS) && !hasDomain(snapshot, 1, KR_MEM_RAM) &&
            snapshot.writableDomains.contains(KR_MEM_RAM) && !snapshot.writableDomains.contains(KR_MEM_ROM),
            "Catalog and edit capabilities follow the active console");
    require(snapshot.watchValues.size() == request.watches.size(), "Preserve requested watch order and IDs");
    for (qsizetype i = 0; i < snapshot.watchValues.size(); ++i)
        require(snapshot.watchValues[i].id == request.watches[i].id, "A sample keeps its own watch ID");
    require(snapshot.watchValues[0].bytes == QByteArray::fromHex("81") &&
            snapshot.watchValues[1].bytes == QByteArray::fromHex("2345") &&
            snapshot.watchValues[2].bytes == QByteArray::fromHex("812345e7") &&
            snapshot.watchValues[2].bigEndian && snapshot.watchValues[3].bytes == QByteArray::fromHex("04"),
            "Exact guest bytes for widths 1, 2, 4 and the safe VDP register mirror");
    for (qsizetype i = 4; i < snapshot.watchValues.size(); ++i)
        require(snapshot.watchValues[i].bytes.isEmpty(), "An invalid or unavailable watch is never truncated");

    request.watchCatalog = false;
    request.watches.clear();
    for (int i = 1; i <= DebugWatchLimit + 3; ++i)
        request.watches.push_back(watch(i, 0, KR_MEM_RAM, Marker, 4));
    snapshot = host.inspect(request);
    require(snapshot.watchDomains.isEmpty() && snapshot.watchValues.size() == DebugWatchLimit &&
            snapshot.watchValues.front().id == 1 && snapshot.watchValues.back().id == DebugWatchLimit &&
            snapshot.watchValues.back().bytes == QByteArray::fromHex("812345e7"),
            "The host clamps watches to 128 samples and omits an unrequested catalog");
    request.watches.resize(DebugWatchLimit);
    constexpr int repeats = 1000;
    qsizetype samples = 0;
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < repeats; ++i)
        samples += host.inspect(request).watchValues.size();
    const auto elapsed = timer.nsecsElapsed();
    require(samples == repeats * DebugWatchLimit, "Every timed snapshot supplies the requested values");
    std::printf("%d snapshots of %d four-byte watches, host only: %.3f us/snapshot\n",
                repeats, DebugWatchLimit, elapsed / (1000.0 * repeats));
    require(host.serialize() == previous && host.runClock().frames == clock.frames &&
            host.runClock().ticks == clock.ticks, "Watch reads do not advance or mutate the guest");
}

void expectReply(LibretroHost& host, const DebuggerCommand& command, bool expected) {
    QString error;
    const auto accepted = host.debuggerCommand(command, error);
    const auto snapshot = host.debuggerSnapshot();
    require(accepted == expected && error.isEmpty() == expected &&
            snapshot.configureRequestId == command.configureRequestId &&
            snapshot.configureAccepted == expected && snapshot.configureError == error,
            QStringLiteral("Project request %1 returned %2 (%3)")
                .arg(command.configureRequestId).arg(accepted).arg(error));
}

void testConfigure(LibretroHost& host, const QString& nextFixture) {
    const auto gameKey = host.debuggerSnapshot().gameKey;
    require(!gameKey.isEmpty(), "Debugger reports a current game key");

    DebuggerBreakpoint point;
    point.id = 1;
    point.cpu = KR_DEBUG_M68K;
    point.events = KR_DEBUG_EXEC;
    point.first = point.last = Loop;
    point.oneShot = true;
    auto disabled = point;
    disabled.id = 2;
    disabled.enabled = false;
    DebuggerCommand request;
    request.kind = DebuggerCommand::Configure;
    request.cpu = KR_DEBUG_M68K;
    request.gameKey = gameKey;
    request.configureRequestId = 100;
    request.breakpoints = {point, disabled};
    expectReply(host, request, true);
    DebuggerCommand initialNavigation;
    initialNavigation.kind=DebuggerCommand::Navigate;
    initialNavigation.address=0x200;
    initialNavigation.followPc=false;
    QString navigationError;
    require(host.debuggerCommand(initialNavigation,navigationError),navigationError);

    require(!host.runDebugFrame() && host.debuggerStopped(), "The new one-shot breakpoint hits inside a real frame");
    const auto hit = host.debuggerSnapshot();
    require(hit.followingPc && hit.address == Loop && hit.breakpoints.size() == 2 &&
            hit.breakpoints.front().hits == 1 && !hit.breakpoints.front().enabled,
            "The runtime counter and one-shot disable are visible");

    auto bad = request;
    bad.configureRequestId = 101;
    bad.breakpoints[1].condition = QStringLiteral("UNKNOWN == 0");
    expectReply(host, bad, false);
    const auto rejection = host.debuggerSnapshot();
    QString error;
    require(hit.instructions.size()>1 && hit.instructionLocations.size()==hit.instructions.size(),
            "Debugger captures physical identities for displayed instructions");
    const auto& mapped=hit.instructionLocations[1];
    require(mapped.source!=0,"Second instruction has a proven mapped source");
    DebuggerCommand navigate;
    navigate.kind=DebuggerCommand::Navigate;
    navigate.cpu=KR_DEBUG_M68K;
    navigate.gameKey=gameKey;
    navigate.address=hit.instructions[1].address;
    navigate.followPc=false;
    navigate.physicalSource=mapped.source;
    navigate.physicalOffset=mapped.offset;
    navigate.navigationRequestId=1;
    require(host.debuggerCommand(navigate,error),error);
    auto navigation=host.debuggerSnapshot();
    require(navigation.navigationRequestId==1 && navigation.navigationAccepted &&
            navigation.navigationAddress==navigate.address && !navigation.followingPc,
            "Physical navigation is acknowledged");
    DebuggerCommand refresh;
    refresh.address=0;
    refresh.followPc=true;
    require(host.debuggerCommand(refresh,error),error);
    navigation=host.debuggerSnapshot();
    require(navigation.navigationAddress==navigate.address && !navigation.followingPc &&
            navigation.navigationRequestId==1,"Queued stale refresh cannot overwrite explicit navigation or its reply");
    navigate.physicalOffset++;
    navigate.navigationRequestId=2;
    require(!host.debuggerCommand(navigate,error) && !error.isEmpty(),"Reject a no-longer-mapped bank without mutation");
    navigation=host.debuggerSnapshot();
    require(navigation.navigationRequestId==2 && !navigation.navigationAccepted &&
            navigation.navigationAddress==navigate.address && navigation.navigationError==error,
            "Failed bank navigation reports a persistent specific reply");
    require(host.debuggerCommand({}, error), error);
    auto snapshot = host.debuggerSnapshot();
    require(snapshot.configureRequestId == 101 && snapshot.configureError == rejection.configureError &&
            !snapshot.configureAccepted && snapshot.partialFrame &&
            snapshot.breakpoints.size() == 2 && !snapshot.breakpoints.front().enabled &&
            snapshot.breakpoints.front().hits == 1 && snapshot.breakpoints.back().condition.isEmpty(),
            "Invalid conditions preserve the suspended frame, acknowledgement and existing breakpoint definitions");

    bad.kind = DebuggerCommand::Validate;
    bad.configureRequestId = 102;
    expectReply(host, bad, false);
    auto validation = request;
    validation.kind = DebuggerCommand::Validate;
    validation.configureRequestId = 103;
    validation.breakpoints = {disabled};
    const auto clock = host.runClock();
    expectReply(host, validation, true);
    snapshot = host.debuggerSnapshot();
    require(snapshot.partialFrame && snapshot.breakpoints.size() == 2 &&
            snapshot.breakpoints.front().hits == 1 && !snapshot.breakpoints.front().enabled &&
            host.runClock().frames == clock.frames && host.runClock().ticks == clock.ticks,
            "Validation acknowledges its result without installing breakpoints or running the guest");

    request.configureRequestId = 104;
    request.resetBreakpointHits = true;
    request.breakpoints[0].hits = 999;
    request.breakpoints[1].hits = 999;
    expectReply(host, request, true);
    snapshot = host.debuggerSnapshot();
    require(snapshot.breakpoints.size() == 2 && snapshot.breakpoints.front().enabled &&
            !snapshot.breakpoints.back().enabled && snapshot.breakpoints.front().hits == 0 &&
            snapshot.breakpoints.back().hits == 0,
            "Project replacement uses requested enabled flags and resets all hit counts");

    require(host.debuggerCommand(initialNavigation,error),error);
    host.reset();
    require(host.debuggerCommand({},error),error);
    require(host.debuggerSnapshot().followingPc,"Reset returns the debugger view to the PC without a polling side effect");
    host.debuggerDetach();
    require(host.load(nextFixture, error), error);
    const auto currentGame = host.debuggerSnapshot().gameKey;
    require(!currentGame.isEmpty() && currentGame != gameKey, "Load an actual different game context");
    request.configureRequestId = 105;
    expectReply(host, request, false);
    snapshot = host.debuggerSnapshot();
    require(snapshot.gameKey == currentGame && !snapshot.attached && snapshot.breakpoints.isEmpty(),
            "A queued project for the earlier game cannot attach or change the new game");

    bad.gameKey = currentGame;
    bad.configureRequestId = 106;
    expectReply(host, bad, false);
    require(!host.debuggerAttached(), "Rejecting an invalid hidden import does not attach the debugger");
    validation.gameKey = currentGame;
    validation.configureRequestId = 107;
    expectReply(host, validation, true);
    require(!host.debuggerAttached() && host.debuggerSnapshot().breakpoints.isEmpty(),
            "Validating a hidden import does not create a live debugger or install breakpoints");

    request.gameKey = currentGame;
    request.configureRequestId = 108;
    expectReply(host, request, true);
    require(host.debuggerSnapshot().breakpoints.size() == 2, "A current project can configure the new game");
}

void setPeerRam(LibretroHost& host, quint32 side, const QByteArray& bytes) {
    DebugInspectionRequest request;
    request.parts = KR_INSPECT_MEMORY;
    request.instance = side;
    request.domain = KR_MEM_ZRAM;
    request.offset = Marker;
    request.length = quint32(bytes.size());
    const auto snapshot = host.inspect(request);
    require(snapshot.available() && snapshot.memory.size() == bytes.size() &&
            snapshot.writableDomains.contains(KR_MEM_ZRAM), "Read the selected Game Gear's work RAM");
    DebugMemoryWrite write;
    write.generation = snapshot.generation;
    write.instance = side;
    write.domain = request.domain;
    write.offset = request.offset;
    write.before = snapshot.memory;
    write.after = bytes;
    QString error;
    require(host.writeInspectionMemory(write, error), error);
}

void testLink(const QString& core, const QTemporaryDir& temporary) {
    const auto path = saveFixture(temporary.filePath("watch.gg"), ggRom());
    LibretroHost host(core, temporary.filePath("gg-watch"));
    QString error;
    require(host.load(path, error) && host.setLinkCableEnabled(true, error), error);
    const auto a = QByteArray::fromHex("6a5c");
    const auto b = QByteArray::fromHex("2b49");
    setPeerRam(host, 0, a);
    setPeerRam(host, 1, b);
    const auto previous = host.serialize();
    require(!previous.isEmpty(), "Serialize the linked pair");
    auto request = watchRequest();
    request.domain = KR_MEM_ZRAM;
    request.watches = {watch(1, 0, KR_MEM_ZRAM, Marker, 2), watch(2, 1, KR_MEM_ZRAM, Marker, 2)};
    for (quint32 selected : {0u, 1u}) {
        request.instance = selected;
        const auto snapshot = host.inspect(request);
        require(snapshot.available() && snapshot.instanceCount == 2 && snapshot.watchValues.size() == 2 &&
                hasDomain(snapshot, 0, KR_MEM_ZRAM) && hasDomain(snapshot, 1, KR_MEM_ZRAM) &&
                snapshot.watchValues[0].bytes == a && snapshot.watchValues[1].bytes == b &&
                !snapshot.watchValues[0].bigEndian && !snapshot.watchValues[1].bigEndian,
                "A and B return their own canonical bytes regardless of the selected page console");
    }
    require(host.serialize() == previous, "Linked watch reads preserve both machines");
    require(host.setLinkCableEnabled(false, error), error);
    request.instance = 0;
    const auto disconnected = host.inspect(request);
    require(disconnected.available() && disconnected.instanceCount == 1 &&
            !hasDomain(disconnected, 1, KR_MEM_ZRAM) && disconnected.watchValues.size() == 2 &&
            disconnected.watchValues[0].bytes == a && disconnected.watchValues[1].bytes.isEmpty(),
            "Disconnect marks B unavailable instead of returning bytes from A");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        require(argc == 2, "Usage: debug_project_host_tests CORE");
        QTemporaryDir temporary;
        require(temporary.isValid(), "Create the watch fixture directory");
        const auto core = app.arguments().at(1);
        const auto image = mdRom();
        const auto path = saveFixture(temporary.filePath("watch.md"), image);
        auto secondImage = image;
        secondImage.replace(0x120, 15, "ANOTHER FIXTURE");
        const auto other = saveFixture(temporary.filePath("new-game.md"), secondImage);
        {
            LibretroHost host(core, temporary.filePath("md-watch"));
            QString error;
            require(host.load(path, error), error);
            host.runFrame();
            testMdWatches(host);
            testConfigure(host, other);
        }
        testLink(core, temporary);
        std::puts("PASS: canonical bounded watches, distinct linked peers and project configure acknowledgements");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
