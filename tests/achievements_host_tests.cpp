#include "core/LibretroHost.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void writeFile(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    require(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "fixture open failed");
    require(f.write(bytes) == bytes.size(), "fixture write failed");
}
QByteArray readFile(const QString& path) {
    QFile f(path);
    require(f.open(QIODevice::ReadOnly), "fixture read failed");
    return f.readAll();
}
QByteArray makeMd() {
    QByteArray rom(64 * 1024, char(0xff));
    qToBigEndian(quint32(0x00ff0000), rom.data());
    qToBigEndian(quint32(0x00000200), rom.data()+4);
    rom.replace(0x100,16,"SEGA GENESIS    ");
    rom.replace(0x120,16,"ACHIEVEMENT TEST");
    rom.replace(0x180,2,"GM");
    qToBigEndian(quint32(rom.size()-1), rom.data()+0x1a4);
    rom.replace(0x1b0,2,"RA");
    rom[0x1b2]=char(0xf8); rom[0x1b3]=char(0x20);
    qToBigEndian(quint32(0x00200000),rom.data()+0x1b4);
    qToBigEndian(quint32(0x0020ffff),rom.data()+0x1b8);
    rom.replace(0x1f0,1,"U");
    rom[0x200]=char(0x60); rom[0x201]=char(0xfe); // BRA.S *
    return rom;
}
QByteArray makeSmd(const QByteArray& rom) {
    require(!(rom.size()%0x4000), "SMD fixture alignment");
    QByteArray smd(512,char(0));
    for (qsizetype base=0;base<rom.size();base+=0x4000) {
        QByteArray block(0x4000,char(0));
        for (qsizetype i=0;i<0x2000;++i) {
            block[i]=rom[base+2*i+1];
            block[0x2000+i]=rom[base+2*i];
        }
        smd+=block;
    }
    return smd;
}
QByteArray makeZ80() {
    QByteArray rom(32*1024,char(0xff));
    rom[0]=char(0x18); rom[1]=char(0xfe); // JR *
    return rom;
}
QByteArray makeCdBios() {
    QByteArray bios(128*1024,char(0));
    qToBigEndian(quint32(0xfffffd00),bios.data());
    qToBigEndian(quint32(0x00000200),bios.data()+4);
    bios.replace(0x100,16,"SEGA MEGA DRIVE ");
    bios.replace(0x110,7,"(C)SEGA");
    bios.replace(0x120,16,"SEGA-CD BOOT ROM");
    bios.replace(0x150,16,"SEGA-CD BOOT ROM");
    bios.replace(0x180,3,"BR ");
    bios[0x1f0]='U';
    bios[0x200]=char(0x60);bios[0x201]=char(0xfe);
    qToBigEndian(quint16(0x60fe),bios.data()+0x18e);
    return bios;
}
QByteArray makeDisc(char marker) {
    QByteArray disc(150*2048,char(0));
    disc.replace(0,16,"SEGADISCSYSTEM  ");
    disc[0x1ff]=marker;
    disc[0x20b]=char(0); // NTSC-U
    return disc;
}
void checkMemory(LibretroHost& host, void* (*memoryData)(unsigned), unsigned bytes) {
    const auto* raw=static_cast<const uint8_t*>(memoryData(RETRO_MEMORY_SYSTEM_RAM));
    require(raw,"core did not expose system RAM");
    uint8_t actual[16]{};
    const auto count=std::min(bytes,16u);
    require(host.readAchievementMemory(0,actual,count)==count,"achievement RAM read failed");
    require(std::memcmp(actual,raw,count)==0,"achievement RAM differs from raw libretro bytes");
}
void checkHash(LibretroHost& host,const QByteArray& original) {
    QString error;
    const auto hash=host.achievementHash(error);
    require(error.isEmpty() && hash.size()==32,"RA ROM hash failed");
    require(hash.toLatin1()==QCryptographicHash::hash(original,QCryptographicHash::Md5).toHex(),
            "RA ROM hash differs from original source bytes");
}
void run(const QString& core) {
    QTemporaryDir temp;
    require(temp.isValid(),"temporary directory failed");
    const auto md=makeMd();
    const auto smd=makeSmd(md);
    const auto z80=makeZ80();
    writeFile(temp.filePath("fixture.md"),md);
    writeFile(temp.filePath("fixture.smd"),smd);
    for (const auto& ext : {"sms","gg","sg"}) writeFile(temp.filePath(QStringLiteral("fixture.")+ext),z80);
    QLibrary library(core);
    auto memoryData=reinterpret_cast<void* (*)(unsigned)>(library.resolve("retro_get_memory_data"));
    require(memoryData,"core memory export unavailable");
    LibretroHost host(core,temp.filePath("data"));
    QString error;
    require(host.load(temp.filePath("fixture.md"),error),qPrintable(error));
    require(host.achievementConsoleId()==1,"MD console id incorrect");
    checkHash(host,md);
    host.runFrame(false,false);
    checkMemory(host,memoryData,16);
    require(host.achievementHardcoreBlockReason().isEmpty(),"unmodified fixture blocked hardcore");
    require(host.setRemoveSpriteLimit(true,error),qPrintable(error));
    require(!host.achievementHardcoreBlockReason().isEmpty(),"removed sprite limit allowed hardcore");
    require(host.setRemoveSpriteLimit(false,error),qPrintable(error));
    require(host.setRegion(3,error),qPrintable(error));
    require(!host.achievementHardcoreBlockReason().isEmpty(),"PAL option allowed hardcore");
    require(host.setRegion(0,error),qPrintable(error));
    const auto* sram=static_cast<const uint8_t*>(memoryData(RETRO_MEMORY_SAVE_RAM));
    require(sram,"MD fixture SRAM unavailable");
    uint8_t save[8]{};
    require(host.readAchievementMemory(0x10000,save,8)==8 &&
            std::memcmp(save,sram,8)==0,"MD achievement SRAM mapping incorrect");

    require(host.saveState(0,error),qPrintable(error));
    const auto oldState=readFile(temp.filePath("data/states/fixture.slot0.krs"));
    require(oldState.mid(108,8)!="KRHOST01","legacy quick state was wrapped");
    QByteArray restored;
    int restores=0;
    host.captureAchievementProgress=[] { return QByteArray("achievements-progress-v1"); };
    host.restoreAchievementProgress=[&](const QByteArray& bytes) { restored=bytes; ++restores; };
    require(host.saveState(1,error),qPrintable(error));
    const auto newState=readFile(temp.filePath("data/states/fixture.slot1.krs"));
    require(newState.mid(108,8)=="KRHOST01","achievement quick state wrapper missing");
    require(host.loadState(1,error),qPrintable(error));
    require(restores==1 && restored=="achievements-progress-v1","achievement progress restore failed");
    require(host.loadState(0,error),qPrintable(error));
    require(restores==2 && restored.isEmpty(),"legacy quick state did not reset achievement progress");
    const auto beforeMalformed=host.serialize();
    auto malformed=newState;
    qToBigEndian(quint32(3*1024*1024),malformed.data()+108+12);
    malformed.replace(72,32,QCryptographicHash::hash(malformed.mid(108),QCryptographicHash::Sha256));
    writeFile(temp.filePath("data/states/fixture.slot2.krs"),malformed);
    require(!host.loadState(2,error),"oversized achievement progress accepted");
    require(restores==2 && host.serialize()==beforeMalformed,
            "malformed progress changed emulation or achievement state");

    require(host.load(temp.filePath("fixture.smd"),error),qPrintable(error));
    require(host.achievementConsoleId()==1,"SMD console id incorrect");
    checkHash(host,smd);
    host.runFrame(false,false);
    checkMemory(host,memoryData,16);
    for (const auto& entry : {std::pair{"sms",11u},std::pair{"gg",15u},std::pair{"sg",33u}}) {
        require(host.load(temp.filePath(QStringLiteral("fixture.")+entry.first),error),qPrintable(error));
        require(host.achievementConsoleId()==entry.second,"8-bit console id incorrect");
        checkHash(host,z80);
        host.runFrame(false,false);
        checkMemory(host,memoryData,entry.second==33u?4u:16u);
    }

    // Synthetic CD header and boot ROM: enough to test loading and hashing,
    // with no retail content or audio track decoding.
    require(QDir().mkpath(temp.filePath("data/bios")),"BIOS fixture directory failed");
    writeFile(temp.filePath("data/bios/bios_CD_U.bin"),makeCdBios());
    const auto discA=makeDisc('A'), discB=makeDisc('B');
    writeFile(temp.filePath("disc-a.iso"),discA);
    writeFile(temp.filePath("disc-b.iso"),discB);
    writeFile(temp.filePath("disc-a.cue"),
              "FILE \"disc-a.iso\" BINARY\r\n  TRACK 01 MODE1/2048\r\n    INDEX 01 00:00:00\r\n");
    require(host.load(temp.filePath("disc-a.cue"),error),qPrintable(error));
    require(host.achievementConsoleId()==9,"CD console id incorrect");
    require(host.setFastCdLoading(true,error),qPrintable(error));
    require(!host.achievementHardcoreBlockReason().isEmpty(),"fast CD loading allowed hardcore");
    require(host.setFastCdLoading(false,error),qPrintable(error));
    require(host.achievementHash(error).toLatin1()==
            QCryptographicHash::hash(discA.left(512),QCryptographicHash::Md5).toHex(),
            "CUE CD header hash incorrect");
    uint8_t cdMemory[16]{};
    for (uint32_t address : {0u,0x10000u,0x90000u})
        require(host.readAchievementMemory(address,cdMemory,sizeof(cdMemory))==sizeof(cdMemory),
                "CD achievement memory map incomplete");
    require(host.changeDisc(temp.filePath("disc-b.iso"),error),qPrintable(error));
    require(host.achievementHash(error).toLatin1()==
            QCryptographicHash::hash(discB.left(512),QCryptographicHash::Md5).toHex(),
            "CD hash was not refreshed after disc change");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        require(argc==2,"usage: achievements_host_tests <core>");
        run(QString::fromLocal8Bit(argv[1]));
        std::puts("achievements_host_tests PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"achievements_host_tests FAIL: %s\n",e.what());
        return 1;
    }
}
