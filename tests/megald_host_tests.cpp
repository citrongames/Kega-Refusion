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
        require(host.isMegaLd() && !host.isCd() && host.discPaths().isEmpty(),"MMI routed through ordinary CD tray");
        require(host.gamePath()==path,"original MMI path not preserved");
        require(host.achievementConsoleId()==0 && host.achievementHash(error).isEmpty(),"MMI fell into full ROM achievements hashing");
        const auto identity=host.movieGameIdentity(); require(identity.size()==32,"MMI lacks bounded recording identity");
        const auto fingerprint=host.movieConfiguration().value(QStringLiteral("bios")).toObject();
        require(fingerprint.size()==1 && (fingerprint.contains(QStringLiteral("bios_LD_J.bin")) ||
            fingerprint.contains(QStringLiteral("bios_LD_U.bin"))),"MMI used ordinary CD/32X firmware identity");
        for (int i=0;i<120;++i) host.runFrame();
        require(!host.frame().isNull(),"MMI core did not present frames");
        if (!host.hardReset(error)) throw std::runtime_error(error.toStdString());
        require(host.isMegaLd() && host.movieGameIdentity()==identity,"MMI reset lost media/identity");
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
