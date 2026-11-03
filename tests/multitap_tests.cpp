#include "core/LibretroHost.h"
#include <QCoreApplication>
#include <QFile>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>
#include <stdexcept>
namespace {
void check(bool b,const char* msg){if(!b)throw std::runtime_error(msg);}
void save(const QString& path,const QByteArray& b){QFile f(path);check(f.open(QIODevice::WriteOnly)&&f.write(b)==b.size(),"write original fixture");}
QByteArray md(bool way){
 QByteArray b(32768,0);qToBigEndian<quint32>(0xffff00,b.data());qToBigEndian<quint32>(0x200,b.data()+4);
 b.replace(0x100,16,"SEGA MEGA DRIVE ");b.replace(0x1f0,3,"USA");int pc=0x200;
 auto w=[&](quint16 v){b[pc++]=char(v>>8);b[pc++]=char(v);};auto a=[&](quint32 v){w(v>>16);w(v);};
 auto put=[&](unsigned v,unsigned addr){w(0x13fc);w(v);a(addr);};
 auto read=[&](unsigned addr,unsigned offset){w(0x1039);a(addr);w(0x13c0);a(0xff0000+offset);};
 w(0x46fc);w(0x2700);const int loop=pc;
 if(way){
  put(0x40,0xa10009);put(0x7f,0xa1000b);put(0x40,0xa10003);
  for(unsigned i=0;i<4;++i){put(i<<4,0xa10005);read(0xa10003,i);}
 }else{
  for(unsigned port=0;port<2;++port){
   unsigned reg=0xa10003+port*2;put(0x60,0xa10009+port*2);put(0x60,reg);read(reg,port*32);
   for(unsigned n=1;n<20;++n){put(n&1?0x20:0,reg);read(reg,port*32+n);}
  }
 }
 w(0x4ef9);a(loop);return b;
}
QByteArray sms(){
 QByteArray b(32768,0);int pc=0;auto v=[&](unsigned n){b[pc++]=char(n);};
 auto out=[&](unsigned value){v(0x3e);v(value);v(0xd3);v(0x3f);};
 auto read=[&](unsigned port,unsigned addr){v(0xdb);v(port);v(0x32);v(addr);v(addr>>8);};
 v(0xf3);v(0x31);v(0xf0);v(0xdf);int loop=pc;out(0xa5);
 for(unsigned i=0;i<4;++i){read(0xdc,0xc000+i*2);read(0xdd,0xc001+i*2);out(0x05);out(0xa5);}
 v(0xc3);v(loop);v(loop>>8);b.replace(0x7ff0,8,"TMR SEGA");b[0x7fff]=char(0x4c);return b;
}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
 check(argc==2,"core path required");QTemporaryDir data;QString error;auto core=QString::fromLocal8Bit(argv[1]);
 const auto team=data.filePath("team.md"),way=data.filePath("way.md"),tap=data.filePath("master.sms");save(team,md(false));save(way,md(true));save(tap,sms());
 LibretroHost host(core,data.path());QLibrary inspect(core);using Memory=void*(*)(unsigned);
 check(host.load(team,error),qPrintable(error));auto memory=reinterpret_cast<Memory>(inspect.resolve("retro_get_memory_data"));check(memory,"RAM export");
 auto frames=[&]{for(int i=0;i<3;++i)host.runFrame();};
 auto byte=[&](unsigned addr,bool mdMode=true){auto* m=static_cast<unsigned char*>(memory(RETRO_MEMORY_SYSTEM_RAM));check(m,"RAM");return unsigned(m[mdMode?(addr^1):addr]);};
 const unsigned direction[]={4,5,6,7}; // libretro U,D,L,R -> device bit0..3
 for(unsigned i=0;i<8;++i){host.setControllerConnected(i,true);host.setControllerKind(i,ControllerType::Pad);host.setControllerSixButton(i,true);}
 check(host.setControllerAdapter(ControllerAdapter::TeamPlayerBoth,error),qPrintable(error));
 for(unsigned i=0;i<8;++i)host.setButtons(i,1u<<direction[i%4]);frames();
 for(unsigned port=0;port<2;++port)for(unsigned i=0;i<4;++i){
  if((byte(port*32+4+i)&15)!=1){std::fprintf(stderr,"ID port%u player%u RAM:",port,i);for(unsigned k=0;k<20;++k)std::fprintf(stderr," %02x",byte(port*32+k));std::fprintf(stderr,"\n");}
  check((byte(port*32+4+i)&15)==1,"Team Player pad ID");
  if((byte(port*32+8+i*3)&15)!=(15u^(1u<<i)))std::fprintf(stderr,"team port%u player%u got%u expected%u\n",port,i,byte(port*32+8+i*3)&15,15u^(1u<<i));
  check((byte(port*32+8+i*3)&15)==(15u^(1u<<i)),"Team Player P1-P8 routing");
 }
 // A disconnected physical slot must not renumber later logical players.
 host.setControllerConnected(1,false);frames();check((byte(5)&15)==15,"Team Player disconnected ID");
 check((byte(14)&15)==7,"P4 shifted after disconnecting P2");
 host.setControllerSixButton(2,false);frames();check((byte(6)&15)==0,"independent 3-button pad ID");
 const auto state=host.serialize();check(host.unserialize(state),"adapter native restore");frames();check((byte(13)&15)==7,"P4 changed after state restore");
 for(auto adapter : {ControllerAdapter::TeamPlayerPort1,ControllerAdapter::TeamPlayerPort2}) {
  for(unsigned i=0;i<8;++i){host.setControllerConnected(i,true);host.setControllerSixButton(i,true);}
  check(host.setControllerAdapter(adapter,error),"single Team Player");
  for(unsigned i=0;i<5;++i)host.setButtons(i,1u<<direction[i%4]);frames();
  unsigned port=adapter==ControllerAdapter::TeamPlayerPort1?0:1;
  for(unsigned i=0;i<4;++i)check((byte(port*32+8+i*3)&15)==(15u^(1u<<((i+port)%4))),"single Team Player logical routing");
 }
 check(host.setControllerAdapter(ControllerAdapter::None,error),"disable adapter");check(host.controllerPlayerCount()==2,"adapter disable count");
 check(host.load(way,error),qPrintable(error));for(unsigned i=0;i<4;++i)host.setControllerConnected(i,true);
 check(host.setControllerAdapter(ControllerAdapter::FourWayPlay,error),qPrintable(error));
 for(unsigned i=0;i<4;++i)host.setButtons(i,1u<<direction[i]);frames();
 for(unsigned i=0;i<4;++i)check((byte(i)&15)==(15u^(1u<<i)),"4 Way Play physical selection");
 check(host.load(tap,error),qPrintable(error));for(unsigned i=0;i<8;++i)host.setControllerConnected(i,true);
 check(host.setControllerAdapter(ControllerAdapter::MasterTapBoth,error),qPrintable(error));
 for(unsigned i=0;i<8;++i)host.setButtons(i,1u<<direction[i%4]);frames();
 for(unsigned i=0;i<4;++i){
  const unsigned dc=byte(i*2,false),dd=byte(i*2+1,false);
  check((dc&15)==(15u^(1u<<i)),"Master Tap port1 P1-P4");
  check(((dc>>6)|((dd&3)<<2))==(15u^(1u<<i)),"Master Tap port2 P5-P8");
 }
 for(auto adapter : {ControllerAdapter::MasterTapPort1,ControllerAdapter::MasterTapPort2}) {
  check(host.setControllerAdapter(adapter,error),"single Master Tap");
  // Master Tap is a four-state counter without a reset command on the wire.
  // Start the probe CPU and adapter together, not halfway through its scan loop.
  check(host.hardReset(error),"Master Tap probe power reset");
  for(unsigned i=0;i<5;++i)host.setButtons(i,1u<<direction[i%4]);frames();
  for(unsigned i=0;i<4;++i){
   unsigned dc=byte(i*2,false),dd=byte(i*2+1,false);
   if(adapter==ControllerAdapter::MasterTapPort1)check((dc&15)==(15u^(1u<<i)),"single Master Tap port1");
   else check(((dc>>6)|((dd&3)<<2))==(15u^(1u<<((i+1)%4))),"single Master Tap port2");
  }
 }
 check(host.setControllerAdapter(ControllerAdapter::None,error),"disable Master Tap");
 for(unsigned i=0;i<2;++i)host.setControllerKind(i,ControllerType::LightPhaser);
 host.setButtons(0,1u<<1);host.setButtons(1,0);frames();
 check(!(byte(0,false)&0x10) && (byte(1,false)&4),"Light Phaser port1 trigger isolated");
 host.setButtons(0,0);host.setButtons(1,1u<<1);frames();
 check((byte(0,false)&0x10) && !(byte(1,false)&4),"Light Phaser port2 trigger isolated");
 host.setControllerConnected(0,false);frames();
 check(!(byte(1,false)&4),"Light Phaser port2 with empty port1");
 std::puts("PASS: real core Team Player P1-P8, empty slot, mixed pads, state restore; 4 Way Play P1-P4; Master Tap P1-P8; two SMS Light Phaser triggers");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
