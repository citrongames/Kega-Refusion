#include "core/LibretroHost.h"
#include "core/DataFolders.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QJsonObject>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool value,const char* detail) { if (!value) throw std::runtime_error(detail); }
QByteArray cartridge() {
    QByteArray bytes(65536,char(0));
    bytes[0]=0x00; bytes[1]=char(0xff); bytes[2]=char(0xf0); bytes[3]=0x00;
    bytes[4]=0; bytes[5]=0; bytes[6]=2; bytes[7]=0;
    bytes.replace(0x100,16,"SEGA GENESIS    "); bytes[0x1f0]='U';
    bytes[0x200]=char(0x60); bytes[0x201]=char(0xfe); return bytes;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if (argc!=4) { std::fprintf(stderr,"usage: megald_host_tests CORE ORIGINAL_MMI BIOS_DIRECTORY\n"); return argc<4?77:2; }
    try {
        const auto path=QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
        const QFileInfo before(path); require(before.isFile(),"original MMI missing");
        const auto bios=QDir(QString::fromLocal8Bit(argv[3])).absolutePath();
        require(QFileInfo(bios).isDir(),"supplied original BIOS directory is missing");
        const auto biosBefore=QDir(bios).entryList(QDir::Files,QDir::Name);
        QTemporaryDir temp; require(temp.isValid(),"cannot create isolated session root");
        QString error;
        if (!DataFolders::save(temp.path(),{{QStringLiteral("bios"),bios}},error)) throw std::runtime_error(error.toStdString());
        LibretroHost host(QString::fromLocal8Bit(argv[1]),temp.path());
        host.setExpansionDevices(ExpansionMode::Disconnected,ExpansionMode::Connected);
        if (!host.load(path,error)) throw std::runtime_error(error.toStdString());
        require(host.isMegaLd() && !host.isCd(),"MMI routed through ordinary CD tray");
        const auto sides=host.discPaths();
        require(!sides.isEmpty() && sides.size()<=16 && host.discIndex()==0,"MMI side API is unavailable or invalid");
        for (const auto& side : sides) require(side==path,"MMI sides do not retain the same original path");
        require(host.gamePath()==path,"original MMI path not preserved");
        require(host.achievementConsoleId()==0 && host.achievementHash(error).isEmpty(),"MMI fell into full ROM achievements hashing");
        const auto identity=host.movieGameIdentity(); require(identity.size()==32,"MMI lacks bounded recording identity");
        const auto fingerprint=host.movieConfiguration().value(QStringLiteral("bios")).toObject();
        require(fingerprint.size()==1 && (fingerprint.contains(QStringLiteral("bios_LD_J.bin")) ||
            fingerprint.contains(QStringLiteral("bios_LD_U.bin"))),"MMI used ordinary CD/32X firmware identity");
        for (int i=0;i<120;++i) host.runFrame();
        require(!host.frame().isNull(),"MMI core did not present frames");
        bool changed=true;
        require(host.selectDisc(0,error,&changed) && !changed,"selecting current MMI side changed timeline");
        require(!host.selectDisc(sides.size(),error,&changed) && !changed,"invalid MMI side disturbed timeline");
        require(!host.changeDisc(path,error,&changed) && !changed,"MMI exposed ordinary disc replacement");
        require(!host.setDiscEjected(true,error,&changed) && !changed,"MMI exposed ordinary CD tray control");
        const int selected=sides.size()>1?1:0;
        if (selected) {
            const auto clock=host.runClock();
            if (!host.selectDisc(selected,error,&changed)) throw std::runtime_error(error.toStdString());
            require(changed && host.discIndex()==selected,"MMI side selection did not reach core");
            require(host.runClock().frames==clock.frames && host.runClock().ticks==clock.ticks,"MMI side selection reset the session clock");
            require(host.movieGameIdentity()==identity && host.gamePath()==path,"MMI side changed image identity/path");
            require(host.movieConfiguration().value(QStringLiteral("discIndex")).toInt(-1)==selected,"recording omitted selected MMI side");
            if (!host.saveState(0,error)) throw std::runtime_error(error.toStdString());
            if (!host.selectDisc(0,error,&changed)) throw std::runtime_error(error.toStdString());
            require(changed && host.discIndex()==0,"MMI side did not switch back");
            if (!host.loadState(0,error)) throw std::runtime_error(error.toStdString());
            require(host.discIndex()==selected,"quick-state restore did not report restored MMI side");
            const auto recorded=host.movieConfiguration();
            if (!host.selectDisc(0,error)) throw std::runtime_error(error.toStdString());
            if (!host.prepareMovie(recorded,error)) throw std::runtime_error(error.toStdString());
            require(host.discIndex()==selected,"recording preparation did not restore MMI side");
            if (!host.restoreMovieConfiguration(error)) throw std::runtime_error(error.toStdString());
        }
        if (!host.hardReset(error)) throw std::runtime_error(error.toStdString());
        require(host.isMegaLd() && host.movieGameIdentity()==identity && host.discIndex()==selected,"MMI reset lost side/media/identity");
        for (int i=0;i<5;++i) host.runFrame();
        host.setExpansionDevices(ExpansionMode::Auto,ExpansionMode::Auto);
        const auto rom=temp.filePath("generated.md");
        { QFile file(rom); require(file.open(QIODevice::WriteOnly),"cannot create original cartridge fixture");
          const auto bytes=cartridge(); require(file.write(bytes)==bytes.size(),"cannot write original cartridge fixture"); }
        if (!host.load(rom,error)) throw std::runtime_error(error.toStdString());
        require(!host.isMegaLd() && host.achievementConsoleId()!=0,"MMI mode leaked into ordinary cartridge");
        host.runFrame(); require(!host.frame().isNull(),"ordinary cartridge regression after MMI"); host.unload();
        const QFileInfo after(path);
        require(before.size()==after.size() && before.lastModified()==after.lastModified(),"original MMI changed");
        require(QDir(bios).entryList(QDir::Files,QDir::Name)==biosBefore,"core created BIOS copies in original folder");
        std::puts("MegaLD host tests passed"); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"MegaLD host tests failed: %s\n",error.what()); return 1; }
}
