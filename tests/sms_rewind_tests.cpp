#include "core/LibretroHost.h"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QJsonObject>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtEndian>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}
void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write original 8-bit fixture");
}
// Original Z80 fixture: once per VBlank sample both sockets and GG Start,
// advance the backdrop/PSG, and count real Pause NMIs at $C003.
QByteArray fixture(bool gg, bool sg) {
    QByteArray rom(32*1024, char(0));
    rom.replace(0, 3, QByteArray::fromHex("c30001"));
    rom.replace(0x66, 11, QByteArray::fromHex("f53a03c03c3203c0f1ed45"));
    int pc=0x100;
    const auto code=[&](std::initializer_list<unsigned> bytes) { for(unsigned value:bytes) rom[pc++]=char(value); };
    code({0xf3,0x31,0xf0,0xdf,0xaf,0x32,0,0xc0,0x32,1,0xc0,0x32,2,0xc0,0x32,3,0xc0});
    const auto reg=[&](unsigned index,unsigned value){code({0x3e,value,0xd3,0xbf,0x3e,0x80|index,0xd3,0xbf});};
    reg(0,sg?0:4);reg(1,0x40);reg(2,0xe);reg(7,0);
    // SMS and GG palettes use different colour widths; initialize both halves.
    code({0x3e,0,0xd3,0xbf,0x3e,0xc0,0xd3,0xbf});
    for(unsigned i=0;i<(gg?64u:32u);++i) code({0x3e,(i*7)&0xff,0xd3,0xbe});
    const int loop=pc;
    code({0xdb,0xbf,0xe6,0x80,0x28,0xfa}); // wait for next VBlank flag
    code({0x3a,0,0xc0,0x3c,0x32,0,0xc0});
    code({0xe6,15,0xd3,0xbf,0x3e,0x87,0xd3,0xbf});
    code({0xdb,0xdc,0x32,1,0xc0,0xdb,0xdd,0x32,2,0xc0,0xdb,0,0x32,4,0xc0});
    code({0x3a,0,0xc0,0xe6,15,0xf6,0x80,0xd3,0x7f,0x3e,0x10,0xd3,0x7f,0x3e,0x90,0xd3,0x7f});
    code({0xc3,unsigned(loop&255),unsigned(loop>>8)});
    if(!sg) {rom.replace(0x7ff0,8,"TMR SEGA");rom[0x7fff]=char(gg?0x6c:0x4c);}
    return rom;
}
struct Input { unsigned a=0,b=0; int x=0,y=0; bool offscreen=false; };
struct Reference { QByteArray rewind,ram,audio; QImage video; unsigned nmi=0; };

void replay(const QString& core, const QString& suffix, ControllerType type, bool baseline,
            const QString& model = {}, unsigned expectedHardware = 0, int region = 0) {
    QTemporaryDir temp;check(temp.isValid(),"8-bit fixture directory");
    const bool gg=suffix=="gg",sg=suffix=="sg";
    const QString path=temp.filePath("original-rewind."+suffix);
    writeFile(path,fixture(gg,sg));
    LibretroHost host(core,temp.path()+"/data");QString error;
    if(region)check(host.setRegion(region,error),qPrintable(error));
    check(host.load(path,error),qPrintable(error));
    if(!model.isEmpty()) {
        auto configuration=host.movieConfiguration();
        auto options=configuration.value(QStringLiteral("options")).toObject();
        options.insert(QStringLiteral("genesis_plus_gx_system_hw"),model);
        configuration.insert(QStringLiteral("options"),options);
        check(host.prepareMovie(configuration,error),qPrintable(error));
    }
    QLibrary inspect(core);
    using Memory=void*(*)(unsigned);
    const auto memory=reinterpret_cast<Memory>(inspect.resolve("retro_get_memory_data"));
    check(memory!=nullptr,"8-bit RAM API");
    using Hardware=unsigned(*)();
    const auto hardware=reinterpret_cast<Hardware>(inspect.resolve("retro_kegarefusion_system_hw"));
    check(hardware&&(!expectedHardware||hardware()==expectedHardware),"8-bit hardware fixture selection");
    const QString label=suffix+(model.isEmpty()?QString():"/"+model)+(region==3?"/PAL":"");
    host.setControllerConnected(1,!gg);
    if(type!=ControllerType::Pad) check(host.setControllerKind(0,type),"8-bit peripheral selection");
    const auto ram=[&] {return QByteArray(static_cast<const char*>(memory(RETRO_MEMORY_SYSTEM_RAM)),0x2000);};
    const auto apply=[&](const Input& in) {
        host.setButtons(0,in.a);host.setButtons(1,in.b);
        if(type==ControllerType::LightPhaser)host.setGunAim(0,in.x,in.y,in.offscreen);
        else host.setPointer(0,in.x,in.y);
    };
    for(int i=0;i<12;++i)host.runFrame();
    std::vector<Input> inputs(72);
    std::vector<Reference> refs;refs.reserve(inputs.size());
    constexpr unsigned start=1u<<RETRO_DEVICE_ID_JOYPAD_START;
    for(unsigned i=0;i<inputs.size();++i) {
        // Exercise released/held/repressed Pause, player-two console Pause,
        // independent pad buttons, trigger/offscreen and paddle positions.
        auto& in=inputs[i];in.a=(i&1?1u<<1:0u)|(i&2?1u:0u)|(i%3?1u<<7:1u<<6);
        if((i>=4&&i<19)||(i>=30&&i<44))in.a|=start;
        in.b=(i&2?1u<<1:0u)|((i>=53&&i<65)?start:0u);
        in.x=int((i*917)%65536)-32768;in.y=int((i*541)%65536)-32768;in.offscreen=i%7==0;
        apply(in);host.runFrame();
        refs.push_back({host.captureRewindState(),ram(),host.audio(),host.frame().copy(),unsigned(quint8(ram()[3]))});
        check(!refs.back().rewind.isEmpty()&&!refs.back().audio.isEmpty(),"8-bit capture missing");
    }
    check(quint8(refs.back().ram[0])>20,"8-bit fixture did not advance VBlanks");
    bool inputChanges=false;for(size_t i=1;i<refs.size();++i)inputChanges|=refs[i-1].ram.mid(1,2)!=refs[i].ram.mid(1,2);
    check(inputChanges,"pad/peripheral fixture did not observe changing input");
    if(!gg && !sg)check(refs.back().nmi==3,"fixture did not observe all three console Pause edges");
    if(sg)check(refs.back().nmi==2,"SG fixture did not observe both player-one Pause edges");
    if(gg)check(refs.back().nmi==0,"GG Start generated a Pause NMI");
    unsigned comparisons=0;
    for(int target=int(inputs.size())-1;target>0;--target) {
        const int predecessor=((target-1)/7)*7;
        check(host.restoreRewindState(refs[size_t(predecessor)].rewind),"8-bit checkpoint restore");
        for(int frame=predecessor+1;frame<=target;++frame) {apply(inputs[size_t(frame)]);host.runFrame();}
        const auto& expected=refs[size_t(target)];
        if(ram()!=expected.ram) {
            std::fprintf(stderr,"RAM mismatch %s device=%d frame=%d checkpoint=%d NMI=%u expected=%u\n",qPrintable(suffix),int(type),target,predecessor,unsigned(quint8(ram()[3])),expected.nmi);
            check(false,"8-bit replay changed CPU RAM/input/Pause edge");
        }
        check(host.frame()==expected.video,"8-bit replay changed rendered frame");
        if(!baseline && host.audio()!=expected.audio) {
            std::fprintf(stderr,"PCM mismatch %s device=%d frame=%d checkpoint=%d bytes=%lld expected=%lld\n",qPrintable(label),int(type),target,predecessor,static_cast<long long>(host.audio().size()),static_cast<long long>(expected.audio.size()));
            check(false,"8-bit replay changed PCM audio");
        }
        ++comparisons;
    }
    // Restore a held-Pause checkpoint while the discarded future is released,
    // then release it and restore again. Both directions must preserve edges.
    if(!gg) {
        const auto& held=refs[12];
        check(host.restoreRewindState(held.rewind),"held Pause checkpoint restore");
        apply(inputs[13]);host.runFrame();
        check(unsigned(quint8(ram()[3]))==held.nmi,"held Pause repeated NMI after restore");
        auto released=inputs[13];released.a&=~start;released.b&=~start;
        apply(released);host.runFrame();
        check(host.restoreRewindState(held.rewind),"held Pause second restore");
        apply(inputs[13]);host.runFrame();
        check(unsigned(quint8(ram()[3]))==held.nmi,"released future leaked Pause latch into checkpoint");
    }
    if(!baseline)check(host.smsPauseStateSupported(),"new Pause state capability missing");
    std::printf("PASS: %s device=%d %u reverse targets: RAM/input/NMI, video and exact PCM; state=%lld bytes\n",qPrintable(label),int(type),comparisons,static_cast<long long>(refs[0].rewind.size()));
}


void lcdFilters(const QString& core) {
    QTemporaryDir temp;check(temp.isValid(),"LCD fixture directory");
    const auto gg=temp.filePath("original-lcd.gg"),sms=temp.filePath("original-lcd.sms");
    auto ggRom=fixture(true,false);
    const auto display=ggRom.indexOf(QByteArray::fromHex("3e40d3bf3e81d3bf"));
    check(display>=0,"LCD fixture display register");ggRom[display+1]=0;
    writeFile(gg,ggRom);writeFile(sms,fixture(false,false));
    LibretroHost host(core,temp.filePath("data"));QString error;
    const auto require=[&](bool value){if(!value)throw std::runtime_error(error.toStdString());};
    require(host.setGameGearLcd(true,error));require(host.load(gg,error));
    check(host.isGameGear()&&host.gameGearLcdEnabled(),"real GG did not apply local LCD preference");
    QLibrary inspect(core);using Query=unsigned(*)(unsigned);
    const auto query=reinterpret_cast<Query>(inspect.resolve("retro_kegarefusion_rewind_video"));
    check(query!=nullptr,"LCD native query missing");
    for(int frame=0;frame<12;++frame)host.runFrame();
    check(query(0)&2u,"GG LCD option did not reach native render configuration");
    const auto position=host.runClock().frames;
    const auto identity=host.movieCoreIdentity(),compatibility=host.networkCompatibility();
    const auto state=host.captureMovieState(false);host.runFrame();const auto lcd=host.frame();
    require(host.restoreMovieState(state,error,true));require(host.setGameGearLcd(false,error));
    check(host.runClock().frames==position,"LCD toggle reset the machine clock");
    host.runFrame();check(!(query(0)&2u),"LCD disable did not reach native core");
    check(host.frame()!=lcd,"GG LCD rendering did not change moving backdrop pixels");
    require(host.setGameGearLcd(true,error));require(host.setBlarggFilter(2,error));host.runFrame();
    check(!host.gameGearLcdEnabled()&&!(query(0)&2u),"Blargg did not disable LCD");
    require(host.setGameGearLcd(true,error));host.runFrame();
    check(host.movieConfiguration().value("options").toObject().value("genesis_plus_gx_blargg_ntsc_filter").toString()=="disabled",
          "LCD did not disable Blargg");
    check(host.movieCoreIdentity()==identity&&host.networkCompatibility()==compatibility,"local LCD changed deterministic core compatibility");
    auto configuration=host.movieConfiguration();auto options=configuration.value("options").toObject();
    options.insert("genesis_plus_gx_lcd_filter","disabled");configuration.insert("options",options);
    require(host.prepareMovie(configuration,error));host.runFrame();
    check(host.gameGearLcdEnabled()&&(query(0)&2u),"recording replaced local LCD preference");
    require(host.restoreMovieConfiguration(error));
    configuration=host.movieConfiguration();configuration.insert("networkCompatibility",QString::fromLatin1(compatibility.toHex()));
    options=configuration.value("options").toObject();options.insert("genesis_plus_gx_lcd_filter","disabled");configuration.insert("options",options);
    require(host.prepareNetwork(configuration,error));host.runFrame();
    check(host.gameGearLcdEnabled()&&(query(0)&2u),"network host replaced local LCD preference");
    require(host.restoreMovieConfiguration(error));
    require(host.hardReset(error));host.runFrame();check(host.gameGearLcdEnabled()&&(query(0)&2u),"power reset lost GG LCD preference");
    // The GG extension must not override an explicitly selected MD-MS machine.
    configuration=host.movieConfiguration();options=configuration.value("options").toObject();
    options.insert("genesis_plus_gx_system_hw","mega drive / genesis");configuration.insert("options",options);
    require(host.prepareMovie(configuration,error));host.runFrame();
    check(!host.isGameGear()&&!host.gameGearLcdEnabled()&&!(query(0)&2u),"GG extension enabled LCD on actual MD-MS hardware");
    require(host.restoreMovieConfiguration(error));host.unload();
    check(!host.isGameGear()&&!host.gameGearLcdEnabled(),"closed game retained effective GG LCD state");
    require(host.load(sms,error));host.runFrame();
    check(!host.isGameGear()&&!host.gameGearLcdEnabled()&&!(query(0)&2u),"LCD leaked into the next SMS cartridge");
    // A .sms fixture running in GG-MS is genuinely a Game Gear too.
    configuration=host.movieConfiguration();options=configuration.value("options").toObject();
    options.insert("genesis_plus_gx_system_hw","game gear");configuration.insert("options",options);
    require(host.prepareMovie(configuration,error));host.runFrame();
    check(host.isGameGear()&&host.gameGearLcdEnabled()&&(query(0)&2u),"actual GG-MS did not enable LCD for an SMS filename");
    require(host.restoreMovieConfiguration(error));require(host.load(gg,error));host.runFrame();
    check(host.gameGearLcdEnabled()&&(query(0)&2u),"returning to GG lost local LCD preference");
    std::puts("PASS: GG/GG-MS LCD rendering, local movie/net preference, Blargg exclusion, power/close and hardware-gated game changes");
}

void compatibleBuild(const QString& core,const QString& directory,bool writer) {
    check(QDir().mkpath(directory),"cross-build compatibility directory");
    const QString path=directory+"/original-compatible.sms";
    if(writer)writeFile(path,fixture(false,false));
    LibretroHost host(core,directory+"/data");QString error;check(host.load(path,error),qPrintable(error));
    QLibrary inspect(core);using Memory=void*(*)(unsigned);
    const auto memory=reinterpret_cast<Memory>(inspect.resolve("retro_get_memory_data"));
    check(memory!=nullptr,"cross-build RAM API");
    const auto ram=[&]{return QByteArray(static_cast<const char*>(memory(RETRO_MEMORY_SYSTEM_RAM)),0x2000);};
    const auto video=[&]{return QByteArray(reinterpret_cast<const char*>(host.frame().constBits()),host.frame().sizeInBytes());};
    const auto read=[&](const QString& name){QFile file(directory+'/'+name);check(file.open(QIODevice::ReadOnly),"read cross-build fixture");return file.readAll();};
    for(int i=0;i<12;++i)host.runFrame();
    if(writer) {
        writeFile(directory+"/old-native.state",host.serialize());
        writeFile(directory+"/old-movie.state",host.captureMovieState());
        writeFile(directory+"/old-recording-abi.txt",host.movieConfiguration().value(QStringLiteral("coreCompatibility")).toString().toUtf8());
        for(int i=0;i<10;++i)host.runFrame();
        writeFile(directory+"/expected.ram",ram());writeFile(directory+"/expected.video",video());
    } else {
        check(host.movieConfiguration().value(QStringLiteral("coreCompatibility")).toString().toUtf8()==read("old-recording-abi.txt"),"compatible SMS build changed recording ABI");
        const auto perturb=[&] {host.setButtons(0,1u<<RETRO_DEVICE_ID_JOYPAD_START);host.runFrame();host.runFrame();host.setButtons(0,0);};
        const auto compare=[&] {for(int i=0;i<10;++i)host.runFrame();check(ram()==read("expected.ram")&&video()==read("expected.video"),"actual old-build state changed RAM/video");};
        perturb();check(host.unserialize(read("old-native.state")),"actual old-build native state rejected");compare();
        perturb();check(host.restoreMovieState(read("old-movie.state"),error),qPrintable(error));compare();
    }
    std::puts(writer?"PASS: wrote actual pre-change native/movie state fixtures":"PASS: actual pre-change native/movie states restored; recording ABI unchanged");
}

void legacy(const QString& core) {
    QTemporaryDir temp;check(temp.isValid(),"legacy fixture directory");
    const QString path=temp.filePath("original-legacy.sms");writeFile(path,fixture(false,false));
    LibretroHost host(core,temp.path()+"/data");QString error;check(host.load(path,error),qPrintable(error));
    QLibrary inspect(core);
    using Memory=void*(*)(unsigned);using Size=size_t(*)();using Save=bool(*)(void*,size_t);using Load=bool(*)(const void*,size_t);
    auto memory=reinterpret_cast<Memory>(inspect.resolve("retro_get_memory_data"));
    auto size=reinterpret_cast<Size>(inspect.resolve("retro_serialize_size"));
    auto save=reinterpret_cast<Save>(inspect.resolve("retro_serialize"));
    auto load=reinterpret_cast<Load>(inspect.resolve("retro_unserialize"));
    check(memory&&size&&save&&load,"legacy raw state API");
    for(int i=0;i<12;++i)host.runFrame();
    const auto nmi=[&] {return unsigned(static_cast<const unsigned char*>(memory(RETRO_MEMORY_SYSTEM_RAM))[3]);};
    QByteArray state(qsizetype(size()),char(0));check(save(state.data(),size()),"legacy state save");
    const qsizetype trailer=state.indexOf("KRINP001");check(trailer>0,"legacy peripheral trailer");
    const auto rehash=[&](QByteArray& bytes) {
        quint32 hash=2166136261u;for(qsizetype i=0;i<124;++i)hash=(hash^quint8(bytes[trailer+i]))*16777619u;
        qToLittleEndian(hash,bytes.data()+trailer+124);
    };
    // Exact pre-change KRINP001 shape, including its historical padding/checksum.
    state.replace(trailer+90,34,QByteArray(34,char(0)));rehash(state);
    host.setButtons(0,1u<<RETRO_DEVICE_ID_JOYPAD_START);host.runFrame();
    check(load(state.constData(),size()),"pre-latch KRINP001 state rejected");
    const unsigned before=nmi();host.runFrame();check(nmi()==before+1,"legacy state inherited future Pause latch");
    // Older snapshots predate the optional trailer entirely.
    state.replace(trailer,128,QByteArray(128,char(0)));
    check(load(state.constData(),size()),"pre-peripheral state rejected");host.runFrame();
    check(nmi()==before+1,"pre-peripheral state inherited future Pause latch");
    // Trailer validation must still precede machine reset or partial restore.
    check(save(state.data(),size()),"current state save");state[trailer+98]^=1;
    const unsigned unchanged=nmi();check(!load(state.constData(),size()),"corrupted Pause trailer accepted");
    check(nmi()==unchanged,"corrupt state mutated live machine");
    check(host.hardReset(error),qPrintable(error));check(save(state.data(),size()),"post-power state save");
    check(quint8(state[trailer+98])==0,"power reset retained prior held Pause");
    std::printf("PASS: unchanged native size=%lld; legacy KRINP001/no trailer, checksum rejection and reset\n",static_cast<long long>(size()));
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        check(argc==2||argc==3||argc==4,"usage: sms_rewind_tests CORE [--baseline | --lcd-only | --write-legacy DIR | --read-legacy DIR]");
        const QString core=QString::fromLocal8Bit(argv[1]);
        if(argc==3&&QString::fromLocal8Bit(argv[2])=="--lcd-only") {lcdFilters(core);return 0;}
        const bool baseline=argc==3;
        check(!baseline||QString::fromLocal8Bit(argv[2])=="--baseline","unknown SMS rewind test mode");
        if(argc==4) {
            const QString mode=QString::fromLocal8Bit(argv[2]);
            check(mode=="--write-legacy"||mode=="--read-legacy","cross-build compatibility mode");
            compatibleBuild(core,QString::fromLocal8Bit(argv[3]),mode=="--write-legacy");return 0;
        }
        replay(core,"sms",ControllerType::Pad,baseline);
        replay(core,"sms",ControllerType::Paddle,baseline);
        replay(core,"sms",ControllerType::LightPhaser,baseline);
        replay(core,"gg",ControllerType::Pad,baseline);
        replay(core,"sg",ControllerType::Pad,baseline);
        replay(core,"sms",ControllerType::Pad,baseline,"mega drive / genesis",0x81);
        replay(core,"sms",ControllerType::Pad,baseline,"game gear",0x41);
        replay(core,"sms",ControllerType::Pad,baseline,{},0,3);
        if(!baseline) {legacy(core);lcdFilters(core);}
        return 0;
    } catch(const std::exception& failure) {std::fprintf(stderr,"FAIL: %s\n",failure.what());return 1;}
}
