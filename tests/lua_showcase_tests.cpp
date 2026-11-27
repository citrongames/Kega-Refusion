// Explicit user-ROM probe. All output/state lives in the supplied scratch folder.
#include "core/LibretroHost.h"
#include "core/MovieSession.h"
#include "script/ScriptRuntime.h"
#include "ui/GameView.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <array>
#include <cstdio>
#include <stdexcept>

static void check(bool okay, const QString& message) {
    if (!okay) throw std::runtime_error(message.toStdString());
}
static QByteArray read(const QString& path) {
    QFile f(path); check(f.open(QIODevice::ReadOnly),path); return f.readAll();
}
static void errors(ScriptRuntime& lua) {
    for (const auto& u:lua.takeUpdates()) {
        if(u.status==ScriptStatus::Error) throw std::runtime_error(u.text.toStdString());
        if(u.status==ScriptStatus::Log) std::printf("Lua: %s\n",qPrintable(u.text));
    }
}
static MovieInput inputAt(int frame, bool contra) {
    MovieInput input;
    if (contra) {
        if(frame%60==5 && frame<1425) input.buttons[0]|=1<<3;
        if(frame<1800 && frame%26==9) input.buttons[0]|=1<<0;
        if(frame>1740) {
            input.buttons[0]|=1<<0;
            if(frame%200<150)input.buttons[0]|=1<<7;
            if(frame%180<18)input.buttons[0]|=1<<8;
        }
    } else {
        if(frame==720) input.buttons[0]|=1<<3;
        if(frame>900) {
            input.buttons[0]|=1<<7;
            if(frame%100<20) input.buttons[0]|=1<<8;
        }
    }
    return input;
}
static void uiEvent(ScriptRuntime& lua,const LibretroHost& host,const QString& id,
                    const QVariant& value,ScriptUiEventKind kind,quint64& sequence) {
    auto snapshot=lua.uiSnapshot();check(!snapshot.windows.isEmpty(),"script window missing");
    const auto& w=snapshot.windows[0];
    ScriptUiEvent e; e.owner=w.owner;e.run=w.run;e.window=w.handle;e.epoch=snapshot.epoch;
    e.sequence=++sequence;e.kind=kind;e.control=id;e.value=value;
    check(lua.dispatchUi(e,host,1,host.runClock().frames),"UI rejected: "+id);errors(lua);
}
static bool hasText(const ScriptOverlayPtr& overlay) {
    if(!overlay)return false;
    for(const auto& c:overlay->commands) if(c.primitive==ScriptPrimitive::Text) return true;
    return false;
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        const auto a=app.arguments();
        check(a.size()>=6,"usage: lua_showcase_tests CORE ROM SCRIPT sonic|contra OUTPUT [WARMUP] [FRAMES]");
        const bool contra=a[4]=="contra";const int warm=a.size()>6?a[6].toInt():(contra?1900:1200);
        const int frames=a.size()>7?a[7].toInt():360;
        check(warm>=0 && warm<=36000 && frames>=32 && frames<=36000,"frame bounds");
        check(QDir().mkpath(a[5]),"output directory");
        QTemporaryDir temp;check(temp.isValid(),"isolated data");
        LibretroHost host(a[1],temp.path());QString error;
        check(host.setRegion(2,error) && host.load(a[2],error,-1,nullptr,false),error);
        MovieSession movie(host,{});
        for(int i=1;i<=warm;++i) {
            const auto input=inputAt(i,contra);
            check(movie.beforeFrame(input,error),error);host.runFrame();movie.afterFrame();
        }
        check(host.frame().save(a[5]+"/game.png"),"game image");
        QByteArray ram(65536,0);check(host.inspectionRead(0,2,0,ram.data(),size_t(ram.size())),"RAM read");
        QFile ramFile(a[5]+"/ram.bin");check(ramFile.open(QIODevice::WriteOnly),"RAM file");ramFile.write(ram);ramFile.close();
        const auto seed=host.serialize();const auto clock=host.runClock();check(!seed.isEmpty(),"seed");
        const auto source=read(a[3]);
        for(int stride:{1,32}) {
            std::array<double,3> timings{};
            QByteArray reference;
            for(int workload=0;workload<3;++workload) {
                check(host.unserialize(seed),"restore seed");host.setRunClock(clock);
                ScriptRuntime lua;ScriptOverlayPtr overlay;quint64 seq=0;
                lua.setHostPolicy(false,false,false,false,true);
                if(workload) {
                    ScriptCommand command;command.id=1;command.path=a[3];command.source=source;
                    lua.command(command,host);errors(lua);check(lua.hasScripts(),"script started");
                    // Detection may span several frame callbacks without advancing native state.
                    for(int n=0;n<600;++n) {
                        overlay=lua.event(ScriptEvent::Frame,host,1,clock.frames,0,true);
                        errors(lua);
                        const auto windows=lua.uiSnapshot().windows;
                        bool ready=contra;
                        if(!windows.isEmpty() && !contra)
                            for(const auto& c:windows[0].controls)
                                if(c.id=="status" && c.value.toString().startsWith("REV00"))ready=true;
                        if(!windows.isEmpty() && ready)break;
                    }
                    check(!lua.uiSnapshot().windows.isEmpty(),"game recognized and window created");
                    if(workload==2) {
                        const auto controls=lua.uiSnapshot().windows[0].controls;
                        for(const auto& c:controls)
                            if(c.kind==ScriptUiKind::Checkbox && c.enabled && !c.value.toBool())
                                uiEvent(lua,host,c.id,true,ScriptUiEventKind::Change,seq);
                    }
                }
                check(host.setDebugAccessObserver(lua.accessSubscriptions(),
                    +[](void* context,const DebugAccessEvent& event) {
                        static_cast<ScriptRuntime*>(context)->dispatchMemoryAccess(event);
                    },&lua),"connect script observations");
                QElapsedTimer timer;timer.start();
                for(int n=1;n<=frames;++n) {
                    auto input=inputAt(warm+n,contra);
                    const bool present=n%stride==0 || n==frames;
                    if(workload)lua.beforeFrame(host,1,host.runClock().frames,input);
                    check(movie.beforeFrame(input,error),error);
                    if(workload)lua.beginAccessFrame(host,host.runClock().frames+1);
                    host.runFrame(present,stride==1);movie.afterFrame();
                    if(workload)lua.endAccessFrame();
                    if(workload) {
                        overlay=lua.event(ScriptEvent::Frame,host,1,host.runClock().frames,0,present,&input);
                        errors(lua);
                    }
                }
                timings[workload]=double(timer.nsecsElapsed())/(1000.0*frames);
                check(host.setDebugAccessObserver({},nullptr,nullptr),"disconnect observations");
                const auto digest=QCryptographicHash::hash(host.serialize(),QCryptographicHash::Sha256);
                if(!workload)reference=digest;
                else {
                    check(digest==reference,"script changed native game state");
                    check(hasText(overlay),"script has no visible text");
                    if(stride==1) {
                        GameView view;
                        view.resize(960,672);
                        view.setOsdEnabled(false);
                        view.setAspectMode(GameView::AspectMode::SquarePixels);
                        view.setFrame(host.frame(),4.0/3.0,{}, {},overlay);
                        QImage rendered(view.size(),QImage::Format_RGB32);
                        rendered.fill(Qt::black);
                        view.render(&rendered);
                        check(rendered.save(a[5]+QString("/overlay-%1.png").arg(workload)),"overlay image");
                    }
                    const auto scriptData=lua.captureTimeline();
                    check(host.unserialize(seed),"restore for Lua events");host.setRunClock(clock);
                    check(lua.restoreTimeline(scriptData),"Lua timeline validate/restore");
                    lua.invalidateTimeline(false);
                    for(auto event:{ScriptEvent::LoadState,ScriptEvent::Rewind,ScriptEvent::Draw}) {
                        overlay=lua.event(event,host,2,clock.frames);errors(lua);
                    }
                    check(hasText(overlay),"restore lost HUD");
                    const auto windows=lua.uiSnapshot().windows;
                    for(const auto& w:windows) {
                        std::printf("window: %s\n",qPrintable(w.title));
                        for(const auto& c:w.controls)if(c.kind==ScriptUiKind::Label)
                            std::printf("  %s: %s\n",qPrintable(c.id),qPrintable(c.value.toString()));
                    }
                    lua.stopAll();check(!lua.hasScripts() && lua.uiSnapshot().windows.isEmpty(),"stop cleanup");
                }
            }
            std::printf("%s %.5fHz stride %d: baseline %.2fus default %.2fus all-sections %.2fus; state identical\n",
                qPrintable(host.contentName()),host.fps(),stride,timings[0],timings[1],timings[2]);
        }
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
