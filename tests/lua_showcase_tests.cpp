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
// Synthetic slot cases check decoding and bounded drawing, not game progression.
static void healthCases(LibretroHost& host, const QByteArray& source, const QString& path) {
    const auto saved=host.serialize();
    const auto word=[&](quint32 offset,quint16 value) {
        const char bytes[]={char(value>>8),char(value)};
        check(host.inspectionWrite(0,2,offset,bytes,2),"seed health word");
    };
    const auto clear=[&] {for(quint32 offset=0xb000;offset<0xd700;offset+=2)word(offset,0);};
    const auto actor=[&](int slot,int id,int state) {
        const auto base=0xb000+slot*128;
        word(base,id);word(base+2,state);word(base+6,200);word(base+10,200);
        return base;
    };
    ScriptRuntime lua;lua.setHostPolicy(false,false,false,false,true);
    ScriptCommand command;command.id=1;command.source=source;command.path=path;
    lua.command(command,host);errors(lua);
    quint64 frame=host.runClock().frames, sequence=0;
    const auto contact=[&](quint32 address, quint32 flags=0x10, quint32 top=150, quint32 a0=0) {
        for(const auto& [name,value]:std::array<std::pair<const char*,quint32>,7>{{
            {"D2",flags},{"D4",200},{"D5",150},{"D6",200},{"D7",top},{"A1",0xff0000+address},{"A0",a0}}})
            check(host.inspectionWriteRegister(0,1,name,value),"seed collision register");
        lua.dispatchMemoryAccess({0,1,1,0x7ec0,0,2});
    };
    const auto draw=[&] {
        const auto overlay=lua.event(ScriptEvent::Draw,host,1,frame);
        errors(lua);check(bool(overlay),"health overlay");
        return overlay;
    };
    const auto sample=[&](quint32 address) {
        lua.beginAccessFrame(host,++frame);
        contact(address);
        lua.endAccessFrame();
        return draw();
    };
    const auto countText=[](const ScriptOverlayPtr& overlay,const QString& label) {
        int copies=0;
        for(const auto& c:overlay->commands)
            if(c.primitive==ScriptPrimitive::Text && c.text==label)++copies;
        return copies;
    };
    clear();const auto a=actor(0,0x295,11);word(a+0x36,990);
    lua.beginAccessFrame(host,++frame);
    contact(a,0x20,140);
    contact(a);
    lua.endAccessFrame();
    const auto overlay=draw();
    const ScriptDraw *target=nullptr, *backdrop=nullptr, *label=nullptr;
    for(const auto& c:overlay->commands) {
        if(c.primitive==ScriptPrimitive::Rectangle && c.color.rgba()==0xffffd16a)target=&c;
        if(c.primitive==ScriptPrimitive::Text && c.text=="990/1000")label=&c;
    }
    check(target && label,"direct late-enemy HP attached to target");
    for(const auto& c:overlay->commands)
        if(c.primitive==ScriptPrimitive::Rectangle && c.bounds.contains(label->bounds.topLeft())
            && c.bounds.bottom()==target->bounds.top()-1 && c.fill.alpha()>0 && c.fill.alpha()<255)
            backdrop=&c;
    check(backdrop,"semi-transparent label background above target");

    word(a+0x24,0x80);
    word(a+0x2e,200);word(a+0x30,150);word(a+0x32,220);word(a+0x34,170);
    word(0xaea2,a);word(0xa3a2,0xaea4);
    uiEvent(lua,host,"solids",true,ScriptUiEventKind::Change,sequence);
    check(countText(draw(),"990/1000")==1,"one label for combat and solid rectangles");
    uiEvent(lua,host,"contacts",false,ScriptUiEventKind::Change,sequence);
    check(countText(draw(),"990/1000")==1,"solid layer owns its health label");
    uiEvent(lua,host,"solids",false,ScriptUiEventKind::Change,sequence);
    check(countText(draw(),"990/1000")==0,"health hidden with hitbox layers even with HUD on");
    const auto details=lua.uiSnapshot().windows[0].controls;
    for(const auto& control:details)
        if(control.id=="details")check(!control.value.toString().contains("990"),"health removed from slot panel");
    uiEvent(lua,host,"contacts",true,ScriptUiEventKind::Change,sequence);

    clear();const auto b=actor(0,0x33b,2);word(b+0x70,10);
    check(countText(sample(b),"841/851")==1,"strict damage threshold");
    clear();const auto sharedA=actor(0,0x28f,1), sharedB=actor(1,0x290,1);word(0xa240,300);
    lua.beginAccessFrame(host,++frame);
    contact(sharedA);contact(sharedB);
    lua.endAccessFrame();
    check(countText(draw(),"300/4000")==1 && countText(draw(),"300")==0,"shared health duplicated");
    clear();const auto c=actor(0,0x198,4);word(c+0x70,100);
    check(countText(sample(c),"2400/2500")==1,"phase one threshold");
    word(c+2,10);check(countText(sample(c),"901/1001")==1,"phase two threshold");
    word(c+2,12);check(countText(sample(c),"1700/1800")==1,"phase three threshold");

    clear();const auto controller=actor(0,0x018e,1), part=actor(1,0x0197,1);
    word(controller+0x70,10);word(part+0x6a,0xff);word(part+0x6c,controller);
    check(countText(sample(part),"1390/1400")==1,"forwarded boss part health");
    clear();const auto owner=actor(0,0x097,1), proxy=actor(1,2,1);
    word(owner+0x40,0x0800);word(owner+0x36,40);word(owner+0x7c,0xff);word(owner+0x7e,proxy);
    lua.beginAccessFrame(host,++frame);
    contact(proxy,0x10,150,0xff0000+owner);
    lua.endAccessFrame();
    check(countText(draw(),"40/1050")==1,"variant hitbox proxy health");

    clear();
    for(int i=0;i<78;++i){const auto base=actor(i,0x124,1);word(base+0x36,40);}
    const auto controls=lua.uiSnapshot().windows[0].controls;
    for(const auto& control:controls)
        if(control.kind==ScriptUiKind::Checkbox && control.enabled && !control.value.toBool())
            uiEvent(lua,host,control.id,true,ScriptUiEventKind::Change,sequence);
    // Populate the optional layers together; the script must fit its draw/native budgets.
    for(int i=0;i<78;++i) {
        const auto base=0xb000+i*128;
        word(base+0x24,0x80);
        word(base+0x2e,200);word(base+0x30,150);word(base+0x32,200);word(base+0x34,150);
        word(0xaea2+i*2,base);
    }
    word(0xa3a2,0xaea2+78*2);
    for(int i=0;i<14;++i) {
        const auto base=0xd800+i*64;
        word(base,1);word(base+0x24,0x20);word(base+6,200);word(base+10,200);
        word(base+0x2e,200);word(base+0x30,150);word(base+0x32,200);word(base+0x34,150);
        word(0xafc0+i*2,base);
    }
    word(0xafc0+28,0);
    for(int i=0;i<2;++i) {
        const auto base=0xd700+i*128;
        word(base,1);word(base+6,200);word(base+10,200);
        word(base+0x2e,200);word(base+0x30,150);word(base+0x32,200);word(base+0x34,150);
    }
    lua.beginAccessFrame(host,++frame);
    for(int i=0;i<33;++i)contact(0xb000+i*128,0x30);
    lua.endAccessFrame();
    const auto full=lua.event(ScriptEvent::Frame,host,1,frame);
    errors(lua);
    check(full && full->commands.size()<=128 && countText(full,"40/40")>=32,"all-layer drawing budget");
    lua.stopAll();check(host.unserialize(saved),"restore after synthetic health cases");
    std::puts("PASS health overlay: placement, background, layer toggles, shared owners, boss phases and 78-slot draw budget");

}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        const auto a=app.arguments();
        check(a.size()>=6,"usage: lua_showcase_tests CORE ROM SCRIPT sonic|contra OUTPUT [WARMUP] [FRAMES] [NATIVE_STATE]");
        const bool contra=a[4]=="contra";
        const bool healthOnly=contra && a.size()>6 && a[6]=="health";
        const int warm=healthOnly?1570:(a.size()>6?a[6].toInt():(contra?1900:1200));
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
        if(healthOnly){healthCases(host,read(a[3]),a[3]);return 0;}
        if (a.size()>8) {
            check(host.unserialize(read(a[8])),"load explicit native probe state");
            host.runFrame(); // Reconstruct the image from restored native state.
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
        if(contra && a.size()<=8)healthCases(host,source,a[3]);
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
