#include "core/EmulationSession.h"
#include "core/LibretroHost.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>

namespace {
void require(bool ok, const QString& detail) {
    if (!ok) throw std::runtime_error(detail.toStdString());
}

QString saveFixture(const QString& path, const QByteArray& data) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(data) == data.size(),
            file.errorString());
    return path;
}

constexpr quint32 initPc = 0x200;
constexpr quint32 dataOffset = 0x400;
constexpr quint32 choiceOffset = 0x20;
constexpr quint32 alternatePc = 0x600;
constexpr quint32 alternateLoop = 0x618;

QByteArray mdRom() {
    QByteArray rom(64 * 1024, char(0xff));
    qToBigEndian<quint32>(0x00fffff0, rom.data());
    qToBigEndian<quint32>(initPc, rom.data() + 4);
    rom.replace(0x100, 16, "SEGA GENESIS    ");
    rom.replace(0x120, 16, "COVERAGE FIXTURE");
    qToBigEndian<quint32>(quint32(rom.size() - 1), rom.data() + 0x1a4);
    rom[0x1f0] = 'U';

    quint32 pc = initPc;
    const auto word = [&](quint16 value) {
        qToBigEndian(value, rom.data() + pc);
        pc += 2;
    };
    const auto address = [&](quint32 value) {
        word(quint16(value >> 16));
        word(quint16(value));
    };
    const auto writeWord = [&](quint16 value, quint32 target) {
        word(0x33fc); word(value); address(target); // MOVE.W #value,(address).L
    };

    word(0x46fc); word(0x2700);                     // Mask interrupts.
    writeWord(0x8004, 0xc00004);
    writeWord(0x8144, 0xc00004);                    // Mode 5 display.
    word(0x3039); address(dataOffset);             // MOVE.W (ROM).L,D0
    word(0x3239); address(0x10000 + dataOffset + 2); // MOVE.W (mirrored ROM).L,D1
    writeWord(0, 0xff0000 + choiceOffset);
    word(0x4a79); address(0xff0000 + choiceOffset);  // TST.W (RAM).L
    word(0x67f8);                                  // BEQ back to TST.
    word(0x4ef9); address(alternatePc);             // JMP to alternate path.

    pc = alternatePc;
    word(0x3039); address(0x620);
    word(0x23fc); address(0xc0000000); address(0xc00004); // Select CRAM backdrop.
    writeWord(0x000e, 0xc00000);
    require(pc == alternateLoop, "Alternate breakpoint points to its instruction");
    word(0x60fe);                                  // BRA.S *
    qToBigEndian<quint32>(0x12749eb5, rom.data() + dataOffset);
    qToBigEndian<quint16>(0x526a, rom.data() + 0x620);
    return rom;
}

QByteArray ggRom() {
    QByteArray rom(32 * 1024, char(0));
    const auto emitAt = [&](int offset, std::initializer_list<unsigned> bytes) {
        for (const auto byte : bytes) rom[offset++] = char(byte);
    };
    emitAt(0, {0xf3, 0xdb, 0xdc, 0xe6, 0x08, 0xca, 0x00, 0x01, 0xc3, 0x80, 0x01});
    // IN A,($DC); AND $08; JP Z,$0100 on Right; JP $0180 otherwise.
    emitAt(0x100, {0x3a, 0x00, 0x02, 0x76});        // LD A,($0200); HALT
    emitAt(0x180, {0x3a, 0x80, 0x02, 0x76});        // LD A,($0280); HALT
    rom[0x200] = char(0x29);
    rom[0x280] = char(0x73);
    rom.replace(0x7ff0, 8, "TMR SEGA");
    rom[0x7fff] = char(0x6c);
    return rom;
}

DebugCoverageCommand startCoverage(unsigned cpu, unsigned instance = 0) {
    DebugCoverageCommand command;
    command.kind = DebugCoverageCommand::Start;
    command.config.cpu = cpu;
    command.config.instance = instance;
    return command;
}

void coverage(LibretroHost& host, const DebugCoverageCommand& command) {
    QString error;
    require(host.coverageCommand(command, error), error);
}

DebugCoverageSource sourceFor(const DebugCoverageSnapshot& snapshot, unsigned domain) {
    const auto found = std::find_if(snapshot.sources.cbegin(), snapshot.sources.cend(),
                                    [domain](const auto& slot) { return slot.native.inspect_domain == domain; });
    require(found != snapshot.sources.cend(), QStringLiteral("Missing coverage source for domain %1").arg(domain));
    return *found;
}

DebugCoverageSnapshot page(LibretroHost& host, unsigned source, quint32 address, unsigned instance = 0) {
    DebugCoverageCommand command;
    command.instance = instance;
    command.source = source;
    command.offset = address;
    coverage(host, command);
    auto snapshot = host.coverageSnapshot();
    require(snapshot.source == source && snapshot.offset == (address & ~255u) &&
            !snapshot.page.isEmpty() && snapshot.page.size() <= 256,
            "Return the selected, aligned physical coverage page");
    return snapshot;
}

void chooseAlternate(LibretroHost& host) {
    DebugInspectionRequest request;
    request.parts = KR_INSPECT_MEMORY;
    request.domain = KR_MEM_RAM;
    request.offset = choiceOffset;
    request.length = 2;
    const auto snapshot = host.inspect(request);
    require(snapshot.available() && snapshot.memory == QByteArray(2, char(0)), "Read the guest's branch selector");
    DebugMemoryWrite write;
    write.generation = snapshot.generation;
    write.domain = request.domain;
    write.offset = request.offset;
    write.before = snapshot.memory;
    write.after = QByteArray::fromHex("0001");
    QString error;
    require(host.writeInspectionMemory(write, error), error);
}

void testMd(const QString& core, const QTemporaryDir& temp, const QString& path) {
    QString error;
    QByteArray expected;
    QImage picture;
    QByteArray pcm;
    {
        LibretroHost baseline(core, temp.filePath("md-reference"));
        require(baseline.load(path, error), error);
        baseline.runFrame();
        baseline.runFrame();
        expected = baseline.captureRewindState();
        picture = baseline.frame();
        pcm = baseline.audio();
        require(!expected.isEmpty() && !picture.isNull() && !pcm.isEmpty(), "Capture real baseline outputs");
    }
    LibretroHost host(core, temp.filePath("md-coverage"));
    require(host.load(path, error), error);
    const auto start = startCoverage(KR_DEBUG_M68K);
    coverage(host, start);
    host.runFrame();
    auto snapshot = host.coverageSnapshot();
    require(snapshot.available && snapshot.active && snapshot.hasCapture && snapshot.canNavigate,
            "Available CPU coverage and physical navigation");
    const quint64 captureId = snapshot.captureId;
    const auto romId = sourceFor(snapshot, KR_MEM_ROM).native.id;
    const auto ramId = sourceFor(snapshot, KR_MEM_RAM).native.id;
    const auto firstFrame = snapshot.revision;
    host.runFrame();
    require(host.captureRewindState() == expected && host.frame() == picture && host.audio() == pcm &&
            host.runClock().frames == 2, "Coverage preserves completed state, image, PCM and host frame clock");
    snapshot = page(host, romId, initPc);
    require(snapshot.revision == firstFrame && snapshot.page.left(4) == QByteArray(4, char(KR_COV_EXEC)),
            "Repeated paths stay stable and consumed immediate bytes are execution flags only");
    snapshot = page(host, romId, dataOffset);
    require(snapshot.page.left(4) == QByteArray(4, char(KR_COV_READ)) &&
            snapshot.inspectDomain == KR_MEM_ROM && snapshot.inspectOffset == dataOffset,
            "Direct and mirrored ROM data reads mark canonical bytes and navigate there");
    require(page(host, ramId, choiceOffset).page.mid(choiceOffset, 2) == QByteArray(2, char(KR_COV_READ)),
            "Guest RAM reads mark their own physical source");
    const auto code = sourceFor(snapshot, KR_MEM_ROM).code;
    const auto data = sourceFor(snapshot, KR_MEM_ROM).data;

    host.reset();
    snapshot = host.coverageSnapshot();
    require(snapshot.active && snapshot.captureId == captureId && snapshot.boundaries == 1 &&
            sourceFor(snapshot, KR_MEM_ROM).code == code, "Reset retains the union and adds a boundary");
    host.runFrame();
    require(host.restoreRewindState(expected), "Restore the original save boundary");
    snapshot = page(host, romId, alternatePc);
    require(snapshot.active && snapshot.boundaries == 2 && snapshot.page.front() == 0 &&
            sourceFor(snapshot, KR_MEM_ROM).data == data, "Restore retains observed physical coverage");

    chooseAlternate(host);
    const auto beforePreview = host.coverageSnapshot();
    require(host.previewLoadedState(), "Generate the alternate preview frame");
    snapshot = page(host, romId, alternatePc);
    require(host.frame() != picture && snapshot.revision == beforePreview.revision &&
            snapshot.boundaries == beforePreview.boundaries && snapshot.page.front() == 0,
            "Rendered rewind preview reaches unseen code without adding observations or a boundary");
    host.runFrame();
    snapshot = page(host, romId, alternatePc);
    require(snapshot.captureId == captureId && snapshot.page.front() == KR_COV_EXEC &&
            snapshot.page[0x20] == KR_COV_READ &&
            sourceFor(snapshot, KR_MEM_ROM).code > code && sourceFor(snapshot, KR_MEM_ROM).data > data,
            "The same path in a committed frame adds its code and data to the prior union");

    DebugTraceCommand trace;
    trace.kind = DebugTraceCommand::Start;
    trace.config.cpu = KR_DEBUG_M68K;
    trace.config.filters = KR_TRACE_INSTRUCTION;
    trace.config.memoryMiB = 1;
    require(host.traceCommand(trace, error) && host.debuggerAttach(error), error);
    host.runFrame();
    const auto traced = host.traceSnapshot().total;
    require(traced > 0 && host.coverageActive(), "Trace, debugger and coverage coexist");
    host.coverageStop();
    const auto stopped = host.coverageSnapshot().revision;
    host.runFrame();
    require(!host.coverageActive() && host.coverageSnapshot().revision == stopped &&
            host.traceSnapshot().total > traced && host.debuggerAttached(),
            "Detaching coverage preserves live trace and debugger hooks");

    coverage(host, start);
    snapshot = host.coverageSnapshot();
    require(snapshot.captureId != captureId, "New capture gets a distinct ID");
    DebuggerBreakpoint point;
    point.id = 1;
    point.cpu = KR_DEBUG_M68K;
    point.events = KR_DEBUG_EXEC;
    point.first = point.last = alternateLoop;
    DebuggerCommand configure;
    configure.kind = DebuggerCommand::Configure;
    configure.cpu = KR_DEBUG_M68K;
    configure.breakpoints = {point};
    require(host.debuggerCommand(configure, error), error);
    require(!host.runDebugFrame() && host.debuggerSnapshot().address == alternateLoop && host.coverageActive(),
            "A real execution breakpoint still stops with trace and coverage active");
    host.traceStop();
    host.debuggerDetach();
    require(!host.debuggerFramePending() && !host.traceActive() && host.coverageActive(),
            "Detaching the debugger and trace completes the frame without stopping coverage");

    DebugCoverageCommand clear;
    clear.kind = DebugCoverageCommand::Clear;
    clear.captureId = captureId;
    require(!host.coverageCommand(clear, error) && !error.isEmpty() &&
            sourceFor(host.coverageSnapshot(), KR_MEM_ROM).code > 0, "Reject a stale Clear without losing the new union");
    clear.captureId = snapshot.captureId;
    coverage(host, clear);
    require(host.coverageActive() && sourceFor(host.coverageSnapshot(), KR_MEM_ROM).code == 0,
            "Clear keeps the current capture armed");
    host.runFrame();
    require(sourceFor(host.coverageSnapshot(), KR_MEM_ROM).code > 0, "Coverage still records after independent detach");
    host.unload();
    snapshot = host.coverageSnapshot();
    require(!snapshot.active && !snapshot.canNavigate && snapshot.hasCapture &&
            sourceFor(snapshot, KR_MEM_ROM).code > 0, "Unload stops and retains an offline inspection record");
}

void testLink(const QString& core, const QTemporaryDir& temp) {
    const auto path = saveFixture(temp.filePath("coverage.gg"), ggRom());
    LibretroHost host(core, temp.filePath("gg-coverage"));
    QString error;
    require(host.load(path, error) && host.setLinkCableEnabled(true, error), error);
    host.setButtons(0, 0);
    host.setButtons(1, 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    coverage(host, startCoverage(KR_DEBUG_Z80, 1));
    host.runFrame();
    const auto snapshot = host.coverageSnapshot();
    require(snapshot.capturedInstance == 1 && snapshot.instance == 1 && snapshot.instances.size() == 2,
            "Capture selects the actual second Game Gear instance");
    const auto romId = sourceFor(snapshot, KR_MEM_ROM).native.id;
    auto branch = page(host, romId, 0x100, 1);
    require(branch.page.left(4) == QByteArray(4, char(KR_COV_EXEC)) && branch.page[0x80] == 0,
            "Only B executes the Right path");
    branch = page(host, romId, 0x200, 1);
    require(branch.page.front() == KR_COV_READ && branch.page[0x80] == 0,
            "B reads its own ROM data without merging A coverage");
    require(host.setLinkCableEnabled(false, error), error);
    const auto disconnected = host.coverageSnapshot();
    require(!disconnected.active && !disconnected.canNavigate && disconnected.hasCapture &&
            disconnected.capturedInstance == 1 && disconnected.instances.size() == 1,
            "Disconnect stops the B capture and removes its navigation target");
}

template <typename Ready>
void waitFor(Ready ready, const QString& message) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        if (ready()) return;
        QThread::msleep(2);
    }
    require(false, message);
}

void testQueuedExport(const QString& core, const QTemporaryDir& temp, const QString& path) {
    bool loaded = false, paused = false;
    QString error;
    EmulationSession session(core, temp.filePath("coverage-session"));
    QObject::connect(&session, &EmulationSession::gameLoaded, &session, [&] { loaded = true; });
    QObject::connect(&session, &EmulationSession::pausedChanged, &session, [&](bool value) { paused = value; });
    QObject::connect(&session, &EmulationSession::errorOccurred, &session, [&](const QString& detail) { error = detail; });
    session.loadGame(path);
    waitFor([&] { return loaded || !error.isEmpty(); }, "Load the session fixture");
    require(error.isEmpty(), error);
    session.setPaused(true);
    waitFor([&] { return paused; }, "Pause before testing the worker queue");
    session.coverageCommand(startCoverage(KR_DEBUG_M68K));
    DebugCoverageSnapshot snapshot;
    waitFor([&] { return session.takeCoverageSnapshot(snapshot) && snapshot.active; }, "Start worker coverage");
    session.advanceFrame();
    waitFor([&] {
        session.coverageCommand({});
        return session.takeCoverageSnapshot(snapshot) && sourceFor(snapshot, KR_MEM_ROM).code > 0;
    }, "Record a completed frame in worker coverage");
    DebugCoverageCommand command;
    command.kind = DebugCoverageCommand::Export;
    command.captureId = snapshot.captureId;
    command.path = temp.filePath("closed-dialog.kcdl");
    session.coverageCommand(command);
    DebugCoverageCommand stop;
    stop.kind = DebugCoverageCommand::Stop;
    session.coverageCommand(stop);
    waitFor([&] {
        session.coverageCommand({});
        return session.takeCoverageSnapshot(snapshot) && !snapshot.active && !snapshot.writing &&
               QFile::exists(command.path);
    }, "The explicit export survives the stop issued when the viewer closes");
    QFile output(command.path);
    require(output.open(QIODevice::ReadOnly) && output.read(8) == "KRCDL1\r\n" &&
            output.size() > 12, "The worker publishes the exported coverage file");
    session.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        require(argc == 2, "Usage: debug_coverage_host_tests CORE");
        QTemporaryDir temp;
        require(temp.isValid(), "Create temporary coverage fixtures");
        const auto core = app.arguments()[1];
        const auto md = saveFixture(temp.filePath("coverage.md"), mdRom());
        testMd(core, temp, md);
        testLink(core, temp);
        testQueuedExport(core, temp, md);
        std::puts("PASS: coverage continuity, preview exclusion, independent hooks, linked B and queued export");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
