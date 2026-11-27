#include "core/EmulationSession.h"
#include "core/MovieTape.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <cstdio>
#include <functional>
#include <stdexcept>

static void check(bool okay,const char* why) {if(!okay)throw std::runtime_error(why);}
static void waitFor(const std::function<bool()>& predicate,const char* why) {
    QElapsedTimer timer;timer.start();
    while(!predicate() && timer.elapsed()<10000){QCoreApplication::processEvents();QThread::msleep(2);}
    check(predicate(),why);
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        check(argc==2,"usage: lua_automation_tests CORE");
        QTemporaryDir temp;check(temp.isValid(),"temp directory");
        QByteArray rom(32768,char(0));
        qToBigEndian<quint32>(0xffff00,rom.data());qToBigEndian<quint32>(0x200,rom.data()+4);
        rom.replace(0x100,16,QByteArray("SEGA MEGA DRIVE ",16));rom.replace(0x1f0,3,"USA");
        // Original fixture: hold TH high; poll controller data into work RAM.
        const auto code=QByteArray::fromHex("13fc004000a1000913fc004000a1000313f900a1000300ff002060f4");
        rom.replace(0x200,code.size(),code);
        QFile file(temp.filePath("input.md"));
        check(file.open(QIODevice::WriteOnly) && file.write(rom)==rom.size(),"fixture write");file.close();
        EmulationSession session(QString::fromLocal8Bit(argv[1]),temp.path());
        bool loaded=false,paused=false;int movieMode=0;quint64 moviePosition=0;
        QVector<ScriptUpdate> updates;QStringList errors;
        QObject::connect(&session,&EmulationSession::gameLoaded,&app,[&]{loaded=true;});
        QObject::connect(&session,&EmulationSession::pausedChanged,&app,[&](bool p){paused=p;});
        QObject::connect(&session,&EmulationSession::movieStatusChanged,&app,
            [&](int mode,const QString&,quint64 frame,quint64,double){movieMode=mode;moviePosition=frame;});
        QObject::connect(&session,&EmulationSession::errorOccurred,&app,[&](const QString& error){errors+=error;});
        ScriptOverlayPtr graphOverlay;
        QObject::connect(&session,&EmulationSession::videoFrameReady,&app,[&]{QImage image;double aspect;session.takeVideoFrame(image,aspect,nullptr,nullptr,&graphOverlay);});
        QObject::connect(&session,&EmulationSession::scriptsReady,&app,[&]{
            for(const auto& update:session.takeScriptUpdates()) {
                updates.append(update);
                if(update.status==ScriptStatus::Error)std::fprintf(stderr,"Lua %llu: %s\n",static_cast<unsigned long long>(update.id),qPrintable(update.text));
            }
        });
        const auto logged=[&](quint64 id,const QString& text){for(const auto& u:updates)if(u.id==id && u.status==ScriptStatus::Log && u.text==text)return true;return false;};
        const auto status=[&](quint64 id,ScriptStatus value){for(auto i=updates.crbegin();i!=updates.crend();++i)if(i->id==id && i->status!=ScriptStatus::Log)return i->status==value;return false;};
        const auto run=[&](quint64 id,const QByteArray& source){ScriptCommand command;command.id=id;command.path="automation.lua";command.source=source;check(session.scriptCommand(command),"script queue");};
        const auto stop=[&]{ScriptCommand command;command.action=ScriptAction::StopAll;check(session.scriptCommand(command),"stop queue");};
        session.loadGame(file.fileName());session.setPaused(true);
        waitFor([&]{return loaded && paused;},"load paused");
        run(10,R"(
local checkpoint=assert(savestate.create())
assert(savestate.save(checkpoint))
local start=emu.framecount()
assert(input.set(1,{right=true,left=false}))
for i=1,3 do
    assert(emu.frameadvance())
    assert(input.get(1).right)
    assert((memory.read(2,0x20)&8)==0, 'core did not receive right')
end
assert(input.clear(1))
assert(savestate.load(checkpoint))
assert(emu.frameadvance())
assert(emu.framecount()==start+1, 'restored clock')
assert((memory.read(2,0x20)&8)==8, 'override survived restore')
assert(savestate.free(checkpoint))
print('temporary restore passed')
)");
        waitFor([&]{return logged(10,"temporary restore passed");},"temporary save, input and coroutine scheduling");
        check(paused,"frameadvance preserved pause");stop();waitFor([&]{return status(10,ScriptStatus::Stopped);},"stop after automation");
        run(11,"assert(input.set(1,{right=true})); error('intentional')");
        waitFor([&]{return status(11,ScriptStatus::Error);},"script error");
        run(12,"assert(emu.frameadvance()); assert((memory.read(2,0x20)&8)==8); print('error cleanup passed')");
        waitFor([&]{return logged(12,"error cleanup passed");},"errored override released");stop();
        waitFor([&]{return status(12,ScriptStatus::Stopped);},"stop cleanup check");
        // External rewind must not continue a suspended program from its future.
        session.setBackgroundPaused(true);
        run(13,"assert(input.set(1,{right=true})); emu.frameadvance(); error('must not resume')");
        waitFor([&]{return status(13,ScriptStatus::Running);},"waiting coroutine");
        session.resetGame();waitFor([&]{return status(13,ScriptStatus::Stopped);},"external reset interrupts continuation");
        session.setBackgroundPaused(false);
        run(14,R"(
assert(emu.pause())
assert(emu.reset())
assert(emu.frameadvance())
assert(emu.framecount()==1)
assert(emu.reset(true))
assert(emu.frameadvance())
assert(emu.framecount()==1)
assert(emu.pause(false))
print('reset and resume passed')
)");
        waitFor([&]{return logged(14,"reset and resume passed") && !paused;},"script soft/power reset and resume");
        session.setPaused(true);stop();
        waitFor([&]{return paused && status(14,ScriptStatus::Stopped);},"pause after reset test");
        const QString moviePath=temp.filePath("script-input.krm");
        session.startMovieRecording(moviePath);session.setPaused(true);
        waitFor([&]{return movieMode==1 && paused;},"recording paused");
        run(19,"emu.on('draw',function() draw.text(1,1,'doomed') end); assert(emu.reset())");
        waitFor([&]{return status(19,ScriptStatus::Error) && !graphOverlay;},"deferred control rejection removes paused overlay");
        run(20,R"(
local save=assert(savestate.create()); assert(savestate.save(save))
assert(input.set(1,{right=true}))
for i=1,3 do assert(emu.frameadvance()) end
assert(savestate.load(save)); assert(input.clear(1))
assert(emu.frameadvance())
assert(emu.framecount()==1)
-- Replace the abandoned three-frame branch with a neutral frame followed by right.
assert(input.set(1,{right=true}))
assert(emu.frameadvance())
assert((memory.read(2,0x20)&8)==0)
assert(input.clear(1)); assert(savestate.free(save))
print('recorded branch passed')
)");
        waitFor([&]{return logged(20,"recorded branch passed");},"recording and temporary branch restore");
        stop();waitFor([&]{return status(20,ScriptStatus::Stopped);},"stop recording script");
        session.stopMovie();waitFor([&]{return movieMode==0 && !session.hasUnsavedMovie() &&
            !session.movieFileOperationPending() && QFile::exists(moviePath);},"movie saved");
        MovieTape tape;QString error;check(tape.load(moviePath,error),qPrintable(error));
        check(tape.size()==2,"restored movie branch length");
        MovieInput first,second;check(tape.inputAt(0,first)&&tape.inputAt(1,second),"movie inputs");
        check(first.buttons[0]==0 && second.buttons[0]==(1u<<7),"movie contains final scripted controls");
        session.playMovie(moviePath);session.setPaused(true);
        waitFor([&]{return movieMode==2 && paused && moviePosition==0;},"replay paused at start");
        run(21,R"(
assert(not input.set(1,{left=true}), 'playback must reject overrides')
local frame=0
emu.on('beforeframe',function() error('automation reran during playback') end)
emu.on('frame',function()
    frame=frame+1
    local right=frame==2
    assert(input.get(1).right==right)
    assert(((memory.read(2,0x20)&8)==0)==right)
    print('replay '..frame)
end)
)");
        waitFor([&]{return status(21,ScriptStatus::Running);},"playback observer");
        session.advanceFrame();waitFor([&]{return logged(21,"replay 1");},"first recorded frame");
        session.advanceFrame();waitFor([&]{return logged(21,"replay 2")&&movieMode==3;},"second recorded frame and playback end");
        check(paused && errors.isEmpty(),"playback remains paused without session errors");
        stop();waitFor([&]{return status(21,ScriptStatus::Stopped);},"stop playback observer");
        session.stopMovie();waitFor([&]{return movieMode==0;},"finish playback context");
        QFile graph(QFileInfo(QString::fromUtf8(__FILE__)).absoluteDir().filePath("../assets/scripts/examples/input-activity.lua"));
        check(graph.open(QIODevice::ReadOnly),"shipped graph example");
        run(30,graph.readAll());
        waitFor([&]{return status(30,ScriptStatus::Running);},"live graph starts");
        for(int i=0;i<120;++i)session.advanceFrame();
        waitFor([&]{return graphOverlay && graphOverlay->commands.size()>=3 &&
            graphOverlay->commands.front().text.startsWith("Input-poll FPS ") &&
            !graphOverlay->commands.front().text.contains("unavailable");},"shipped FPS graph on real core");
        stop();waitFor([&]{return status(30,ScriptStatus::Stopped);},"stop graph before debugger integration");
        ScriptDebugAnnotations annotations;
        QObject::connect(&session,&EmulationSession::scriptAnnotationsReady,&app,[&]{annotations=session.takeScriptAnnotations();});
        run(40,R"(
local save=assert(savestate.create()); assert(savestate.save(save))
assert(memory.label(1,0x20e,"Controller loop"))
assert(memory.watch(2,0x80,2,"Script scratch"))
assert(memory.write(2,0x80,0x1234,2))
assert(memory.setregister(1,"D0",0x56789abc))
-- A save queued before an edit must not silently undo the new state.
assert(savestate.load(save))
assert(emu.frameadvance())
assert(memory.read(2,0x80,2)==0x1234,'deferred memory edit lost')
assert(memory.registers(1).values.D0==0x56789abc,'deferred register edit lost')
assert(not savestate.load(save),'pre-edit temporary state survived')
print('session mutation passed')
)");
        waitFor([&]{return logged(40,"session mutation passed") && annotations.labels.size()==1 && annotations.watches.size()==1;},"session mutation and annotations");
        check(annotations.labels.front().source!=0,"script label pinned to physical code");
        stop();waitFor([&]{return status(40,ScriptStatus::Stopped) && annotations.labels.isEmpty() && annotations.watches.isEmpty();},"temporary annotations removed");
        run(41,R"(
local hits=0
assert(memory.on(1,"write",0xff0020,nil,function(e)
    assert(e.address==0xff0020)
    assert(memory.read(2,0x20)==e.value)
    hits=hits+1
end))
assert(emu.frameadvance())
assert(hits>0,'native access hook not dispatched')
print('session access passed')
)");
        waitFor([&]{return logged(41,"session access passed");},"native hooks via worker scheduler");
        stop();waitFor([&]{return status(41,ScriptStatus::Stopped);},"stop hooked script");
        session.startMovieRecording(temp.filePath("mutation-policy.krm"));session.setPaused(true);
        waitFor([&]{return movieMode==1 && paused;},"recording mutation policy");
        run(42,"assert(not memory.write(2,0x80,0)); assert(not memory.setregister(1,'D0',0)); print('recording edits rejected')");
        waitFor([&]{return logged(42,"recording edits rejected");},"recording edit policy");
        stop();session.stopMovie();
        waitFor([&]{return movieMode==0 && status(42,ScriptStatus::Stopped);},"recording policy cleanup");
        session.shutdown();
        std::puts("PASS Lua automation: physical input, yielded steps, state/clock, error cleanup, timeline cancellation, recording branch and script-free replay");
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
