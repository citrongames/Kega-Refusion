#include "core/LibretroHost.h"
#include "net/NetplayTimeline.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtEndian>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
          "write generated hardware probe ROM");
}
using Memory = void* (*)(unsigned);
struct Probe {
    LibretroHost host;
    QLibrary library;
    Memory memory = nullptr;
    explicit Probe(const QString& core, const QString& data)
        : host(core,data), library(core) {
        memory = reinterpret_cast<Memory>(library.resolve("retro_get_memory_data"));
        check(memory != nullptr, "core RAM export unavailable");
    }
    unsigned byte(unsigned offset, bool md = true) const {
        auto* ram = static_cast<const unsigned char*>(memory(RETRO_MEMORY_SYSTEM_RAM));
        check(ram != nullptr, "system RAM unavailable");
        return ram[md ? (offset^1) : offset];
    }
    quint16 word(unsigned offset) const {
        auto* ram = static_cast<const char*>(memory(RETRO_MEMORY_SYSTEM_RAM));
        check(ram != nullptr, "system RAM unavailable");
        quint16 value = 0; std::memcpy(&value,ram+offset,2); return value;
    }
    void frames(int count = 3) { while (count--) host.runFrame(false,false); }
};
struct MdCode {
    QByteArray rom = QByteArray(64*1024, char(0));
    int pc = 0x200;
    MdCode(bool jcart) {
        qToBigEndian<quint32>(0x00ff0000,rom.data());
        qToBigEndian<quint32>(0x00000200,rom.data()+4);
        rom.replace(0x100,16,"SEGA MEGA DRIVE ");
        rom.replace(0x180,2,"GM");
        rom.replace(0x1f0,3,"USA");
        if (jcart) rom.replace(0x182,12,"T-123456-00 ");
    }
    void word(quint16 value) { rom[pc++]=char(value>>8); rom[pc++]=char(value); }
    void address(quint32 value) { word(quint16(value>>16)); word(quint16(value)); }
    void putByte(quint8 value,quint32 address_) { word(0x13fc); word(value); address(address_); }
    void putWord(quint16 value,quint32 address_) { word(0x33fc); word(value); address(address_); }
    void readByte(quint32 address_,unsigned offset) {
        word(0x1039); address(address_); word(0x13c0); address(0x00ff0000+offset);
    }
    void readWord(quint32 address_,unsigned offset) {
        word(0x3039); address(address_); word(0x33c0); address(0x00ff0000+offset);
    }
    void jump(int destination) { word(0x4ef9); address(quint32(destination)); }
};
QByteArray jcartRom(bool wayPlay) {
    MdCode code(true);
    code.word(0x46fc); code.word(0x2700); // mask interrupts during I/O probe
    const int loop=code.pc;
    if (wayPlay) {
        code.putByte(0x40,0xa10009); code.putByte(0x7f,0xa1000b);
        code.putByte(0x40,0xa10003);
        for (unsigned player=0;player<4;++player) {
            code.putByte(quint8(player<<4),0xa10005);
            code.readByte(0xa10003,player);
        }
    } else {
        for (unsigned port=0;port<2;++port) {
            const quint32 data=0xa10003+port*2;
            code.putByte(0x60,0xa10009+port*2);
            code.putByte(0x60,data); code.readByte(data,port*32);
            for (unsigned phase=1;phase<20;++phase) {
                code.putByte(phase&1 ? 0x20 : 0,data);
                code.readByte(data,port*32+phase);
            }
        }
    }
    code.putWord(1,0x380000); code.readWord(0x380000,64); // J-Cart TH high
    code.putWord(0,0x380000); code.readWord(0x380000,66); // TH low
    code.jump(loop);
    return code.rom;
}
QByteArray mouseRom() {
    MdCode code(false);
    code.word(0x46fc); code.word(0x2700);
    for (unsigned port=0;port<2;++port) {
        const quint32 data=0xa10003+port*2;
        code.putByte(0x60,0xa10009+port*2);
        code.putByte(0x60,data);
        code.putByte(0x20,data); // TH falling starts mouse acquisition
        for (unsigned phase=0;phase<9;++phase) {
            if (port==0 && phase==5) {
                const int wait=code.pc;
                code.word(0x4a39); code.address(0x00ff0100); // TST.B resume flag
                const int branch=code.pc; code.word(0x6700); // BEQ.S wait
                const int displacement=wait-(branch+2);
                check(displacement>=-128 && displacement<0,"mouse wait branch range");
                code.rom[branch+1]=char(displacement);
            }
            if (phase) code.putByte(phase&1 ? 0 : 0x20,data);
            // The core acknowledges a TR edge after two reads. Store the third.
            code.readByte(data,port*32+phase); // temporary sample, overwritten below
            code.readByte(data,port*32+phase);
            code.readByte(data,port*32+phase);
        }
        code.putByte(0x60,data); // TH rising ends acquisition
    }
    code.word(0x4e72); code.word(0x2700); // STOP with interrupts masked
    return code.rom;
}
QByteArray paddleRom() {
    QByteArray rom(32768,char(0)); int pc=0;
    const auto byte=[&](quint8 value){rom[pc++]=char(value);};
    const auto sample=[&](quint8 port,quint16 address){
        byte(0xdb); byte(port); byte(0x32); byte(quint8(address)); byte(quint8(address>>8));
    };
    byte(0xf3); byte(0x31); byte(0xf0); byte(0xdf); // DI; LD SP,DFF0
    sample(0xdc,0xc000); sample(0xdd,0xc001); // high nibbles
    const int wait=pc;
    byte(0x3a); byte(0x00); byte(0xc1); // LD A,(C100): host-controlled resume flag
    byte(0xb7); // OR A
    byte(0x28); byte(quint8(wait-(pc+1))); // JR Z,wait
    sample(0xdc,0xc002); sample(0xdd,0xc003); // low nibbles
    byte(0x76); // HALT after the paired I/O samples
    rom.replace(0x7ff0,8,"TMR SEGA"); rom[0x7fff]=char(0x4c);
    return rom;
}
unsigned jcartNibble(quint16 data,unsigned pad) {
    return (data >> (pad ? 8 : 0)) & 15;
}
MovieInput localInput(quint16 buttons,int x=0,int y=0) {
    MovieInput local;
    local.buttons[0]=buttons;
    local.aim[0]=quint16(qint16(x)) | (quint64(quint16(qint16(y)))<<16);
    return local;
}
void networkInput(LibretroHost& host,int role,quint16 buttons,int x=0,int y=0,
                  unsigned relativeRolesMask=0) {
    QList<QPair<int,MovieInput>> contributions;
    contributions.append(qMakePair(role,localInput(buttons,x,y)));
    const auto merged=NetplayTimeline::merge(contributions,relativeRolesMask);
    NetplayTimeline::apply(host,merged);
}
void jcart(const QString& core) {
    QTemporaryDir temp; check(temp.isValid(),"J-Cart probe directory");
    const QString normal=temp.filePath("original-jcart.md");
    const QString way=temp.filePath("original-jcart-way.md");
    writeFile(normal,jcartRom(false)); writeFile(way,jcartRom(true));
    Probe probe(core,temp.path()); QString error;
    check(probe.host.load(normal,error),qPrintable(error));
    check(probe.host.hasJCart() && probe.host.jcartPlayerBase()==2 &&
          probe.host.controllerPlayerCount()==4,"J-Cart header or default players");
    for (unsigned player=0;player<8;++player) {
        probe.host.setControllerKind(player,ControllerType::Pad);
        probe.host.setControllerConnected(player,true);
        probe.host.setControllerSixButton(player,false);
    }
    probe.host.setButtons(2,1u<<4); probe.host.setButtons(3,1u<<7);
    probe.frames();
    check(jcartNibble(probe.word(64),0)==14 && jcartNibble(probe.word(64),1)==7,
          "J-Cart P3/P4 directional routing");
    check((jcartNibble(probe.word(66),0)&12)==0 &&
          (jcartNibble(probe.word(66),1)&12)==0,"J-Cart TH write did not reach both pads");
    probe.host.setControllerConnected(2,false); probe.frames();
    check(jcartNibble(probe.word(64),0)==15 && jcartNibble(probe.word(64),1)==7,
          "J-Cart disconnected P3 shifted P4");
    probe.host.setControllerConnected(2,true);
    const auto configuration=probe.host.movieConfiguration();
    const auto stateBeforeInvalid=probe.host.serialize();
    check(!configuration.isEmpty() && !stateBeforeInvalid.isEmpty(),"J-Cart movie baseline");
    for (bool wrongCart : {true,false}) {
        auto invalid=configuration;
        if (wrongCart) invalid.insert(QStringLiteral("jcart"),false);
        else invalid.insert(QStringLiteral("jcartBase"),3);
        check(!probe.host.prepareMovie(invalid,error),
              "invalid J-Cart movie metadata was accepted");
        check(probe.host.movieConfiguration()==configuration &&
              probe.host.serialize()==stateBeforeInvalid,
              "invalid J-Cart metadata changed live session");
    }
    auto legacy=configuration;
    legacy.insert(QStringLiteral("version"),1);
    legacy.remove(QStringLiteral("jcart")); legacy.remove(QStringLiteral("jcartBase"));
    legacy.remove(QStringLiteral("playerCount"));
    QJsonArray three=legacy.value(QStringLiteral("controllers")).toArray();
    while (three.size()>3) three.removeAt(three.size()-1);
    legacy.insert(QStringLiteral("controllers"),three);
    check(probe.host.prepareMovie(legacy,error),qPrintable(error));
    check(probe.host.hasJCart() && !probe.host.controllerConnected(2) &&
          !probe.host.controllerConnected(3),
          "legacy three-slot movie invented cartridge controllers");
    check(probe.host.restoreMovieConfiguration(error),qPrintable(error));
    probe.host.setButtons(2,1u<<4); probe.host.setButtons(3,1u<<7);
    probe.frames();
    const auto saved=probe.host.serialize(); check(!saved.isEmpty(),"J-Cart native save");
    check(probe.host.unserialize(saved),"J-Cart native restore"); probe.frames();
    check(jcartNibble(probe.word(64),0)==14 && jcartNibble(probe.word(64),1)==7,
          "J-Cart routing changed after native restore");
    networkInput(probe.host,4,1u<<4); probe.frames();
    check(jcartNibble(probe.word(64),0)==15 && jcartNibble(probe.word(64),1)==14,
          "netplay role four did not reach J-Cart P4");

    constexpr std::array<unsigned,4> direction{4,5,6,7};
    for (auto adapter : {ControllerAdapter::TeamPlayerPort1,ControllerAdapter::TeamPlayerPort2}) {
        check(probe.host.setControllerAdapter(adapter,error),qPrintable(error));
        check(probe.host.jcartPlayerBase()==5 && probe.host.controllerPlayerCount()==7,
              "J-Cart single Team Player role count");
        for (unsigned player=0;player<7;++player) {
            probe.host.setControllerConnected(player,true);
            probe.host.setControllerSixButton(player,player<5);
            unsigned buttons=1u<<direction[player%4];
            if (player==5) buttons|=1u<<3; // J-Cart Start at TH low
            if (player==6) buttons|=1u<<1; // J-Cart A at TH low
            probe.host.setButtons(player,buttons);
        }
        probe.frames();
        const unsigned port=adapter==ControllerAdapter::TeamPlayerPort1?0:1;
        for (unsigned slot=0;slot<4;++slot)
            check((probe.byte(port*32+8+slot*3)&15)==(15u^(1u<<((slot+port)%4))),
                  "J-Cart + Team Player controller callback routing");
        check(jcartNibble(probe.word(64),0)==(15u^(1u<<(5%4))) &&
              jcartNibble(probe.word(64),1)==(15u^(1u<<(6%4))),
              "J-Cart + Team Player P6/P7 routing");
        const quint16 lowPhase=probe.word(66);
        check((lowPhase&0x40)==0 && (lowPhase&0x20)==0 &&
              (lowPhase&0x4000)==0 && (lowPhase&0x1000)==0,
              "J-Cart + Team Player TH low / Start / A write routing");
        networkInput(probe.host,7,1u<<6); probe.frames();
        check(jcartNibble(probe.word(64),0)==15 && jcartNibble(probe.word(64),1)==11,
              "netplay role seven did not reach J-Cart after Team Player");
    }
    check(probe.host.setControllerAdapter(ControllerAdapter::None,error),"disable Team Player");
    check(probe.host.load(way,error),qPrintable(error));
    check(probe.host.hasJCart(),"J-Cart 4-Way ROM detection");
    check(probe.host.setControllerAdapter(ControllerAdapter::FourWayPlay,error),qPrintable(error));
    check(probe.host.jcartPlayerBase()==4 && probe.host.controllerPlayerCount()==6,
          "J-Cart + 4-Way player base");
    for (unsigned player=0;player<6;++player) {
        probe.host.setControllerConnected(player,true);
        probe.host.setButtons(player,1u<<direction[player%4]);
    }
    probe.frames();
    for (unsigned player=0;player<4;++player)
        check((probe.byte(player)&15)==(15u^(1u<<player)),"J-Cart + 4-Way P1-P4");
    check(jcartNibble(probe.word(64),0)==14 && jcartNibble(probe.word(64),1)==13,
          "J-Cart + 4-Way P5/P6");
}
void mouse(const QString& core) {
    QTemporaryDir temp; check(temp.isValid(),"mouse probe directory");
    const QString path=temp.filePath("original-mouse.md"); writeFile(path,mouseRom());
    Probe probe(core,temp.path()); QString error;
    check(probe.host.load(path,error),qPrintable(error));
    check(probe.host.setControllerKind(0,ControllerType::Mouse) &&
          probe.host.setControllerKind(1,ControllerType::Mouse),"enable two mice");
    probe.host.setControllerConnected(1,true);
    check(probe.host.hardReset(error),qPrintable(error));
    probe.host.setPointer(0,-17,34); probe.host.setPointer(1,63,-5);
    probe.host.setButtons(0,(1u<<1)|(1u<<8)|(1u<<3)); // A+C+Start
    probe.host.setButtons(1,1u); // B
    probe.frames(1); // CPU waits midway through port A's handshake
    check((probe.byte(0)&15)==11 && (probe.byte(4)&15)==13,
          "mouse port A prefix or buttons");
    const auto halfway=probe.host.serialize(); check(!halfway.isEmpty(),"mid-mouse save");
    auto* ram=static_cast<unsigned char*>(probe.memory(RETRO_MEMORY_SYSTEM_RAM));
    check(ram!=nullptr,"mouse RAM flag unavailable"); ram[0x100^1]=1;
    probe.frames(1);
    const auto read=[&](unsigned port,unsigned phase){return probe.byte(port*32+phase)&15;};
    check(read(0,3)==3 && read(0,5)==14 && read(0,6)==15 &&
          read(0,7)==13 && read(0,8)==14,"mouse A signed deltas");
    check(read(1,0)==11 && read(1,3)==0 && read(1,4)==2 &&
          read(1,5)==3 && read(1,6)==15 && read(1,7)==0 && read(1,8)==5,
          "mouse B independent buttons and signed deltas");
    std::array<unsigned,18> finished{};
    for (unsigned port=0;port<2;++port)
        for (unsigned phase=0;phase<9;++phase)
            finished[port*9+phase]=read(port,phase);
    check(probe.host.unserialize(halfway),"mid-mouse native restore");
    ram=static_cast<unsigned char*>(probe.memory(RETRO_MEMORY_SYSTEM_RAM));
    check(ram!=nullptr,"restored mouse RAM flag unavailable"); ram[0x100^1]=1;
    probe.frames(1);
    for (unsigned port=0;port<2;++port)
        for (unsigned phase=0;phase<9;++phase)
            check(read(port,phase)==finished[port*9+phase],
                  "mid-mouse restore changed acquisition protocol");
    probe.host.setControllerConnected(0,false);
    check(probe.host.hardReset(error),qPrintable(error));
    networkInput(probe.host,2,1u<<1,21,-12,1u<<1);
    ram=static_cast<unsigned char*>(probe.memory(RETRO_MEMORY_SYSTEM_RAM));
    check(ram!=nullptr,"empty-port mouse flag unavailable"); ram[0x100^1]=1;
    probe.frames(1);
    check(read(1,0)==11 && read(1,4)==1 && read(1,5)==1 &&
          read(1,6)==5 && read(1,7)==0 && read(1,8)==12,
          "mouse on port B failed with port A disconnected");
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "read cross-OS mouse fixture");
    return file.readAll();
}
void crossOsMouse(const QString& core,const QString& directory,bool write) {
    check(QDir().mkpath(directory),"create cross-OS mouse fixture directory");
    const QDir dir(directory);
    const QString rom=dir.filePath(QStringLiteral("original-mouse.md"));
    const QString statePath=dir.filePath(QStringLiteral("mouse-network-state.bin"));
    const QString expectedPath=dir.filePath(QStringLiteral("mouse-expected-nibbles.bin"));
    writeFile(rom,mouseRom());
    Probe probe(core,directory); QString error;
    check(probe.host.load(rom,error),qPrintable(error));
    check(probe.host.setControllerKind(0,ControllerType::Mouse) &&
          probe.host.setControllerKind(1,ControllerType::Mouse),
          "cross-OS dual mouse setup");
    probe.host.setControllerConnected(1,true);
    check(probe.host.hardReset(error),qPrintable(error));
    probe.host.setPointer(0,-17,34); probe.host.setPointer(1,63,-5);
    probe.host.setButtons(0,(1u<<1)|(1u<<8)|(1u<<3));
    probe.host.setButtons(1,1u);
    probe.frames(1); // Port A waits after acquiring button nibble.
    check((probe.byte(0)&15)==11 && (probe.byte(4)&15)==13,
          "cross-OS mouse did not reach handshake pause");
    if (write) {
        const auto state=probe.host.captureNetworkState();
        check(!state.isEmpty(),"capture mid-handshake network state");
        writeFile(statePath,state);
    } else {
        const auto state=readFile(statePath);
        check(probe.host.restoreNetworkState(state,error),qPrintable(error));
    }
    auto* ram=static_cast<unsigned char*>(probe.memory(RETRO_MEMORY_SYSTEM_RAM));
    check(ram!=nullptr,"cross-OS mouse RAM unavailable");
    ram[0x100^1]=1;
    probe.frames(1);
    QByteArray nibbles;
    for (unsigned port=0;port<2;++port)
        for (unsigned phase=0;phase<9;++phase)
            nibbles.append(char(probe.byte(port*32+phase)&15));
    if (write) writeFile(expectedPath,nibbles);
    else check(nibbles==readFile(expectedPath),
               "cross-OS mouse network restore changed handshake nibbles");
    std::puts(write?"PASS: wrote mid-handshake network mouse fixture":
                    "PASS: cross-OS network mouse restore matched all 18 nibbles");
}
void unsupportedHardware(const QString& core) {
    QTemporaryDir temp; check(temp.isValid(),"unsupported device probe directory");
    const QString gg=temp.filePath("original-game-gear.gg");
    const QString sg=temp.filePath("original-sg1000.sg");
    writeFile(gg,paddleRom()); writeFile(sg,paddleRom());
    Probe probe(core,temp.path()); QString error;
    probe.host.setControllerKind(0,ControllerType::Mouse); // persisted MD setting
    check(probe.host.load(gg,error),qPrintable(error));
    check(probe.host.controllerKind(0)==ControllerType::Pad &&
          probe.host.movieConfiguration().value(QStringLiteral("controllers")).toArray()
              .at(0).toObject().value(QStringLiteral("kind")).toInt()==int(ControllerType::Pad),
          "Game Gear retained unsupported Mouse mapping");
    probe.host.setControllerKind(0,ControllerType::Paddle); // persisted SMS setting
    check(probe.host.load(sg,error),qPrintable(error));
    check(probe.host.controllerKind(0)==ControllerType::Pad &&
          probe.host.movieConfiguration().value(QStringLiteral("controllers")).toArray()
              .at(0).toObject().value(QStringLiteral("kind")).toInt()==int(ControllerType::Pad),
          "SG-1000 retained unsupported Paddle mapping");
}
void paddle(const QString& core) {
    QTemporaryDir temp; check(temp.isValid(),"paddle probe directory");
    const QString path=temp.filePath("original-paddle.sms"); writeFile(path,paddleRom());
    Probe probe(core,temp.path()); QString error;
    check(probe.host.setRegion(1,error),"select Japanese paddle protocol");
    check(probe.host.load(path,error),qPrintable(error));
    check(probe.host.setControllerKind(0,ControllerType::Paddle) &&
          probe.host.setControllerKind(1,ControllerType::Paddle),"enable two paddles");
    probe.host.setControllerConnected(1,true);
    const auto sample=[&](int x0,int x1,bool fire0,bool fire1) {
        check(probe.host.hardReset(error),qPrintable(error));
        probe.host.setPointer(0,x0,0); probe.host.setPointer(1,x1,0);
        probe.host.setButtons(0,fire0?1u:0u); probe.host.setButtons(1,fire1?1u:0u);
        probe.frames(1);
        const auto halfway=probe.host.serialize();
        check(!halfway.isEmpty(),"mid-paddle native save");
        auto* ram=static_cast<unsigned char*>(probe.memory(RETRO_MEMORY_SYSTEM_RAM));
        check(ram!=nullptr,"paddle resume flag unavailable"); ram[0x100]=1;
        probe.frames(1);
        const auto nibbleB=[&](unsigned pair){
            return ((probe.byte(pair*2,false)>>6) |
                    ((probe.byte(pair*2+1,false)&3)<<2)) & 15;
        };
        const unsigned a=(probe.byte(0,false)&15)*16+(probe.byte(2,false)&15);
        const unsigned b=nibbleB(0)*16+nibbleB(1);
        check(bool(probe.byte(0,false)&16)!=fire0 &&
              bool(probe.byte(1,false)&4)!=fire1,"paddle parallel fire button routing");
        check(probe.host.unserialize(halfway),"mid-paddle native restore");
        ram=static_cast<unsigned char*>(probe.memory(RETRO_MEMORY_SYSTEM_RAM));
        check(ram!=nullptr,"restored paddle resume flag unavailable"); ram[0x100]=1;
        probe.frames(1);
        const unsigned restoredA=(probe.byte(0,false)&15)*16+(probe.byte(2,false)&15);
        const unsigned restoredB=nibbleB(0)*16+nibbleB(1);
        check(restoredA==a && restoredB==b,"mid-paddle restore changed nibble phase");
        return std::array<unsigned,2>{a,b};
    };
    check(sample(-32768,32767,true,false)==std::array<unsigned,2>{0,255},
          "paddle A=0 B=255 independent values");
    check(sample(0,-32768,false,true)==std::array<unsigned,2>{128,0},
          "paddle A=128 B=0 independent values");
    check(sample(32767,0,false,false)==std::array<unsigned,2>{255,128},
          "paddle A=255 B=128 independent values");

}
} // namespace
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        check(argc==2 || argc==4,
              "usage: controller_devices_tests CORE [--write DIR | --read DIR]");
        const QString core=QString::fromLocal8Bit(argv[1]);
        if (argc==4) {
            const QString mode=QString::fromLocal8Bit(argv[2]);
            check(mode==QStringLiteral("--write") || mode==QStringLiteral("--read"),
                  "unknown cross-OS mouse fixture mode");
            crossOsMouse(core,QString::fromLocal8Bit(argv[3]),mode==QStringLiteral("--write"));
            return 0;
        }
        jcart(core); mouse(core); paddle(core); unsupportedHardware(core);
        std::puts("PASS: original ROM J-Cart routing, dual mouse and paddle protocols, disconnected slots and native state restore");
        return 0;
    } catch(const std::exception& failure) {
        std::fprintf(stderr,"FAIL: %s\n",failure.what()); return 1;
    }
}
