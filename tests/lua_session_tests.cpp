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

static void check(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
static void waitFor(const std::function<bool()>& predicate, const char* text) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed()<10000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    check(predicate(),text);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    try {
        check(argc==2,"usage: lua_session_tests CORE");
        QTemporaryDir temp;
        check(temp.isValid(),"temp folder");
        QByteArray rom(32768,char(0));
        qToBigEndian<quint32>(0xffff00,rom.data());
        qToBigEndian<quint32>(0x200,rom.data()+4);
        rom.replace(0x100,16,QByteArray("SEGA MEGA DRIVE ",16));
        rom.replace(0x1f0,3,"USA");
        rom[0x200]=char(0x60);rom[0x201]=char(0xfe);
        QFile out(temp.filePath("original.md"));
        check(out.open(QIODevice::WriteOnly) && out.write(rom)==rom.size(),"fixture");
        out.close();
        EmulationSession session(QString::fromLocal8Bit(argv[1]),temp.path());
        QVector<ScriptUpdate> updates;
        ScriptOverlayPtr overlay;
        QImage image;
        double aspect=0;
        bool loaded=false,paused=false,closed=false;
        QObject::connect(&session,&EmulationSession::gameLoaded,&app,[&]{loaded=true;});
        QObject::connect(&session,&EmulationSession::gameClosed,&app,[&]{closed=true;});
        QObject::connect(&session,&EmulationSession::pausedChanged,&app,[&](bool value){paused=value;});
        QObject::connect(&session,&EmulationSession::scriptsReady,&app,[&]{updates+=session.takeScriptUpdates();});
        QObject::connect(&session,&EmulationSession::videoFrameReady,&app,[&]{
            session.takeVideoFrame(image,aspect,nullptr,nullptr,&overlay);
        });
        auto status=[&](quint64 id,ScriptStatus wanted) {
            for (auto i=updates.crbegin();i!=updates.crend();++i)
                if (i->id==id && i->status!=ScriptStatus::Log) return i->status==wanted;
            return false;
        };
        session.loadGame(out.fileName());
        waitFor([&]{return loaded && !image.isNull();},"game load");
        session.setPaused(true);
        waitFor([&]{return paused;},"pause");
        ScriptCommand run;
        run.id=1;run.path="frame-hud.lua";
        run.source=R"(
assert(memory.read(1,0x100)==0x53)
local function render(e)
    draw.text(2,2,tostring(e.frame),0xffffffff)
    draw.rect(20,20,12,12,0xff0000ff,0x00ff0080)
end
emu.on('draw',render)
emu.on('frame',render)
emu.on('rewind',function(e) print('rewind'); render(e) end)
emu.on('loadstate',render)
emu.on('reset',render)
)";
        check(session.scriptCommand(run),"run queue");
        waitFor([&]{return status(1,ScriptStatus::Running) && overlay && overlay->commands.size()==2;},"initial HUD");
        check(overlay->commands[0].text==QString::number(overlay->frame),"initial frame identity");
        const auto initial=overlay->frame;
        session.advanceFrame();
        waitFor([&]{return overlay && overlay->frame>initial;},"step forward");
        check(overlay->commands[0].text==QString::number(overlay->frame),"forward frame identity");
        const auto forward=overlay->frame;
        session.backFrame();
        waitFor([&]{return overlay && overlay->frame<forward;},"rewind redraw");
        check(overlay->commands[0].text==QString::number(overlay->frame),"rewind frame identity");
        session.saveStateToFile(temp.filePath("checkpoint.krs"));
        waitFor([&]{return QFile::exists(temp.filePath("checkpoint.krs"));},"save");
        session.advanceFrame();
        waitFor([&]{return overlay && overlay->frame>=forward;},"forward after save");
        session.loadStateFromFile(temp.filePath("checkpoint.krs"));
        waitFor([&]{return !overlay;},"predictive load preview is clean");
        session.advanceFrame();
        waitFor([&]{return overlay && overlay->frame==forward;},"load resumes matching HUD");

        ScriptCommand bad=run;bad.id=2;bad.source="while true do end";
        check(session.scriptCommand(bad),"bad queue");
        waitFor([&]{return status(2,ScriptStatus::Error);},"runaway containment");
        check(status(1,ScriptStatus::Running),"unrelated script survived");
        const auto before=overlay->frame;
        session.advanceFrame();
        waitFor([&]{return overlay && overlay->frame>before;},"game survives runaway");
        ScriptCommand stop;stop.action=ScriptAction::StopAll;
        check(session.scriptCommand(stop),"stop queue");
        waitFor([&]{return status(1,ScriptStatus::Stopped) && !overlay;},"stop clears paused HUD");

        // A start already queued for the old game cannot attach to its successor.
        loaded=false;
        session.loadGame(out.fileName());
        run.id=3;
        check(session.scriptCommand(run),"stale queue");
        waitFor([&]{return loaded && status(3,ScriptStatus::Error);},"old generation rejected");
        session.setPaused(true);waitFor([&]{return paused;},"second pause");
        run.id=4;
        check(session.scriptCommand(run),"fresh queue");
        waitFor([&]{return status(4,ScriptStatus::Running);},"fresh context");
        check(session.scriptCommand(stop),"stop before network");
        waitFor([&]{return status(4,ScriptStatus::Stopped);},"stop before topology change");
        bool networkApplied=false;
        QObject::connect(&session,&EmulationSession::networkModeApplied,&app,[&](int mode){networkApplied=mode==1;});
        QObject::connect(&session,&EmulationSession::networkSnapshotReady,&app,[&]{session.acknowledgeNetworkSnapshot();});
        session.setNetworkRole(1);
        session.setNetworkMode(1);
        run.id=5;
        check(session.scriptCommand(run),"cold topology queued run");
        waitFor([&]{return networkApplied && status(5,ScriptStatus::Error);},"cold topology invalidates queued script");
        run.id=6;
        check(session.scriptCommand(run),"local read-only network script");
        waitFor([&]{return status(6,ScriptStatus::Running) && overlay;},"network script enabled");
        session.setNetworkMode(0);
        waitFor([&]{return !session.networkActive();},"network exit");
        session.closeGame();
        waitFor([&]{return closed && status(6,ScriptStatus::Stopped);},"close frees scripts");
        session.shutdown();
        std::puts("PASS Lua session: exact frame packets, pause/step/rewind/load, runaway isolation, stop and game lifetime");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1;
    }
}
