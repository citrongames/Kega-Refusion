#include "core/LibretroHost.h"
#include <QCoreApplication>
#include <QFile>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
constexpr quint32 mailbox=0xff0000,controller=0xff8000;
constexpr int romSize=0x20000;
struct Code {
    QByteArray bytes;
    void w(quint16 value) { const int at=bytes.size();bytes.resize(at+2);qToBigEndian(value,bytes.data()+at); }
    void l(quint32 value) { w(quint16(value>>16));w(quint16(value)); }
    void immediate(quint16 value,quint32 address) { w(0x33fc);w(value);l(address); }
    void copyWord(quint32 from,quint32 to) { w(0x33f9);l(from);l(to); }
    void copyLong(quint32 from,quint32 to) { w(0x23f9);l(from);l(to); }
    void jump(quint32 address) { w(0x4ef9);l(address); }
};
// This original fixture runs its controller from RAM so every register write
// may remap the complete ROM, including the currently selected boot bank.
QByteArray fixture() {
    QByteArray rom(romSize,'\0');
    qToBigEndian<quint32>(0x00fffffc,rom.data());
    qToBigEndian<quint32>(0x00000200,rom.data()+4);
    rom.replace(0x100,16,"SEGA MEGA DRIVE ");rom.replace(0x120,20,"ORIGINAL BANK PROBE ");
    rom.replace(0x180,2,"GM");rom[0x1f0]='U';
    qToBigEndian<quint32>(romSize-1,rom.data()+0x1a4);
    qToBigEndian<quint32>(0xff0000,rom.data()+0x1a8);
    qToBigEndian<quint32>(0xffffff,rom.data()+0x1ac);
    Code ram;
    // Input initialization polls real VBlank while extended video is blanked.
    ram.immediate(0x9880,0xc00004);                       // Firecore register $18=$80
    ram.immediate(0x8104,0xc00004);                       // mode 5, display off
    for(bool blank:{false,true}) {
        const int poll=ram.bytes.size();
        ram.w(0x3039);ram.l(0xc00004);                    // MOVE.W VDP status,D0
        ram.w(0x0240);ram.w(8);                          // ANDI.W #VBlank,D0
        ram.w(blank?0x6700:0x6600);                     // wait for each hardware phase
        const int displacement=ram.bytes.size();ram.w(quint16(poll-displacement));
    }
    ram.immediate(0x9800,0xc00004);                       // subsequent DMA uses ordinary mode
    ram.immediate(0x5a01,mailbox+0x26);                  // both phases observed
    const int loop=ram.bytes.size();
    ram.w(0x3039);ram.l(mailbox);                         // MOVE.W command,D0
    ram.w(0x6700);const int idleBranch=ram.bytes.size();ram.w(0);
    ram.w(0x0c40);ram.w(3);                              // command 3 also selects flash chip
    ram.w(0x6600);const int gpioBranch=ram.bytes.size();ram.w(0);
    ram.copyWord(mailbox+0x24,0xa10104);                  // active-low GPIO bit 5
    const int afterGpio=ram.bytes.size();
    ram.w(0x0c40);ram.w(2);                              // command 2 queries existing mapping
    ram.w(0x6700);const int queryBranch=ram.bytes.size();ram.w(0);
    ram.copyWord(mailbox+2,0xb01028);                     // low 16 bits, byte offset
    ram.copyWord(mailbox+4,0xb0102a);                     // high 16 bits
    const int query=ram.bytes.size();
    ram.w(0x2079);ram.l(mailbox+8);                       // MOVEA.L sample address,A0
    ram.w(0x23d0);ram.l(mailbox+0x10);                    // MOVE.L (A0),sample result
    ram.w(0x4eb9);ram.l(0x4000);                          // JSR mapped ROM code
    // Short ordinary VRAM DMA checks the mapper's DMA path as well as CPU
    // reads. Mailbox words contain registers $15/$16 for the sample address.
    for(quint16 reg:{0x8004,0x8114,0x8f02,0x9302,0x9400,0x9700})
        ram.immediate(reg,0xc00004);
    ram.copyWord(mailbox+0x20,0xc00004);ram.copyWord(mailbox+0x22,0xc00004);
    ram.w(0x23fc);ram.l(0x40000080);ram.l(0xc00004);       // DMA two words to VRAM[0]
    ram.w(0x23fc);ram.l(0);ram.l(0xc00004);                // VRAM read at 0
    ram.copyLong(0xc00000,mailbox+0x18);
    ram.w(0x4279);ram.l(mailbox);                         // CLR.W command (completion)
    ram.jump(controller+quint32(loop));
    qToBigEndian<quint16>(quint16(loop-idleBranch),ram.bytes.data()+idleBranch);
    qToBigEndian<quint16>(quint16(query-queryBranch),ram.bytes.data()+queryBranch);
    qToBigEndian<quint16>(quint16(afterGpio-gpioBranch),ram.bytes.data()+gpioBranch);
    rom.replace(0x800,ram.bytes.size(),ram.bytes);
    Code boot;
    boot.w(0x46fc);boot.w(0x2700);                        // mask interrupts
    boot.w(0x41f9);boot.l(0x800);                         // LEA ROM controller,A0
    boot.w(0x43f9);boot.l(controller);                    // LEA RAM controller,A1
    boot.w(0x303c);boot.w(quint16(ram.bytes.size()/2-1));
    boot.w(0x32d8);boot.w(0x51c8);boot.w(0xfffc);           // MOVE.W (A0)+,(A1)+; DBRA
    boot.jump(controller);rom.replace(0x200,boot.bytes.size(),boot.bytes);
    const quint32 bases[]={0,0x1002,0x2004,0x12004,0x10002};
    for(unsigned i=0;i<5;++i) {
        qToBigEndian<quint32>(0xa1b20000u+i,rom.data()+bases[i]+0x1000);
        Code stub;stub.immediate(quint16(0xc100+i),mailbox+0x14);stub.w(0x4e75);
        rom.replace(bases[i]+0x4000,stub.bytes.size(),stub.bytes);
    }
    rom[romSize-2]=char(0xca);rom[romSize-1]=char(0xfe);
    return rom;
}
// The two concatenated flash chips carry independent executable signatures.
QByteArray dualFixture() {
    constexpr int chipSize=0x800000;
    QByteArray rom(chipSize*2,'\0');const auto image=fixture();
    for(unsigned chip=0;chip<2;++chip) {
        const int origin=int(chip)*chipSize;rom.replace(origin,image.size(),image);
        if(!chip)rom.replace(0x100,16,"DMC RedKid      ");
        qToBigEndian<quint32>(chipSize-1,rom.data()+origin+0x1a4);
        const quint32 bases[]={0,0x1002,0x2004,0x12004,0x10002,0x7f0002};
        for(unsigned i=0;i<6;++i) {
            qToBigEndian<quint32>((chip?0xd3e40000u:0xa1b20000u)+i,rom.data()+origin+bases[i]+0x1000);
            Code stub;stub.immediate(quint16((chip?0xd100:0xc100)+i),mailbox+0x14);stub.w(0x4e75);
            rom.replace(origin+bases[i]+0x4000,stub.bytes.size(),stub.bytes);
        }
        qToBigEndian<quint16>(chip?0xbeef:0xcafe,rom.data()+origin+chipSize-2);
    }
    return rom;
}
struct Probe {
    LibretroHost host;
    QLibrary library;
    void* (*memory)(unsigned)=nullptr;
    size_t (*stateSize)()=nullptr;
    bool (*serialize)(void*,size_t)=nullptr;
    bool (*unserialize)(const void*,size_t)=nullptr;
    Probe(const QString& core,const QString& data):host(core,data),library(core) {
        memory=reinterpret_cast<decltype(memory)>(library.resolve("retro_get_memory_data"));
        stateSize=reinterpret_cast<decltype(stateSize)>(library.resolve("retro_serialize_size"));
        serialize=reinterpret_cast<decltype(serialize)>(library.resolve("retro_serialize"));
        unserialize=reinterpret_cast<decltype(unserialize)>(library.resolve("retro_unserialize"));
        require(memory && stateSize && serialize && unserialize,"core memory/state exports");
    }
    void putByte(unsigned offset,quint8 value) {
        auto* ram=static_cast<quint8*>(memory(RETRO_MEMORY_SYSTEM_RAM));require(ram,"system RAM");
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
        offset^=1u;
#endif
        ram[offset]=value;
    }
    void putWord(unsigned offset,quint16 value) { putByte(offset,quint8(value>>8));putByte(offset+1,quint8(value)); }
    void putLong(unsigned offset,quint32 value) { putWord(offset,quint16(value>>16));putWord(offset+2,quint16(value)); }
    QByteArray ram() {
        QByteArray result;quint32 base=0;bool bigEndian=false;QString error;
        require(host.cheatMemory(result,base,bigEndian,error),qPrintable(error));
        require(base==mailbox && bigEndian && result.size()==65536,"normalized MD RAM");return result;
    }
    void ready() {
        for(int i=0;i<4;++i) {
            host.runFrame(false,false);const auto result=ram();
            if(qFromBigEndian<quint16>(result.constData()+0x26)==0x5a01)return;
        }
        require(false,"display-off extended VBlank did not cycle through active and blank phases");
    }
    void sample(bool select,quint32 base,quint32 address,quint32 expected,quint16 code,const char* label,int gpio=-1) {
        putWord(2,quint16(base));putWord(4,quint16(base>>16));putLong(8,address);
        putLong(0x10,0);putWord(0x14,0);putLong(0x18,0);
        putWord(0x20,quint16(0x9500|((address>>1)&0xff)));
        putWord(0x22,quint16(0x9600|((address>>9)&0xff)));
        if(gpio>=0)putWord(0x24,quint16(gpio));
        putWord(0,gpio>=0?3:(select?1:2));
        QByteArray result;
        for(int i=0;i<8;++i) {
            host.runFrame(false,false);result=ram();
            if(qFromBigEndian<quint16>(result.constData())==0)break;
        }
        if(qFromBigEndian<quint16>(result.constData())!=0 ||
           qFromBigEndian<quint32>(result.constData()+0x10)!=expected ||
           qFromBigEndian<quint16>(result.constData()+0x14)!=code ||
           qFromBigEndian<quint32>(result.constData()+0x18)!=expected) {
            std::fprintf(stderr,"%s: cmd=%04x CPU=%08x code=%04x DMA=%08x; expected %08x/%04x\n",label,
                qFromBigEndian<quint16>(result.constData()),qFromBigEndian<quint32>(result.constData()+0x10),
                qFromBigEndian<quint16>(result.constData()+0x14),qFromBigEndian<quint32>(result.constData()+0x18),expected,code);
            throw std::runtime_error(label);
        }
    }
    QByteArray nativeState() {
        QByteArray state(qsizetype(stateSize()),'\0');
        require(serialize(state.data(),size_t(state.size())),"capture native state");return state;
    }
};
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if(argc!=2) { std::fprintf(stderr,"Usage: firecore_bank_tests CORE\n");return 2; }
    try {
        QTemporaryDir temp;require(temp.isValid(),"fixture directory");
        const auto path=temp.filePath("Original Bank Probe.atf");
        QFile file(path);const auto rom=fixture();
        require(file.open(QIODevice::WriteOnly) && file.write(rom)==rom.size(),"write generated ROM");file.close();
        Probe probe(QString::fromLocal8Bit(argv[1]),temp.filePath("data"));QString error;
        require(probe.host.load(path,error),qPrintable(error));require(probe.host.isFirecore(),"explicit ATF profile");
        probe.ready();
        probe.sample(false,0,0x1000,0xa1b20000,0xc100,"power-on base zero");
        auto legacy=probe.nativeState();
        require(legacy.size()>=160,"mapper trailer space");
        // Previous Firecore states reserved these bytes but left them zero.
        legacy.replace(legacy.size()-160,160,QByteArray(160,'\0'));
        probe.sample(true,0x1002,0x1000,0xa1b20001,0xc101,"low word byte offset");
        probe.sample(true,0x1002,0x1000,0xa1b20001,0xc101,"repeated register values");
        probe.sample(true,0x2004,0x1000,0xa1b20002,0xc102,"replace low word");
        probe.sample(true,0x12004,0x1000,0xa1b20003,0xc103,"high word byte offset");
        probe.sample(true,0x2004,0x1000,0xa1b20002,0xc102,"replace high word with zero");
        probe.sample(true,0x10002,0x1000,0xa1b20004,0xc104,"even unaligned bank");
        probe.sample(false,0x10002,0xfffc,0xcafe00ff,0xc104,"CPU and DMA wrap within 64 KiB page");
        const auto saved=probe.host.captureMovieState();require(!saved.isEmpty(),"capture nonzero movie state");
        const auto native=probe.nativeState();
        require(native.mid(native.size()-160,8)=="KRFCBNK1" &&
                qFromLittleEndian<quint32>(native.constData()+native.size()-152)==0x10002,"nonzero mapper trailer");
        probe.sample(true,0x2004,0x1000,0xa1b20002,0xc102,"change bank after save");
        require(probe.host.restoreMovieState(saved,error),qPrintable(error));
        probe.sample(false,0x10002,0x1000,0xa1b20004,0xc104,"nonzero state restores data DMA and ROM execution");
        require(probe.unserialize(legacy.constData(),size_t(legacy.size())),"restore legacy native Firecore state");
        probe.sample(false,0,0x1000,0xa1b20000,0xc100,"legacy Firecore state defaults to base zero");
        probe.sample(true,0x10002,0x1000,0xa1b20004,0xc104,"select nonzero bank before reset");
        require(probe.host.hardReset(error),qPrintable(error));probe.ready();
        probe.sample(false,0,0x1000,0xa1b20000,0xc100,"hard reset defaults to base zero");
        const auto dualPath=temp.filePath("Original Dual Flash Probe.atf");
        QFile dualFile(dualPath);const auto dualRom=dualFixture();
        require(dualFile.open(QIODevice::WriteOnly) && dualFile.write(dualRom)==dualRom.size(),"write generated dual-flash ROM");dualFile.close();
        require(probe.host.load(dualPath,error),qPrintable(error));probe.ready();
        probe.sample(false,0,0x1000,0xa1b20000,0xc100,"dual-flash power-on first chip");
        auto dualLegacy=probe.nativeState();dualLegacy.replace(dualLegacy.size()-160,160,QByteArray(160,'\0'));
        probe.sample(true,0x10002,0x1000,0xa1b20004,0xc104,"dual-flash first chip local offset",0x20);
        probe.sample(true,0x10002,0x1000,0xd3e40004,0xd104,"dual-flash active-low second chip",0);
        probe.sample(true,0x10002,0x1000,0xd3e40004,0xd104,"dual-flash repeat selector",0);
        const auto dualSaved=probe.host.captureMovieState();require(!dualSaved.isEmpty(),"capture second-chip state");
        probe.sample(true,0x10002,0x1000,0xa1b20004,0xc104,"dual-flash switch back at same offset",0x20);
        require(probe.host.restoreMovieState(dualSaved,error),qPrintable(error));
        probe.sample(false,0x10002,0x1000,0xd3e40004,0xd104,"dual-flash state restores second chip and offset");
        probe.sample(true,0x7f0002,0x1000,0xd3e40005,0xd105,"dual-flash second chip end bank",0);
        probe.sample(false,0x7f0002,0xfffc,0xbeef00ff,0xd105,"dual-flash second chip wraps within chip");
        probe.sample(true,0x7f0002,0x1000,0xa1b20005,0xc105,"dual-flash first chip end bank",0x20);
        probe.sample(false,0x7f0002,0xfffc,0xcafe00ff,0xc105,"dual-flash first chip wraps within chip");
        probe.sample(true,0,0x1000,0xd3e40000,0xd100,"dual-flash second chip zero offset",0);
        const auto chipOnly=probe.host.captureMovieState();require(!chipOnly.isEmpty(),"capture chip-only selection");
        probe.sample(true,0,0x1000,0xa1b20000,0xc100,"dual-flash first chip zero offset",0x20);
        require(probe.host.restoreMovieState(chipOnly,error),qPrintable(error));
        probe.sample(false,0,0x1000,0xd3e40000,0xd100,"dual-flash zero-offset state retains chip selection");
        require(probe.unserialize(dualLegacy.constData(),size_t(dualLegacy.size())),"restore dual-flash legacy state");
        probe.sample(false,0,0x1000,0xa1b20000,0xc100,"dual-flash legacy state defaults first chip base zero");
        probe.sample(true,0x10002,0x1000,0xd3e40004,0xd104,"dual-flash second chip before power reset",0);
        require(probe.host.hardReset(error),qPrintable(error));probe.ready();
        probe.sample(false,0,0x1000,0xa1b20000,0xc100,"dual-flash power reset restores first chip base zero");
        std::puts("Firecore VBlank, bank registers, CPU fetch/read, DMA, wrap, states and dual 8 MiB flash passed");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what());return 1; }
}
