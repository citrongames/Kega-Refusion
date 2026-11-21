#include "core/EmulationSession.h"
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
        session.requestInspection({});
        QThread::msleep(30);
        require(!session.takeInspection(snapshot),"cancel retained a snapshot");
        session.closeGame();
        waitFor([&]{return closed;},"close timeout");
        session.requestInspection({1002,KR_INSPECT_MEMORY,KR_MEM_RAM,0,256});
        bool empty=false;
        waitFor([&]{
            if(session.takeInspection(snapshot) && snapshot.serial==1002)empty=true;
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
