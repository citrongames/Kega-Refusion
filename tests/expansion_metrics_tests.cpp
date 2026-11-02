#include "core/LibretroHost.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <rc_consoles.h>
#include <cstdio>
#include <stdexcept>
static void check(bool okay, const char* reason) { if (!okay) throw std::runtime_error(reason); }
static QByteArray rom(bool readsInput) {
    QByteArray data(65536, char(0xff));
    auto longAt = [&](int p, quint32 v) { for(int i=0;i<4;++i)data[p+i]=char(v>>(24-i*8)); };
    longAt(0, 0xfff000); longAt(4, 0x200);
    data.replace(0x100,16,"SEGA GENESIS    ");
    data.replace(0x120,16,"KR INPUT PROBE  ");
    data.replace(0x150,16,"KR INPUT PROBE  ");
    data.replace(0x180,2,"GM");
    longAt(0x1a0,0); longAt(0x1a4,65535); longAt(0x1a8,0xff0000); longAt(0x1ac,0xffffff);
    data[0x1f0]='U'; int pc=0x200;
    auto w=[&](quint16 v){data[pc++]=char(v>>8);data[pc++]=char(v);};
    w(0x46fc);w(0x2700); // Mask interrupts; original, tiny 68000 fixture.
    w(0x33fc);w(0x1234);w(0xff);w(0x0100); // execution marker in work RAM
    if(readsInput){w(0x1039);w(0xa1);w(0x0003);w(0x60f8);} // poll port A in loop
    else w(0x60fe);
    return data;
}
static void write(const QString& path,const QByteArray& data){QFile f(path);check(f.open(QIODevice::WriteOnly),"fixture open");check(f.write(data)==data.size(),"fixture write");}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    if(argc!=3){std::fprintf(stderr,"usage: expansion_metrics_tests CORE BIOS_DIRECTORY\n");return 2;}
    try {
        QTemporaryDir temp;check(temp.isValid(),"temp dir");QDir().mkpath(temp.path()+"/bios");
        for(const auto& name:QStringList{"bios_CD_U.bin","bios_CD_E.bin","bios_CD_J.bin","32X_G_BIOS.BIN","32X_M_BIOS.BIN","32X_S_BIOS.BIN"})
            check(QFile::copy(QString::fromLocal8Bit(argv[2])+"/"+name,temp.path()+"/bios/"+name),"copy user BIOS into isolated fixture");
        const auto polling=temp.path()+"/poll.md", silent=temp.path()+"/silent.md";
        write(polling,rom(true));write(silent,rom(false));
        LibretroHost host(QString::fromLocal8Bit(argv[1]),temp.path());QString error;
        QLibrary lib(QString::fromLocal8Bit(argv[1]));
        using Get=unsigned(*)();auto hardware=reinterpret_cast<Get>(lib.resolve("retro_kegarefusion_system_hw"));
        auto features=reinterpret_cast<Get>(lib.resolve("retro_kegarefusion_core_features"));
        using Memory=void*(*)(unsigned);auto memory=reinterpret_cast<Memory>(lib.resolve("retro_get_memory_data"));
        check(hardware && features && memory,"inspection exports");
        for(int mode=0;mode<4;++mode) {
            host.setExpansionDevices(mode&1,mode&2);
            if(!host.load(polling,error)) throw std::runtime_error(error.toStdString());
            check(host.supportsInputPollMetrics(),"input telemetry API");
            check((hardware()==0x84)==bool(mode&1),"CD mode1 presence");
            check(bool(features()&2)==bool(mode&2),"32X presence");
            check(host.achievementConsoleId()==RC_CONSOLE_MEGA_DRIVE,"ordinary ROM achievement identity");
            for(int i=0;i<90;++i)host.runFrame(false,false);
            check(host.frameInputPolled(),"polling ROM must read emulated controller");
            check(static_cast<quint16*>(memory(RETRO_MEMORY_SYSTEM_RAM))[0x80]==0x1234,"ordinary cartridge executes with attachments");
            const auto configuration=host.movieConfiguration();
            const auto state=host.captureMovieState();check(!state.isEmpty(),"snapshot with hardware");
            check(host.restoreMovieState(state,error),"restore hardware snapshot");
            check(host.saveState(0,error),"save quick state with attachment topology");
            host.setExpansionDevices(false,false);
            check(bool(features()&2)==bool(mode&2),"setting must not reset running session");
            check(host.hardReset(error),"apply pending hardware on power reset");
            check(hardware()!=0x84 && !(features()&2),"power reset applies automatic attachments");
            if(mode) check(!host.loadState(0,error),"quick state must reject incompatible attachment topology");
            check(host.prepareMovie(configuration,error),"recording restores its own attachment options");
            check((hardware()==0x84)==bool(mode&1) && bool(features()&2)==bool(mode&2),"recorded hardware topology");
            check(host.restoreMovieState(state,error),"recording restores hardware snapshot after topology change");
            check(host.restoreMovieConfiguration(error),"restore user options after recording");
            check(host.hardReset(error),"power cycle after recording");
        }
        check(host.load(silent,error),"silent ROM load");
        for(int i=0;i<4;++i){host.runFrame();check(!host.frameInputPolled(),"frontend polls must not count as game reads");}
        check(host.load(polling,error),"polling ROM reload");host.runFrame();check(host.frameInputPolled(),"polling resumes");
        check(host.load(silent,error),"silent ROM again");host.runFrame();check(!host.frameInputPolled(),"read flag does not leak across frames/games");
        std::puts("PASS: actual controller reads, four attachment combinations, cartridge execution, state restoration and pending power-cycle settings");
    } catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
