#include "core/EmulationSession.h"
#include "core/MovieSession.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <atomic>
#include <cstdio>
#include <stdexcept>

namespace {

void require(bool condition, const QString& detail) {
    if (!condition) throw std::runtime_error(detail.toStdString());
}

constexpr quint32 loopAddress = 0x23e;

QByteArray makeRom() {
    QByteArray rom(32 * 1024, char(0xff));
    qToBigEndian<quint32>(0x00fffff0, rom.data());
    qToBigEndian<quint32>(0x00000200, rom.data() + 4);
    rom.replace(0x100, 16, "SEGA GENESIS    ");
    rom.replace(0x120, 16, "DBG SESSION TEST");
    rom.replace(0x180, 2, "GM");
    qToBigEndian<quint32>(quint32(rom.size() - 1), rom.data() + 0x1a4);
    rom[0x1f0] = 'U';

    // Original MD code sets a colored backdrop and feeds three PSG tone
    // bytes, then loops at a fixed instruction for repeatable mid-frame stops.
    const auto program = QByteArray::fromHex(
        "46fc2700"             // MOVE.W #$2700,SR
        "33fc800400c00004"     // Enable Mode 5.
        "33fc814400c00004"     // Enable display.
        "23fcc000000000c00004" // Select CRAM backdrop.
        "33fc000e00c00000"     // Write backdrop color.
        "13fc008d00c00011"     // PSG low tone bits.
        "13fc001200c00011"     // PSG high tone bits.
        "13fc009300c00011"     // PSG attenuation.
        "60fe");               // BRA.S *
    require(0x200 + qsizetype(program.size()) - 2 == loopAddress, "Fixture breakpoint address");
    rom.replace(0x200, program.size(), program);
    return rom;
}

void writeRom(const QString& path) {
    const auto rom = makeRom();
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(rom) == rom.size(),
            "Create original debugger-session ROM");
}

struct Observations {
    struct Counts {
        quint64 clock;
        unsigned video, audio;
    };

    quint64 clock = 0;
    unsigned actions = 0, pauseNotices = 0, debuggerNotifications = 0;
    int movieMode = MovieSession::Off;
    bool loaded = false, paused = false;
    QString error;
    std::atomic_uint video{0}, audio{0};
    std::atomic_bool pauseOnNextPicture{false};

    Counts counts() const { return {clock, video.load(), audio.load()}; }

    void connect(EmulationSession& session) {
        QObject::connect(&session, &EmulationSession::gameLoaded, &session, [&] { loaded = true; });
        QObject::connect(&session, &EmulationSession::pausedChanged, &session,
                         [&](bool value) { paused = value; ++pauseNotices; });
        QObject::connect(&session, &EmulationSession::runTimerChanged, &session,
                         [&](quint64 frames, quint64) { clock = frames; });
        QObject::connect(&session, &EmulationSession::actionCompleted, &session, [&] { ++actions; });
        QObject::connect(&session, &EmulationSession::errorOccurred, &session,
                         [&](const QString& message) { error = message; });
        QObject::connect(&session, &EmulationSession::debuggerReady, &session,
                         [&] { ++debuggerNotifications; });
        QObject::connect(&session, &EmulationSession::movieStatusChanged, &session,
                         [&](int mode, const QString&, quint64, quint64, double) { movieMode = mode; });
        QObject::connect(&session, &EmulationSession::audioSamples, &session,
                         [&](const QByteArray&, int) { ++audio; }, Qt::DirectConnection);
        QObject::connect(&session, &EmulationSession::videoFrame, &session,
                         [&](const QImage&, double) {
            ++video;
            if (pauseOnNextPicture.exchange(false)) session.setPaused(true);
        }, Qt::DirectConnection);
    }
};

template<typename Condition>
void waitFor(Observations& observations, Condition complete, const QString& detail) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        require(observations.error.isEmpty(), detail + QStringLiteral(" — ") + observations.error);
        if (complete()) return;
        QThread::msleep(2);
    } while (timer.elapsed() < 5000);
    require(false, detail);
}

template<typename Condition>
DebuggerSnapshot waitSnapshot(EmulationSession& session, Observations& observations, Condition complete,
                              const QString& detail) {
    DebuggerSnapshot snapshot;
    waitFor(observations, [&] {
        if (!session.takeDebuggerSnapshot(snapshot)) return false;
        require(snapshot.status.isEmpty(), detail + QStringLiteral(" — ") + snapshot.status);
        return complete(snapshot);
    }, detail);
    return snapshot;
}

void send(EmulationSession& session, DebuggerCommand::Kind kind) {
    DebuggerCommand command;
    command.kind = kind;
    command.cpu = KR_DEBUG_M68K;
    session.debuggerCommand(command);
}

void requireSameCounts(Observations::Counts before, Observations::Counts after) {
    require(after.clock == before.clock && after.video == before.video && after.audio == before.audio,
            "A suspended instruction advanced the public clock, video or PCM");
}

bool partialStop(const DebuggerSnapshot& snapshot) {
    return snapshot.attached && snapshot.stopped && snapshot.partialFrame &&
           snapshot.selectedCpu == KR_DEBUG_M68K && snapshot.event == KR_DEBUG_EXEC &&
           snapshot.address == loopAddress;
}

DebuggerSnapshot suspendFrame(EmulationSession& session, Observations& observations) {
    DebuggerCommand settings;
    settings.kind = DebuggerCommand::Configure;
    DebuggerBreakpoint breakpoint;
    breakpoint.id = 1;
    breakpoint.cpu = KR_DEBUG_M68K;
    breakpoint.events = KR_DEBUG_EXEC;
    breakpoint.first = breakpoint.last = loopAddress;
    settings.breakpoints = {breakpoint};
    session.debuggerCommand(settings);
    waitSnapshot(session, observations, [](const auto& snapshot) {
        return snapshot.attached && !snapshot.partialFrame && snapshot.breakpoints.size() == 1;
    }, "Arm the debugger while paused");

    const auto before = observations.counts();
    session.advanceFrame();
    const auto snapshot = waitSnapshot(session, observations, partialStop, "Break in a single frame");
    require(snapshot.frame == before.clock, "Partial snapshot reports the previous completed frame number");
    requireSameCounts(before, observations.counts());
    return snapshot;
}

quint64 cpuCycles(const DebuggerSnapshot& snapshot) {
    for (const auto& cpu : snapshot.cpus)
        if (cpu.cpu == KR_DEBUG_M68K) return cpu.cycles;
    throw std::runtime_error("Suspended 68000 is missing from the debugger snapshot");
}

void stepInsideFrame(EmulationSession& session, Observations& observations, DebuggerSnapshot& stopped) {
    const auto before = observations.counts();
    const auto cycles = cpuCycles(stopped);
    send(session, DebuggerCommand::StepInto);
    stopped = waitSnapshot(session, observations, [&](const auto& snapshot) {
        return partialStop(snapshot) && cpuCycles(snapshot) > cycles;
    }, "Step inside the suspended self-branch frame");
    require(stopped.frame == before.clock, "Instruction step exposed an unfinished frame number");
    requireSameCounts(before, observations.counts());
}

void runSession(const QString& core, const QTemporaryDir& directory) {
    const QString rom = directory.filePath("original-debug-session.md");
    const QString save = directory.filePath("debug-boundary.krs");
    const QString movie = directory.filePath("debug-frames.krm");
    writeRom(rom);

    Observations observations;
    EmulationSession session(core, directory.filePath("data"));
    observations.connect(session);
    session.setRunTimerEnabled(true);
    session.setRewindEnabled(false); // Manual frame history still captures both boundaries.
    observations.pauseOnNextPicture = true;
    session.loadGame(rom);
    waitFor(observations, [&] { return observations.loaded && observations.paused && observations.clock == 1; },
            "Pause at the first published frame");
    require(observations.video.load() == 1, "Startup published an extra frame");

    auto stopped = suspendFrame(session, observations);
    stepInsideFrame(session, observations, stopped);
    stepInsideFrame(session, observations, stopped);
    require(observations.debuggerNotifications > 0, "Debugger mailbox did not notify the observer");

    const auto beforeSave = observations.counts();
    unsigned action = observations.actions;
    session.saveStateToFile(save);
    waitFor(observations, [&] { return observations.actions > action && QFile::exists(save); },
            "Save after draining a suspended frame");
    const auto saved = observations.counts();
    require(saved.clock == beforeSave.clock + 1 && saved.video == beforeSave.video + 1 &&
            saved.audio == beforeSave.audio, "Save completed exactly one suspended manual frame");

    stopped = suspendFrame(session, observations);
    action = observations.actions;
    session.loadStateFromFile(save);
    waitFor(observations, [&] { return observations.actions > action && observations.clock == saved.clock; },
            "Load a complete checkpoint while stopped inside the following frame");
    send(session, DebuggerCommand::Refresh);
    waitSnapshot(session, observations, [&](const auto& snapshot) {
        return !snapshot.partialFrame && snapshot.generation > stopped.generation &&
               snapshot.frame == saved.clock;
    }, "State load invalidates the suspended debugger generation");

    suspendFrame(session, observations);
    const auto beforeBack = observations.counts();
    session.backFrame();
    waitFor(observations, [&] {
        return observations.video.load() >= beforeBack.video + 2 && observations.clock == saved.clock;
    }, "Drain the partial frame and rewind back to its starting boundary");
    require(observations.video.load() == beforeBack.video + 2 && observations.audio.load() == beforeBack.audio,
            "Rewind published one completed frame and one restored image");

    stopped = suspendFrame(session, observations);
    const auto beforeReset = observations.counts();
    action = observations.actions;
    session.hardResetGame();
    waitFor(observations, [&] { return observations.actions > action && observations.clock == 0; },
            "Power reset from a suspended frame");
    require(observations.video.load() == beforeReset.video + 1 && observations.audio.load() == beforeReset.audio,
            "Reset drained exactly one complete manual frame");
    send(session, DebuggerCommand::Refresh);
    const auto reset = waitSnapshot(session, observations, [&](const auto& snapshot) {
        return !snapshot.partialFrame && snapshot.generation > stopped.generation && snapshot.frame == 0;
    }, "Power reset publishes a fresh debugger generation");
    require(reset.attached && reset.breakpoints.size() == 1 && reset.breakpoints.front().enabled,
            "Power reset preserves the active breakpoint");

    send(session, DebuggerCommand::Detach);
    waitSnapshot(session, observations, [](const auto& snapshot) {
        return !snapshot.attached && !snapshot.partialFrame;
    }, "Detach before recording the uninterrupted first frame");

    const auto beforeMovie = observations.counts();
    const auto beforeMoviePause = observations.pauseNotices;
    observations.pauseOnNextPicture = true;
    session.startMovieRecording(movie);
    waitFor(observations, [&] {
        return observations.movieMode == MovieSession::Recording && observations.paused &&
               observations.pauseNotices >= beforeMoviePause + 2 &&
               observations.video.load() > beforeMovie.video;
    }, "Record the first frame and pause");
    require(observations.video.load() == beforeMovie.video + 1, "Recording advanced past its first frame");

    constexpr unsigned recordedButton = 1u << 6;
    session.setButtons(0, recordedButton);
    stopped = suspendFrame(session, observations);
    stepInsideFrame(session, observations, stopped);
    const auto beforeStopMovie = observations.counts();
    session.stopMovie();
    waitFor(observations, [&] { return observations.movieMode == MovieSession::Off; },
            "Stop recording while the second frame is suspended");
    require(observations.clock == beforeStopMovie.clock + 1 &&
            observations.video.load() == beforeStopMovie.video + 1 &&
            observations.audio.load() == beforeStopMovie.audio, "Movie stop publishes the pending frame once");

    MovieTape tape;
    QString error;
    const bool loaded = tape.load(movie, error);
    require(loaded, QStringLiteral("Read finished recording — ") + error);
    MovieInput lastInput;
    require(tape.size() == 2 && tape.resumeFrame() == 2 && tape.inputAt(1, lastInput) &&
            lastInput.buttons[0] == recordedButton, "Recording holds precisely two committed inputs and the matching cursor");

    suspendFrame(session, observations);
    const auto beforeShutdown = observations.counts();
    QElapsedTimer closing;
    closing.start();
    session.shutdown();
    require(closing.elapsed() < 5000 && !session.isRunning() && !session.hasGame(),
            "Shutdown drains the suspended emulator and releases worker resources");
    require(observations.video.load() == beforeShutdown.video + 1 &&
            observations.audio.load() == beforeShutdown.audio, "Shutdown commits the pending manual frame once");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        std::fprintf(stderr, "Usage: debug_execution_session_tests CORE\n");
        return 2;
    }
    try {
        QTemporaryDir temp;
        require(temp.isValid(), "Create isolated debugger-session directory");
        runSession(app.arguments().at(1), temp);
        std::puts("PASS: suspended-frame publication, save/load, rewind, reset, movie cursor and shutdown");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
