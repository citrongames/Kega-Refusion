#include "core/EmulationSession.h"
#include "core/MovieSession.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <cstdio>
#include <functional>
#include <stdexcept>

static void require(bool value,const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void waitFor(const std::function<bool()>& done,const char* message) {
    QElapsedTimer timer; timer.start();
    bool ready = false;
    while (!(ready = done()) && timer.elapsed()<8000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    require(ready,message);
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        require(argc==2,"usage: inspection_session_tests CORE");
        QTemporaryDir temp; require(temp.isValid(),"temporary storage");
        QByteArray rom(32768,0);
        qToBigEndian<quint32>(0xffff00,rom.data());
        qToBigEndian<quint32>(0x200,rom.data()+4);
        rom.replace(0x100,16,QByteArray("SEGA MEGA DRIVE  ",16));
        rom.replace(0x1f0,3,"USA");
        rom[0x200]=char(0x60); rom[0x201]=char(0xfe);
        const QString path=temp.filePath("original.md");
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write(rom)==rom.size(),"fixture");
        file.close();
        EmulationSession session(QString::fromLocal8Bit(argv[1]),temp.path());
        QString error;
        QObject::connect(&session,&EmulationSession::errorOccurred,&app,[&](const QString& text){error=text;});
        bool loaded=false,paused=false,closed=false;
        QObject::connect(&session,&EmulationSession::gameLoaded,&app,[&]{loaded=true;});
        QObject::connect(&session,&EmulationSession::pausedChanged,&app,[&](bool value){paused=value;});
        QObject::connect(&session,&EmulationSession::gameClosed,&app,[&]{closed=true;});
        session.loadGame(path);
        waitFor([&]{return loaded||!error.isEmpty();},"load timeout");
        require(error.isEmpty(),qPrintable(error));
        session.setPaused(true);
        waitFor([&]{return paused;},"pause timeout");
        DebugInspectionSnapshot snapshot;
        auto request = [&](quint64 serial) {
            session.requestInspection({serial,KR_INSPECT_MEMORY|KR_INSPECT_VIDEO,KR_MEM_RAM,0,256});
            waitFor([&]{return session.takeInspection(snapshot) && snapshot.serial==serial;},
                    "snapshot timeout");
            require(snapshot.available(),"snapshot unavailable");
            require(snapshot.memory.size()==256,"memory window size");
        };
        request(1);
        const auto firstGeneration=snapshot.generation;
        const auto firstFrame=snapshot.frame;
        // Sustained UI changes collapse into a bounded latest request.
        for (quint64 serial=2;serial<=1000;++serial)
            session.requestInspection({serial,KR_INSPECT_MEMORY,KR_MEM_RAM,0,256});
        bool latest=false;
        waitFor([&]{
            if(session.takeInspection(snapshot) && snapshot.serial==1000)latest=true;
            return latest;
        },"latest request lost");
        require(snapshot.frame==firstFrame,"inspection advanced paused machine");
        session.resetGame();
        request(1001);
        require(snapshot.generation>firstGeneration,"reset did not invalidate inspection generation");
        quint64 serial = 1002;
        bool writeDone = false;
        QString writeError;
        QObject::connect(&session, &EmulationSession::debugMemoryWriteFinished, &app,
                         [&](const QString& result) { writeError = result; writeDone = true; });
        const auto edit = [](const DebugInspectionSnapshot& source) {
            DebugMemoryWrite write;
            write.generation = source.generation;
            write.instance = source.instance;
            write.domain = source.memoryDomain;
            write.offset = source.memoryOffset;
            write.before = source.memory.left(2);
            write.after = QByteArray::fromHex("1234");
            return write;
        };
        const auto submit = [&](const DebugMemoryWrite& write, bool accepted) {
            writeDone = false;
            require(session.writeDebugMemory(write), "enqueue memory edit");
            waitFor([&] { return writeDone; }, "memory edit result timeout");
            require(writeError.isEmpty() == accepted, qPrintable(QStringLiteral("Unexpected memory edit result: %1").arg(writeError)));
        };
        require(snapshot.canEdit && snapshot.writableDomains.contains(KR_MEM_RAM), "paused RAM is writable");
        const auto accepted = edit(snapshot);
        const auto pausedFrame = snapshot.frame;
        submit(accepted, true);
        request(serial++);
        require(snapshot.frame == pausedFrame, "memory edit advanced paused emulation");
        require(snapshot.memory.left(2) == accepted.after, "paused write readback");
        submit(accepted, false);
        auto mismatch = edit(snapshot);
        mismatch.before[0] = char(mismatch.before.at(0) ^ 1);
        submit(mismatch, false);

        session.setPaused(false);
        waitFor([&] { return !paused; }, "resume timeout");
        request(serial++);
        require(!snapshot.canEdit, "running memory is read-only");
        submit(edit(snapshot), false);

        DebuggerCommand stop;
        stop.kind = DebuggerCommand::Pause;
        session.debuggerCommand(stop);
        DebuggerSnapshot debug;
        waitFor([&] { return session.takeDebuggerSnapshot(debug) && debug.stopped && debug.partialFrame; },
                "partial debugger stop timeout");
        request(serial++);
        require(!snapshot.canEdit && snapshot.memory.left(2) == accepted.after, "partial frame cannot edit RAM");
        submit(edit(snapshot), false);
        stop.kind = DebuggerCommand::Detach;
        session.debuggerCommand(stop);
        waitFor([&] { return session.takeDebuggerSnapshot(debug) && !debug.attached; }, "detach timeout");

        int movieMode = MovieSession::Off;
        quint64 movieFrame = 0;
        QObject::connect(&session, &EmulationSession::movieStatusChanged, &app,
                         [&](int mode, const QString&, quint64 frame, quint64, double) {
                             movieMode = mode; movieFrame = frame;
                         });
        const QString moviePath = temp.filePath(QStringLiteral("original.krm"));
        session.startMovieRecording(moviePath);
        waitFor([&] { return movieMode == MovieSession::Recording && movieFrame >= 30; }, "recording timeout");
        session.setPaused(true);
        waitFor([&] { return paused; }, "recording pause timeout");
        request(serial++);
        require(!snapshot.canEdit, "recording cannot edit RAM while paused");
        submit(edit(snapshot), false);
        session.stopMovie();
        waitFor([&] { return movieMode == MovieSession::Off; }, "stop recording timeout");

        session.playMovie(moviePath);
        waitFor([&] { return movieMode == MovieSession::Playback; }, "playback timeout");
        session.setPaused(true);
        waitFor([&] { return paused; }, "playback pause timeout");
        request(serial++);
        require(!snapshot.canEdit, "playback cannot edit RAM while paused");
        submit(edit(snapshot), false);
        session.stopMovie();
        waitFor([&] { return movieMode == MovieSession::Off; }, "stop playback timeout");

        session.requestInspection({});
        QThread::msleep(30);
        require(!session.takeInspection(snapshot),"cancel retained a snapshot");
        session.closeGame();
        waitFor([&]{return closed;},"close timeout");
        session.requestInspection({serial,KR_INSPECT_MEMORY,KR_MEM_RAM,0,256});
        bool empty=false;
        waitFor([&]{
            if(session.takeInspection(snapshot) && snapshot.serial==serial)empty=true;
            return empty;
        },"empty context timeout");
        require(!snapshot.available() && snapshot.memory.isEmpty() && snapshot.blocks.isEmpty(),
                "closed game leaked inspection memory");
        session.shutdown();
        puts("inspection_session_tests: passed");
        return 0;
    } catch(const std::exception& error) {
        fprintf(stderr,"inspection_session_tests: %s\n",error.what());
        return 1;
    }
}
