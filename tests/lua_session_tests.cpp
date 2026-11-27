#include "core/EmulationSession.h"
#include "core/MovieTape.h"
#include "core/MovieFileTask.h"
#include <atomic>
#include <QCoreApplication>
#include <QDir>
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
// A ready recording result must not wait for per-thread DLL/TLS teardown.
static void checkMovieFileWorker() {
    std::atomic_int exits{0};
    QThreadPool pool;
    pool.setMaxThreadCount(1);
    pool.setExpiryTimeout(-1);
    const auto work = [&] {
        struct OnExit {
            std::atomic_int* exits;
            ~OnExit() { ++*exits; }
        };
        thread_local OnExit cleanup{&exits};
        return QThread::currentThreadId();
    };
    auto first = submitMovieFileTask(pool,work);
    const auto worker = first.get();
    auto second = submitMovieFileTask(pool,work);
    check(second.get()==worker && exits.load()==0,
          "movie result joined or replaced the file worker");
    auto failed = submitMovieFileTask(pool,[]() -> int { throw std::runtime_error("file failure"); });
    bool exception = false;
    try { (void)failed.get(); } catch (const std::runtime_error&) { exception = true; }
    check(exception,"movie worker lost exception");
    std::atomic_bool cancelledDone{false};
    { auto discarded = submitMovieFileTask(pool,[&] { cancelledDone.store(true); }); }
    pool.waitForDone();
    check(exits.load()==1 && cancelledDone.load(),"movie worker did not drain and release at shutdown");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    try {
        check(argc==2,"usage: lua_session_tests CORE");
        checkMovieFileWorker();
        qputenv("KEGA_TEST_MOVIE_FILE_DELAY_MS","200");
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
        ScriptUiSnapshot panels;
        QImage image;
        double aspect=0;
        bool loaded=false,paused=false,closed=false;
        int movieMode=0;
        QObject::connect(&session,&EmulationSession::movieStatusChanged,&app,[&](int mode) { movieMode=mode; });
        QObject::connect(&session,&EmulationSession::gameLoaded,&app,[&]{loaded=true;});
        QObject::connect(&session,&EmulationSession::gameClosed,&app,[&]{closed=true;});
        QObject::connect(&session,&EmulationSession::pausedChanged,&app,[&](bool value){paused=value;});
        QObject::connect(&session,&EmulationSession::scriptsReady,&app,[&]{updates+=session.takeScriptUpdates();});
        QObject::connect(&session,&EmulationSession::videoFrameReady,&app,[&]{
            session.takeVideoFrame(image,aspect,nullptr,nullptr,&overlay);
        });
        QObject::connect(&session,&EmulationSession::scriptUiReady,&app,[&] {
            panels=session.takeScriptUiSnapshot();
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

        auto logged=[&](quint64 id, const QString& message) {
            for (const auto& update : updates)
                if (update.id==id && update.status==ScriptStatus::Log && update.text.contains(message)) return true;
            return false;
        };
        ScriptCommand panelCommand;
        panelCommand.id=8; panelCommand.path="paused-panel.lua";
        panelCommand.source=R"(
local count = 0
local firstFrame = emu.framecount()
local w = assert(ui.window("Tools", {
    {type="button",id="click",text="Write"},
    {type="button",id="reset",text="Reset"},
    {type="checkbox",id="enabled",text="Enabled",value=true},
    {type="label",id="status",text="Ready"}
}))
assert(ui.on(w,function(e)
    if e.event == 'close' then
        assert(not ui.get(w,'status'))
        print('panel closed')
    elseif e.control == 'reset' then
        assert(emu.reset())
        print('panel reset queued')
    elseif e.event == 'click' then
        count = count + 1
        if count == 1 then assert(emu.framecount() == firstFrame) end
        assert(memory.write(2,0x20,0x6b))
        assert(ui.set(w,'status','Clicked '..count))
        print('panel click '..count)
    else
        print('panel change '..tostring(e.value))
    end
end))
local function render()
    draw.text(2,2,tostring(memory.read(2,0x20)),0xffffffff)
end
emu.on('draw',render)
emu.on('frame',render)
emu.on('rewind',render)
)";
        check(session.scriptCommand(panelCommand),"panel start");
        waitFor([&]{return panels.windows.size()==1 && status(8,ScriptStatus::Running);},"panel published");
        const auto firstPanel=panels.windows[0];
        ScriptUiEvent click;
        click.owner=firstPanel.owner; click.run=firstPanel.run;
        click.window=firstPanel.handle;click.epoch=panels.epoch;
        click.sequence=1;click.kind=ScriptUiEventKind::Click;click.control="click";
        check(session.postScriptUiEvent(click),"paused click queue");
        waitFor([&]{return logged(8,"panel click 1") && overlay &&
            overlay->commands.size()==1 && overlay->commands[0].text=="107" &&
            !panels.windows.isEmpty() && panels.windows[0].acknowledged==1;},"paused callback/write/redraw");
        check(paused,"panel unexpectedly resumed game");
        check(panels.windows[0].controls.back().value.toString()=="Clicked 1","panel model update");

        const auto oldEpoch=panels.epoch;
        const auto panelFrame=overlay->frame;
        session.advanceFrame();
        waitFor([&]{return overlay && overlay->frame>panelFrame;},"panel frame step");
        session.backFrame();
        waitFor([&]{return panels.epoch!=oldEpoch;},"panel timeline epoch");
        click.sequence=2;
        check(session.postScriptUiEvent(click),"stale timeline event queue");
        ScriptUiEvent change=click;
        change.epoch=panels.epoch;change.sequence=3;
        change.kind=ScriptUiEventKind::Change;change.control="enabled";change.value=false;
        check(session.postScriptUiEvent(change),"current timeline event queue");
        waitFor([&]{return logged(8,"panel change false");},"paused event after rewind");
        check(!logged(8,"panel click 2"),"old timeline click leaked");
        const auto beforeResetEpoch=panels.epoch;
        ScriptUiEvent resetClick=click;
        resetClick.epoch=panels.epoch;resetClick.sequence=4;resetClick.control="reset";
        check(session.postScriptUiEvent(resetClick),"UI reset queue");
        click.epoch=beforeResetEpoch;click.sequence=5;
        check(session.postScriptUiEvent(click),"click after reset queue");
        waitFor([&]{return panels.epoch!=beforeResetEpoch && logged(8,"panel reset queued");},"UI reset applied");
        change.epoch=panels.epoch;change.sequence=6;change.value=true;
        check(session.postScriptUiEvent(change),"reset barrier event");
        waitFor([&]{return !panels.windows.isEmpty() && panels.windows[0].acknowledged==6;},"post-reset event");
        check(!logged(8,"panel click 2"),"same-batch click crossed reset");
        ScriptUiEvent close=change;close.sequence=7;close.kind=ScriptUiEventKind::Close;
        close.control.clear();close.value.clear();
        check(session.postScriptUiEvent(close),"close event queue");
        waitFor([&]{return panels.windows.isEmpty() && logged(8,"panel closed");},"close lifecycle");

        check(session.scriptCommand(panelCommand),"panel restart");
        waitFor([&]{return panels.windows.size()==1 && panels.windows[0].run!=firstPanel.run;},"new run panel");
        const auto newPanel=panels.windows[0];
        close.sequence=8;
        check(session.postScriptUiEvent(close),"old run close queue");
        change.owner=newPanel.owner;change.run=newPanel.run;change.window=newPanel.handle;
        change.epoch=panels.epoch;change.sequence=9;change.value=false;
        check(session.postScriptUiEvent(change),"new run change");
        waitFor([&]{return !panels.windows.isEmpty() && panels.windows[0].acknowledged==9;},"new run survives old close");
        check(session.scriptCommand(stop),"stop panel");
        waitFor([&]{return panels.windows.isEmpty() && status(8,ScriptStatus::Stopped);},"stop destroys panels");

        QFile panelExample(QStringLiteral(KEGA_SOURCE_ROOT "/assets/scripts/examples/08_WindowControls.lua"));
        check(panelExample.open(QIODevice::ReadOnly),"open shipped panel example");
        panelCommand.id=9;panelCommand.path=panelExample.fileName();panelCommand.source=panelExample.readAll();
        check(session.scriptCommand(panelCommand),"run shipped panel example");
        waitFor([&]{return panels.windows.size()==1 && status(9,ScriptStatus::Running) &&
            overlay && overlay->commands.size()==2;},"example window/HUD");
        const auto exampleWindow=panels.windows[0];
        change.owner=exampleWindow.owner;change.run=exampleWindow.run;
        change.window=exampleWindow.handle;change.epoch=panels.epoch;
        change.sequence=10;change.control="period";change.value=1.0;
        check(session.postScriptUiEvent(change),"example sample period");
        waitFor([&]{return panels.windows[0].acknowledged==10;},"example period callback");
        session.advanceFrame();
        waitFor([&]{
            for (const auto& control:panels.windows[0].controls)
                if (control.id=="activity") return !control.points.isEmpty();
            return false;
        },"example graph after frame");
        change.sequence=11;change.control="hud";change.value=false;
        check(session.postScriptUiEvent(change),"example HUD checkbox");
        waitFor([&]{return !overlay && panels.windows[0].acknowledged==11;},"example paused HUD toggled");
        check(status(9,ScriptStatus::Running),"example faulted");
        check(session.scriptCommand(stop),"stop example");
        waitFor([&]{return panels.windows.isEmpty() && status(9,ScriptStatus::Stopped);},"example cleanup");

        // Exercise the shipped watch in the paused UI and its native annotation lifetime.
        ScriptDebugAnnotations annotations;
        QObject::connect(&session,&EmulationSession::scriptAnnotationsReady,&app,[&] {
            annotations=session.takeScriptAnnotations();
        });
        QFile watchExample(QStringLiteral(KEGA_SOURCE_ROOT "/assets/scripts/examples/03_MemoryWatch.lua"));
        check(watchExample.open(QIODevice::ReadOnly),"open shipped memory watch");
        panelCommand.id=10;panelCommand.path=watchExample.fileName();panelCommand.source=watchExample.readAll();
        check(session.scriptCommand(panelCommand),"run memory watch");
        waitFor([&]{return panels.windows.size()==1 && status(10,ScriptStatus::Running) && overlay &&
            overlay->commands.size()==2 && overlay->commands.back().text.startsWith("RAM [0x0]");},"watch shows paused value");
        quint64 watchSequence=0;
        auto watchEvent=[&](ScriptUiEventKind kind,const QString& id,const QVariant& value=QVariant{}) {
            check(panels.windows.size()==1,"memory watch window");
            const auto& window=panels.windows.front();
            ScriptUiEvent event;
            event.owner=window.owner;event.run=window.run;event.window=window.handle;
            event.epoch=panels.epoch;event.sequence=++watchSequence;
            event.kind=kind;event.control=id;event.value=value;
            check(session.postScriptUiEvent(event),"watch event queue");
            if (kind!=ScriptUiEventKind::Close)
                waitFor([&]{return panels.windows.size()==1 &&
                    panels.windows.front().acknowledged==watchSequence;},"watch event dispatched");
        };
        watchEvent(ScriptUiEventKind::Change,"offset",QStringLiteral("1.5"));
        waitFor([&]{return overlay && overlay->commands.back().text=="RAM: no valid address" &&
            annotations.watches.isEmpty();},"fractional memory address handled");
        watchEvent(ScriptUiEventKind::Change,"offset",QStringLiteral("0xFFFFFFFF"));
        waitFor([&]{return overlay && overlay->commands.back().text=="RAM: unavailable";},"large address handled");
        watchEvent(ScriptUiEventKind::Click,"pin");
        check(status(10,ScriptStatus::Running) && annotations.watches.isEmpty(),"out-of-range watch rejected");
        watchEvent(ScriptUiEventKind::Change,"offset",QStringLiteral("0x20"));
        watchEvent(ScriptUiEventKind::Change,"width",3);
        const QString wordLabel=QStringLiteral("RAM [0x20] = 0x");
        waitFor([&]{return overlay && overlay->commands.back().text.startsWith(wordLabel) &&
            overlay->commands.back().text.size()==wordLabel.size()+8;},"four-byte read shown");
        watchEvent(ScriptUiEventKind::Click,"pin");
        waitFor([&]{return annotations.watches.size()==1;},"temporary RAM Watch row published");
        check(annotations.watches.front().offset==0x20 && annotations.watches.front().width==4,"watch row address and width");
        watchEvent(ScriptUiEventKind::Change,"offset",QStringLiteral("0x24"));
        watchEvent(ScriptUiEventKind::Click,"pin");
        waitFor([&]{return annotations.watches.size()==1 && annotations.watches.front().offset==0x24;},
                "watch replaces previous row");
        watchEvent(ScriptUiEventKind::Click,"remove");
        waitFor([&]{return annotations.watches.isEmpty();},"watch removed");
        watchEvent(ScriptUiEventKind::Click,"pin");
        waitFor([&]{return annotations.watches.size()==1;},"watch re-added");
        watchEvent(ScriptUiEventKind::Close,"");
        waitFor([&]{return panels.windows.isEmpty() && annotations.watches.isEmpty() && !overlay;},
                "closing the panel cleans up annotations and overlay");
        check(session.scriptCommand(stop),"stop closed watch");
        waitFor([&]{return status(10,ScriptStatus::Stopped);},"watch stops");

        QFile eventsExample(QStringLiteral(KEGA_SOURCE_ROOT "/assets/scripts/examples/04_Events.lua"));
        check(eventsExample.open(QIODevice::ReadOnly),"open shipped events");
        panelCommand.id=15;panelCommand.path=eventsExample.fileName();panelCommand.source=eventsExample.readAll();
        check(session.scriptCommand(panelCommand),"run events example");
        auto eventLabel=[&](const QString& event) {
            return overlay && overlay->commands.size()==4 &&
                overlay->commands.front().primitive==ScriptPrimitive::Rectangle &&
                overlay->commands[1].text.contains(event);
        };
        waitFor([&]{return status(15,ScriptStatus::Running) && eventLabel("draw @");},"paused draw event");
        session.advanceFrame();
        waitFor([&]{return eventLabel("frame @");},"completed-frame event");
        check(overlay->commands[2].text==QString("Before %1 / Frame %2").arg(overlay->frame-1).arg(overlay->frame),
            "beforeframe reports previous frame");
        session.backFrame();
        waitFor([&]{return eventLabel("rewind @");},"rewind event");
        bool exampleReset=false;
        const auto resetEvent=QObject::connect(&session,&EmulationSession::actionCompleted,&app,
            [&]{exampleReset=true;});
        session.resetGame();
        waitFor([&]{return exampleReset;},"example game reset");
        QObject::disconnect(resetEvent);
        session.advanceFrame();
        waitFor([&]{return eventLabel("frame @") && overlay->commands.back().text.endsWith("reset");},
                "events remembers the reset after the next completed frame");
        check(status(15,ScriptStatus::Running) && paused,"events example preserves pause");
        check(session.scriptCommand(stop),"stop events example");
        waitFor([&]{return status(15,ScriptStatus::Stopped) && !overlay;},"events example stops");

        // Explicit Lua data follows native time; locals/VM stacks do not.
        const QString plainState=temp.filePath("plain-before-lua.krs");
        session.saveStateToFile(plainState);
        waitFor([&]{return QFile::exists(plainState);},"legacy state");
        ScriptCommand tracked;
        tracked.id=11;tracked.path="timeline.lua";
        tracked.source=R"(
assert(timeline.define('session-counter',1,{count=0}))
local function show()
    draw.text(2,2,tostring(timeline.get().count)..'/'..timeline.status())
end
emu.on('frame',function()
    local data=timeline.get()
    data.count=data.count+1
    assert(timeline.set(data))
    show()
end)
emu.on('draw',show)
emu.on('rewind',show)
emu.on('reset',function() print('reset:'..timeline.get().count..'/'..timeline.status()); show() end)
emu.on('loadstate',function() print('loaded:'..timeline.get().count..'/'..timeline.status()); show() end)
)";
        const auto trackedSource=tracked.source;
        check(session.scriptCommand(tracked),"start declared data");
        waitFor([&]{return overlay && overlay->commands.size()==1 && overlay->commands[0].text=="0/current";},"initial declared data");
        auto stepCounter=[&](const QString& expected) {
            session.advanceFrame();
            waitFor([&]{return overlay && overlay->commands.size()==1 && overlay->commands[0].text==expected;},"timeline frame");
        };
        stepCounter("1/current");stepCounter("2/current");stepCounter("3/current");
        const QString trackedState=temp.filePath("with-lua.krs");
        session.saveStateToFile(trackedState);
        waitFor([&]{return QFile::exists(trackedState);},"file with declared data");
        stepCounter("4/current");stepCounter("5/current");
        session.backFrame();
        waitFor([&]{return overlay && overlay->commands.size()==1 && overlay->commands[0].text=="4/restored";},"rewind declared counter");
        session.loadStateFromFile(trackedState);
        waitFor([&]{return logged(11,"loaded:3/restored");},"file restores declared data");
        stepCounter("4/current");
        check(session.scriptCommand(stop),"stop data script");
        waitFor([&]{return status(11,ScriptStatus::Stopped);},"data script stopped");
        // Load without any script, then start it: no automatic script launch/import.
        session.loadStateFromFile(trackedState);
        ScriptCommand barrier;barrier.id=12;barrier.path="barrier.lua";barrier.source="print('bare load barrier')";
        check(session.scriptCommand(barrier),"bare load barrier queue");
        waitFor([&]{return logged(12,"bare load barrier");},"bare load succeeds");
        check(session.scriptCommand(tracked),"restart same script");
        waitFor([&]{return overlay && overlay->commands[0].text=="0/current";},"restart begins defaults");
        updates.clear();
        session.loadStateFromFile(trackedState);
        waitFor([&]{return logged(11,"loaded:3/restored");},"same source restart matches");
        tracked.source=trackedSource+"\n-- incompatible source revision\n";
        check(session.scriptCommand(tracked),"changed source restart");
        waitFor([&]{return overlay && overlay->commands[0].text=="0/current";},"changed source defaults");
        session.loadStateFromFile(trackedState);
        waitFor([&]{return logged(11,"loaded:0/incompatible");},"changed source cannot import data");
        session.loadStateFromFile(plainState);
        waitFor([&]{return logged(11,"loaded:0/missing");},"old save restores defaults");
        session.resetGame();
        waitFor([&]{return logged(11,"reset:0/reset");},"reset restores defaults");
        check(session.scriptCommand(stop),"stop timeline test");
        waitFor([&]{return status(11,ScriptStatus::Stopped);},"timeline stop");

        tracked.id=13;tracked.path="temporary-timeline.lua";
        tracked.source=R"(
assert(timeline.define('temporary-counter',1,{count=0}))
assert(timeline.set({count=17}))
local save=assert(savestate.create())
assert(savestate.save(save))
assert(timeline.set({count=29}))
assert(savestate.load(save))
assert(emu.frameadvance())
assert(timeline.get().count==17 and timeline.status()=='restored')
assert(savestate.free(save))
print('temporary timeline restored')
)";
        check(session.scriptCommand(tracked),"temporary data script");
        waitFor([&]{return logged(13,"temporary timeline restored");},"temporary data capture during startup");
        check(session.scriptCommand(stop),"stop temporary data");
        waitFor([&]{return status(13,ScriptStatus::Stopped);},"temporary stop");

        QFile timelineExample(QStringLiteral(KEGA_SOURCE_ROOT "/assets/scripts/examples/06_Timeline.lua"));
        check(timelineExample.open(QIODevice::ReadOnly),"open shipped timeline example");
        tracked.id=14;tracked.path=timelineExample.fileName();tracked.source=timelineExample.readAll();
        check(session.scriptCommand(tracked),"run timeline example");
        waitFor([&]{return status(14,ScriptStatus::Running) && overlay &&
            overlay->commands[1].text.startsWith("Frames: 0");},"timeline example starts");
        session.advanceFrame();
        waitFor([&]{return overlay && overlay->commands[1].text.startsWith("Frames: 1");},"timeline example advances");
        session.advanceFrame();
        waitFor([&]{return overlay && overlay->commands[1].text.startsWith("Frames: 2");},"timeline example second frame");
        session.backFrame();
        waitFor([&]{return overlay && overlay->commands[1].text.startsWith("Frames: 1");},"timeline example rewind");
        check(session.scriptCommand(stop),"stop example timeline");
        waitFor([&]{return status(14,ScriptStatus::Stopped);},"timeline example cleanup");

        auto runMovieScript=[&](quint64 id, QByteArray source) {
            ScriptCommand command;
            command.id=id;
            command.path="native-movie.lua";
            command.source=std::move(source);
            check(session.scriptCommand(command),"movie script queue");
        };
        runMovieScript(20,R"(
assert(movie.status().mode == 'off' and not movie.status().pending)
assert(not movie.play('../input'))
local reset = false
emu.on('reset',function()
    reset = movie.status().mode == 'recording'
    assert(not movie.stop())
end)
emu.on('beforeframe',function() assert(not movie.stop()) end)
assert(movie.record('input'))
assert(movie.status().pending)
assert(not movie.play('other'))
assert(emu.frameadvance())
local s = movie.status()
assert(reset and s.mode == 'recording' and not s.pending and s.frame == 1 and s.length == 1)
assert(#s.id == 32 and s.path:sub(-9) == 'input.krm' and s.fps > 59)
assert(input.set(1,{a=true}))
assert(emu.frameadvance())
assert(movie.status().frame == 2 and input.get(1).a)
print('recorded exact inputs')
)");
        waitFor([&]{return logged(20,"recorded exact inputs");},"Lua record/reset/step");
        const QString moviePath=temp.filePath("movies/input.krm");
        check(!QFile::exists(moviePath),"Lua movie already persisted");
        check(QDir().mkpath(moviePath),"conflicting directory fixture");
        runMovieScript(21,R"(
local firstFrame = emu.framecount()
local sawProgress, sawError = false, false
emu.on('frame',function()
    local s = movie.status()
    if s.pending and not sawProgress and emu.framecount() >= firstFrame + 2 then
        assert(s.mode == 'off')
        sawProgress = true
        print('live frames advanced while saving')
    end
    if s.error and not s.pending and not sawError then
        assert(s.mode == 'off')
        sawError = true
        print('detached save failed')
    end
end)
assert(emu.pause(false))
assert(movie.stop())
)");
        waitFor([&]{return logged(21,"live frames advanced while saving") &&
                           logged(21,"detached save failed");},"detached save progress and failure");
        check(session.hasUnsavedMovie() && !session.movieFileOperationPending(),"detached tape retained for retry");
        session.setPaused(true);
        waitFor([&]{return paused;},"pause for retry");
        check(QDir().rmdir(moviePath),"remove conflicting directory fixture");
        runMovieScript(22,R"(
assert(movie.status().error and not movie.record('another'))
assert(movie.stop())
assert(emu.frameadvance())
local s = movie.status()
assert(s.mode == 'off' and not s.pending and not s.error)
print('detached save retried')
)");
        waitFor([&]{return logged(22,"detached save retried");},"Lua retry while paused");
        check(!session.hasUnsavedMovie() && paused,"save complete without unpausing");
        MovieTape source, branch;
        QString movieError;
        check(source.load(moviePath,movieError) && source.size()==2 &&
              source.resumeFrame()==2,"persisted only the captured frames");
        runMovieScript(23,R"(
assert(timeline.define('branch-counter',1,0))
assert(movie.play('input.krm'))
assert(emu.frameadvance())
local s = movie.status()
assert(s.mode == 'playback' and s.frame == 1 and s.length == 2 and not input.get(1).a)
assert(timeline.get()==0 and timeline.status()=='missing')
assert(timeline.set(42))
local original = s.id
emu.on('beforeframe',function() assert(input.set(1,{b=true})) end)
assert(movie.rerecord('branch'))
assert(emu.frameadvance())
s = movie.status()
assert(s.mode == 'recording' and s.id ~= original and s.frame == 2 and input.get(1).b)
assert(timeline.get()==42 and timeline.status()=='current')
assert(movie.stop())
assert(emu.frameadvance())
assert(movie.status().mode == 'off' and not movie.status().error)
print('playback branch saved')
)");
        waitFor([&]{return logged(23,"playback branch saved");},"Lua play then rerecord through top-level continuation");
        check(branch.load(temp.filePath("movies/branch.krm"),movieError) &&
              branch.id()!=source.id() && branch.size()==2 && branch.resumeFrame()==2,"distinct branch metadata");
        MovieInput originalFirst, originalSecond, branchFirst, branchSecond;
        check(source.inputAt(0,originalFirst) && source.inputAt(1,originalSecond) &&
              branch.inputAt(0,branchFirst) && branch.inputAt(1,branchSecond) &&
              originalFirst==branchFirst && originalSecond!=branchSecond,"branch input prefix and divergence");
        runMovieScript(24,R"(
assert(movie.resume('input'))
assert(emu.frameadvance())
local s = movie.status()
assert(s.mode == 'recording' and s.frame == 3 and s.length == 3)
assert(movie.stop())
assert(emu.frameadvance())
assert(movie.play('input'))
for i = 1,3 do
    assert(emu.frameadvance())
    assert(movie.status().frame == i)
end
assert(movie.status().mode == 'finished')
assert(movie.takeover())
assert(emu.frameadvance())
assert(movie.status().mode == 'off')
print('resumed and took over completed playback')
)");
        waitFor([&]{return logged(24,"resumed and took over completed playback");},"Lua resume and finished takeover");
        MovieTape resumed;
        check(resumed.load(moviePath,movieError) && resumed.id()==source.id() &&
              resumed.size()==3 && resumed.resumeFrame()==3,"resumed file preserved identity and appended frames");
        check(!QFile::exists(moviePath+".bak"),"explicit movie save has no adjacent backup");
        check(session.scriptCommand(stop),"stop movie test scripts");
        waitFor([&]{return status(24,ScriptStatus::Stopped);},"stop movie test scripts");
        session.setPaused(true);
        waitFor([&]{return paused;},"pause for stale request");
        runMovieScript(25,R"(
assert(movie.play('input'))
assert(emu.frameadvance())
error('stale movie preparation resumed unexpectedly')
)");
        waitFor([&]{return session.movieFileOperationPending();},"movie prepare pending before reset");
        session.resetGame();
        waitFor([&]{return !session.movieFileOperationPending() && status(25,ScriptStatus::Stopped);},
                "stale prepared playback cancelled");
        check(movieMode==0,"stale candidate was not committed");

        // Exercise the shipped window, native recording, and its persisted measurements.
        qunsetenv("KEGA_TEST_MOVIE_FILE_DELAY_MS");
        QFile example(QStringLiteral(KEGA_SOURCE_ROOT "/assets/scripts/examples/07_FrameLog.lua"));
        check(example.open(QIODevice::ReadOnly),"open recording example");
        ScriptCommand recordExample;
        recordExample.path=example.fileName();
        recordExample.source=example.readAll();
        auto exampleRun=[&](quint64 id) {
            recordExample.id=id;
            check(session.scriptCommand(recordExample),"run recording example");
            waitFor([&]{return panels.windows.size()==1 && status(id,ScriptStatus::Running);},
                    "recording example window");
        };
        quint64 exampleSequence=0;
        auto exampleClick=[&](const QString& control) {
            check(panels.windows.size()==1,"recording example window missing");
            const auto& window=panels.windows.front();
            ScriptUiEvent event;
            event.owner=window.owner;event.run=window.run;event.window=window.handle;
            event.epoch=panels.epoch;event.sequence=++exampleSequence;
            event.kind=ScriptUiEventKind::Click;event.control=control;
            check(session.postScriptUiEvent(event),"recording example button queue");
        };
        auto exampleGraph=[&] {
            if (!panels.windows.isEmpty())
                for (const auto& control:panels.windows.front().controls)
                    if (control.id=="activity") return control.points;
            return QVector<double>{};
        };
        session.setPaused(true);
        waitFor([&]{return paused;},"pause before recording example");
        exampleRun(26);
        check(movieMode==0 && !session.movieFileOperationPending(),"example must wait for Record");
        const auto beforeRecordEpoch=panels.epoch;
        exampleClick("record");
        waitFor([&]{return movieMode==1 && !session.movieFileOperationPending()
            && panels.epoch!=beforeRecordEpoch;},"example recording started");
        check(paused,"record button preserves pause");
        exampleClick("check");
        waitFor([&]{return panels.windows.size()==1
            && panels.windows.front().acknowledged==exampleSequence;},"example status while paused");
        session.setPaused(false);
        waitFor([&]{return !exampleGraph().isEmpty();},"recorded input-poll sample");
        exampleClick("stop");
        waitFor([&]{return logged(26,"Recording and measurements saved");},"save movie and sidecar");
        const auto recordedPoints=exampleGraph();
        MovieTape measured;
        check(measured.load(temp.filePath("movies/lua-frame-log.krm"),movieError) &&
            measured.size()>=30 && recordedPoints.size()==qsizetype(measured.size()/30),
            "sample per completed recording window");
        check(session.scriptCommand(stop),"stop example before replay");
        waitFor([&]{return status(26,ScriptStatus::Stopped) && panels.windows.isEmpty();},"recording example stopped");

        exampleRun(27);
        check(exampleGraph().isEmpty(),"restarted script has no retained samples");
        exampleClick("play");
        waitFor([&]{return movieMode==3 && exampleGraph()==recordedPoints;},"native playback restores sidecar graph");
        check(status(27,ScriptStatus::Running) && overlay && overlay->commands.size()==2 &&
            overlay->commands.front().primitive==ScriptPrimitive::Rectangle &&
            overlay->commands.back().text.startsWith("Play "),"playback progress has a backing");
        // The script's Stop button can still be visible after another movie is
        // started from the main menu while the emulator cannot complete a frame.
        session.setBackgroundPaused(true);
        session.stopMovie();
        waitFor([&]{return movieMode==0 && !session.hasUnsavedMovie();},"finish example playback");
        const auto originalPlaybackEpoch=panels.epoch;
        session.playMovie(moviePath);
        session.setPaused(true);
        waitFor([&]{return movieMode==2 && paused && panels.epoch!=originalPlaybackEpoch;},
                "unrelated playback starts without a completed frame");
        exampleClick("stop");
        waitFor([&]{return !panels.windows.isEmpty() && panels.windows.front().acknowledged==exampleSequence;},
                "dispatch stale Stop button");
        bool unrelatedMoviePreserved=false;
        for (const auto& control:panels.windows.front().controls)
            if (control.id=="status") unrelatedMoviePreserved=control.value.toString()=="The active recording changed.";
        check(unrelatedMoviePreserved && movieMode==2,"stale example Stop must preserve unrelated playback");
        session.stopMovie();
        waitFor([&]{return movieMode==0;},"finish unrelated playback");
        session.setBackgroundPaused(false);
        check(session.scriptCommand(stop),"stop playback example");
        waitFor([&]{return status(27,ScriptStatus::Stopped) && panels.windows.isEmpty();},"stop playback example");

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
        run.source.prepend("assert(movie.status().mode == 'off' and not movie.record('online'))\n");
        check(session.scriptCommand(run),"local read-only network script");
        waitFor([&]{return status(6,ScriptStatus::Running) && overlay;},"network script enabled");
        session.setNetworkMode(0);
        waitFor([&]{return !session.networkActive();},"network exit");
        session.closeGame();
        waitFor([&]{return closed && status(6,ScriptStatus::Stopped);},"close frees scripts");
        session.shutdown();
        std::puts("PASS Lua session: paused panels, shipped tool window, frame lifecycle, deterministic movie controls, asynchronous save/retry and stale request cancellation");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1;
    }
}
