#include "core/LibretroHost.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtEndian>

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}
void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(bytes)==bytes.size(),"write original cheat probe ROM");
}
void putLong(QByteArray& data,int at,quint32 value) {
    qToBigEndian(value,data.data()+at);
}
QByteArray mdRom() {
    QByteArray rom(64*1024,char(0));
    putLong(rom,0,0x00ff0000); putLong(rom,4,0x00000200);
    rom.replace(0x100,16,"SEGA MEGA DRIVE ");
    rom.replace(0x120,16,"ORIGINAL CHEAT  ");
    rom.replace(0x180,2,"GM");
    rom.replace(0x1f0,3,"USA");
    int pc=0x200;
    const auto word=[&](quint16 value){rom[pc++]=char(value>>8);rom[pc++]=char(value);};
    const auto address=[&](quint32 value){word(quint16(value>>16));word(quint16(value));};
    word(0x46fc); word(0x2700); // mask interrupts while repeatedly sampling RAM
    const int loop=pc;
    for (unsigned i=0;i<8;++i) {
        word(0x1039); address(0x00ff0100+i); // MOVE.B source,D0
        word(0x13c0); address(0x00ff0200+i); // MOVE.B D0,echo
    }
    word(0x4ef9); address(quint32(loop));
    return rom;
}
QByteArray smsRom() {
    QByteArray rom(32*1024,char(0));
    int pc=0;
    const auto byte=[&](quint8 value){rom[pc++]=char(value);};
    byte(0xf3); byte(0x31); byte(0xf0); byte(0xdf); // DI; LD SP,DFF0
    const int loop=pc;
    for (unsigned i=0;i<8;++i) {
        byte(0x3a); byte(quint8(0x100+i)); byte(0xc1); // LD A,(C100+i)
        byte(0x32); byte(quint8(0x200+i)); byte(0xc2); // LD (C200+i),A
    }
    byte(0xc3); byte(quint8(loop)); byte(quint8(loop>>8));
    rom.replace(0x7ff0,8,"TMR SEGA"); rom[0x7fff]=char(0x4c);
    return rom;
}
using Memory=void* (*)(unsigned);
struct Probe {
    LibretroHost host;
    QLibrary library;
    Memory memory=nullptr;
    bool md;
    Probe(const QString& core,const QString& data,bool megaDrive)
        : host(core,data),library(core),md(megaDrive) {
        memory=reinterpret_cast<Memory>(library.resolve("retro_get_memory_data"));
        require(memory!=nullptr,"core RAM export");
    }
    unsigned char* ram() const {
        auto* result=static_cast<unsigned char*>(memory(RETRO_MEMORY_SYSTEM_RAM));
        require(result!=nullptr,"core system RAM");
        return result;
    }
    static unsigned physical(unsigned offset,bool md) {
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
        return md ? offset^1u : offset;
#else
        Q_UNUSED(md);
        return offset;
#endif
    }
    void set(unsigned offset,unsigned value) {
        ram()[physical(offset,md)]=quint8(value);
    }
    unsigned get(unsigned offset) const {
        return ram()[physical(offset,md)];
    }
    void source(const std::array<unsigned,8>& values) {
        for (unsigned i=0;i<values.size();++i) set(0x100+i,values[i]);
    }
    void frames(int count=2) {
        while (count--) host.runFrame(false,false);
    }
    void expect(const std::array<unsigned,8>& values,const char* message) const {
        QByteArray snapshot; quint32 base=0; bool bigEndian=false; QString error;
        require(host.cheatMemory(snapshot,base,bigEndian,error),qPrintable(error));
        require(base==(md?0xff0000u:0xc000u) && bigEndian==md &&
                snapshot.size()==(md?65536:8192),"normalized RAM range or endian");
        for (unsigned i=0;i<values.size();++i)
            require(quint8(snapshot[0x100+i])==values[i] &&
                    get(0x100+i)==values[i] &&
                    get(0x200+i)==values[i],message);
    }
};
CheatEntry code(quint32 address,int width,bool bigEndian,quint32 value,const char* label) {
    const auto native=Cheats::ramCode(address,width,bigEndian,value);
    require(!native.isEmpty(),"generate native grouped RAM code");
    return {native,QString::fromLatin1(label),true};
}
void megaDrive(const QString& core) {
    QTemporaryDir temp; require(temp.isValid(),"MD cheat probe directory");
    const QString path=temp.filePath("Original Cheat Fixture.md");
    const QString renamed=temp.filePath("Original Renamed Fixture.md");
    const auto rom=mdRom(); writeFile(path,rom); writeFile(renamed,rom);
    Probe probe(core,temp.path(),true); QString error;
    require(probe.host.load(path,error),qPrintable(error));
    require(probe.host.cheatsSupported(),"MD cheat support");
    const std::array<unsigned,8> sentinel{0xa1,0xb2,0xc3,0xd4,0xe5,0xf6,0x87,0x98};
    probe.source(sentinel); probe.frames(); probe.expect(sentinel,"MD baseline CPU echo / endian");
    const CheatEntry byte=code(0xff0100,1,true,0x7f,"byte freeze");
    const CheatEntry word=code(0xff0102,2,true,0x0034,"word freeze high zero");
    const CheatEntry longword=code(0xff0104,4,true,0x00123400,"long freeze high/low zero");
    const CheatList all{byte,word,longword};
    bool changed=false;
    require(probe.host.setCheats(all,error,&changed) && changed,qPrintable(error));
    const std::array<unsigned,8> frozen{0x7f,0xb2,0x00,0x34,0x00,0x12,0x34,0x00};
    probe.frames(); probe.expect(frozen,"MD 8/16/32-bit freeze or neighbor changed");
    CheatList partial=all; partial[1].enabled=false;
    require(probe.host.setCheats(partial,error,&changed) && changed,qPrintable(error));
    const std::array<unsigned,8> alternate{0xa0,0xb2,0xcd,0xef,0x11,0x22,0x33,0x44};
    probe.source(alternate); probe.frames();
    probe.expect({0x7f,0xb2,0xcd,0xef,0x00,0x12,0x34,0x00},
                 "disabling one grouped cheat did not spare neighbor / other groups");
    require(probe.host.setCheats(all,error,&changed) && changed,qPrintable(error));
    probe.frames(); probe.expect(frozen,"re-enabling grouped word did not restore freeze");
    probe.host.reset();
    probe.source(alternate); probe.frames(); probe.expect(frozen,"reset lost compound RAM freeze");
    require(probe.host.setCheats({},error,&changed) && changed,qPrintable(error));
    probe.source(alternate); probe.frames(); probe.expect(alternate,"disabled cheats still froze MD RAM");

    // A grouped code longer than GPX's 255-byte per-call buffer must be
    // flattened by the host into separate native atoms.
    QStringList parts;
    for (unsigned i=0;i<30;++i)
        parts.append(Cheats::ramCode(0xff0300+i,1,true,i+1));
    const QString longGroup=parts.join(QLatin1Char('+'));
    require(longGroup.size()>255,"long grouped cheat fixture too short");
    CheatList withGroup=all;
    withGroup.append({longGroup,QStringLiteral("thirty native bytes"),true});
    require(probe.host.setCheats(withGroup,error,&changed) && changed,qPrintable(error));
    for (unsigned i=0;i<30;++i) probe.set(0x300+i,0);
    probe.source(sentinel); probe.frames(); probe.expect(frozen,"grouped MD freeze failed");
    QByteArray snapshot; quint32 base=0; bool bigEndian=false;
    require(probe.host.cheatMemory(snapshot,base,bigEndian,error),qPrintable(error));
    for (unsigned i=0;i<30;++i)
        require(quint8(snapshot[0x300+i])==i+1,
                "grouped native code truncated after 255 characters");

    probe.host.unload();
    require(probe.host.load(path,error),qPrintable(error));
    require(probe.host.cheats()==withGroup,"filename cheat storage did not survive reload");
    probe.source(sentinel);
    for (unsigned i=0;i<30;++i) probe.set(0x300+i,0);
    probe.frames(); probe.expect(frozen,"reloaded MD cheats did not freeze");
    require(probe.host.cheatMemory(snapshot,base,bigEndian,error),qPrintable(error));
    require(quint8(snapshot[0x300+29])==30,"reloaded long group lost final native atom");
    const QDir saved(temp.filePath("cheats"));
    require(saved.entryList({QStringLiteral("*.json")},QDir::Files).size()==1,
            "per-filename cheat JSON was not saved");
    probe.host.unload();
    require(probe.host.load(renamed,error),qPrintable(error));
    require(probe.host.cheats().isEmpty(),"renamed ROM reused another filename's cheats");
    probe.source(sentinel); probe.frames(); probe.expect(sentinel,"renamed ROM inherited freeze");
}
void masterSystem(const QString& core) {
    QTemporaryDir temp; require(temp.isValid(),"SMS cheat probe directory");
    const QString path=temp.filePath("Original Cheat Fixture.sms");
    writeFile(path,smsRom());
    Probe probe(core,temp.path(),false); QString error;
    require(probe.host.load(path,error),qPrintable(error));
    require(probe.host.cheatsSupported(),"SMS cheat support");
    const std::array<unsigned,8> sentinel{0xa1,0xb2,0xc3,0xd4,0xe5,0xf6,0x87,0x98};
    probe.source(sentinel); probe.frames(); probe.expect(sentinel,"SMS baseline CPU echo / endian");
    const CheatList codes{
        code(0xc100,1,false,0x7f,"SMS byte"),
        code(0xc102,2,false,0x0034,"SMS word high zero"),
        code(0xc104,4,false,0x00123400,"SMS long high/low zero")};
    require(probe.host.setCheats(codes,error),qPrintable(error));
    probe.frames();
    probe.expect({0x7f,0xb2,0x34,0x00,0x00,0x34,0x12,0x00},
                 "SMS 8/16/32-bit freeze or neighbor changed");
    require(probe.host.setCheats({},error),qPrintable(error));
    probe.source(sentinel); probe.frames();
    probe.expect(sentinel,"SMS disabling did not stop freeze");
}
} // namespace

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        require(argc==2,"usage: cheat_integration_tests CORE");
        const QString core=QString::fromLocal8Bit(argv[1]);
        megaDrive(core);
        masterSystem(core);
        std::puts("PASS: original MD/SMS CPU RAM echo, normalized search memory, grouped 8/16/32-bit freeze, persistence");
        return 0;
    } catch(const std::exception& failure) {
        std::fprintf(stderr,"FAIL: %s\n",failure.what());
        return 1;
    }
}
