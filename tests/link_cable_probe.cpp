#include "core/LibretroHost.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtEndian>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <array>
#include <vector>
struct Action{int first,last,player,button;};
void require(bool b,const QString& why){if(!b)throw std::runtime_error(why.toStdString());}
int peerMappings(){QFile f("/proc/self/maps");if(!f.open(QIODevice::ReadOnly))return -1;return f.readAll().count("_linkpeer");}
qint64 rss(){QFile f("/proc/self/status");if(!f.open(QIODevice::ReadOnly))return -1;for(const auto& line:f.readAll().split('\n'))if(line.startsWith("VmRSS:")){const auto words=line.simplified().split(' ');return words[1].toLongLong();}return -1;}
void hashFrame(QCryptographicHash& hash,const QImage& frame){hash.addData(QByteArrayView(reinterpret_cast<const char*>(frame.constBits()),frame.sizeInBytes()));}
int main(int argc,char**argv){QCoreApplication app(argc,argv);try{
 require(argc>=5,"usage CORE ROM OUT FRAMES [INPUT] [single]");const QString out=argv[3];QDir().mkpath(out);QTemporaryDir temp;QString error;
 const bool single=argc>6&&QByteArray(argv[6])=="single";LibretroHost host(argv[1],temp.path());host.setExpansionDevices(ExpansionMode::Auto, ExpansionMode::Auto);
 const auto before=rss();require(host.setLinkCableEnabled(!single,error),error);require(host.load(argv[2],error),error);
 std::vector<Action> actions;
 if(argc>5){QFile f(argv[5]);require(f.open(QIODevice::ReadOnly),f.errorString());QTextStream stream(&f);while(!stream.atEnd()){const auto row=stream.readLine().simplified().split(' ');if(row.size()==4)actions.push_back({row[0].toInt(),row[1].toInt(),row[2].toInt(),row[3].toInt()});}}
 const int frames=QByteArray(argv[4]).toInt();const auto start=std::chrono::steady_clock::now();qint64 audioNonzero=0;
 for(int n=0;n<frames;++n){unsigned pad[2]{};for(const auto&a:actions)if(n>=a.first&&n<=a.last)pad[a.player]|=1u<<a.button;host.setButtons(0,pad[0]);host.setButtons(1,pad[1]);host.runFrame();for(char byte:host.audio())audioNonzero+=byte!=0;
 if(n==400||n==900||n==1640||n==1880||n==2160||n==3900||n==frames-1){for(int r=1;r<=2;++r)host.frameForLinkRole(r).save(out+QString("/frame-%1-%2.png").arg(n).arg(r));}
 }
 const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
 QJsonObject result{{"frames",frames},{"seconds",seconds},{"emulated_fps",frames/seconds},{"video_rate",host.fps()},{"rss_before_kib",before},{"rss_linked_kib",rss()},{"peer_mappings",peerMappings()},{"audio_nonzero_bytes",audioNonzero}};
 if(!single){
 const auto native=host.serialize();
 const bool gameGear=host.isGameGear();
 require(native.startsWith(gameGear?"KRGGLK01":"KRLINK01"),"pair was not created");
 result["pair_state_magic"]=QString::fromLatin1(native.left(8));
 result["video_width"]=host.frameForLinkRole(1).width();result["video_height"]=host.frameForLinkRole(1).height();
 auto corrupt=native;const auto aSize=qFromBigEndian<quint32>(native.constData()+8);corrupt[48+aSize]='!';
 require(!host.unserialize(corrupt),"corrupt second console state accepted");
 require(host.serialize()==native,"failed pair restore changed the live session");
 result["failed_restore_atomic"]=true;
 if(gameGear){auto brokenWire=native;brokenWire[brokenWire.size()-1]=char(0xff);
 require(!host.unserialize(brokenWire),"corrupt GG wire state accepted");
 require(host.serialize()==native,"failed GG wire restore changed the live session");result["failed_wire_restore_atomic"]=true;}
 const auto state=host.captureMovieState();require(!state.isEmpty(),"empty pair state");
 auto replay=[&]{QCryptographicHash video(QCryptographicHash::Sha256),audio(QCryptographicHash::Sha256);for(int n=0;n<180;++n){host.setButtons(0,n>=10&&n<70?1u<<4:0);host.setButtons(1,n>=10&&n<40?1u<<7:0);host.runFrame();hashFrame(video,host.frameForLinkRole(1));hashFrame(video,host.frameForLinkRole(2));audio.addData(host.audio());}return std::array<QByteArray,2>{{video.result().toHex(),audio.result().toHex()}};};
 const auto first=replay();for(int r=1;r<=2;++r)host.frameForLinkRole(r).save(out+QString("/replay-%1.png").arg(r));require(host.restoreMovieState(state,error),error);const auto second=replay();
 result["state_bytes"]=state.size();result["video_replay_equal"]=first[0]==second[0];result["audio_replay_equal"]=first[1]==second[1];result["video_hash"]=QString(first[0]);result["audio_hash"]=QString(first[1]);
 require(host.setLinkCableEnabled(false,error),error);result["peer_mappings_after_disable"]=peerMappings();result["rss_after_disable_kib"]=rss();
 QJsonArray cycles;
 for(int i=0;i<8;++i){require(host.setLinkCableEnabled(true,error),error);host.runFrame();require(host.setLinkCableEnabled(false,error),error);cycles.append(QJsonObject{{"rss_kib",rss()},{"peer_mappings",peerMappings()}});}
 result["repeated_toggle_cleanup"]=cycles;host.unload();result["peer_mappings_after_unload"]=peerMappings();result["rss_after_unload_kib"]=rss();
 }
 QFile file(out+"/result.json");require(file.open(QIODevice::WriteOnly),file.errorString());file.write(QJsonDocument(result).toJson());std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
