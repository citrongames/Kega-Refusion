#include "core/EmulationSession.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <atomic>
#include <cstdio>
#include <functional>
#include <stdexcept>
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void waitFor(const std::function<bool()>& predicate){
 QElapsedTimer t;t.start();while(!predicate() && t.elapsed()<4000){QCoreApplication::processEvents();QThread::msleep(2);}require(predicate(),"session timeout");
}
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);
 try {
 const bool fastOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--fast-forward");
 const bool checksumOnly=argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--checksum");
 require(argc==2 || fastOnly || checksumOnly,"core argument required");QTemporaryDir temp;require(temp.isValid(),"temporary directory");
 QByteArray rom(32768,'\0');qToBigEndian<quint32>(0xffff00,rom.data());qToBigEndian<quint32>(0x200,rom.data()+4);
 rom.replace(0x100,16,QByteArray("SEGA MEGA DRIVE ",16));rom.replace(0x1f0,3,"USA");rom[0x200]=char(0x60);rom[0x201]=char(0xfe);
 const auto path=temp.filePath("original.md");QFile file(path);require(file.open(QIODevice::WriteOnly) && file.write(rom)==rom.size(),"ROM fixture");file.close();
 EmulationSession session(QString::fromLocal8Bit(argv[1]),temp.path());
 std::atomic<int> frames{0};int ready=0,errors=0,actions=0;bool paused=false;
 QObject::connect(&session,&EmulationSession::videoFrame,&app,[&](const QImage&,double){++frames;},Qt::DirectConnection);
 QObject::connect(&session,&EmulationSession::videoFrameReady,&app,[&]{++ready;});
 QObject::connect(&session,&EmulationSession::errorOccurred,&app,[&](const QString& s){++errors;std::fprintf(stderr,"%s\n",qPrintable(s));});
 QObject::connect(&session,&EmulationSession::actionCompleted,&app,[&](const QString&){++actions;});
 QObject::connect(&session,&EmulationSession::effectivePauseChanged,&app,[&](bool p){paused=p;});
 session.loadGame(path);waitFor([&]{return frames.load()>=5;});
 if(checksumOnly){
  session.setPaused(true);waitFor([&]{return paused;});
  std::atomic<int> audioResets{0};
  QObject::connect(&session,&EmulationSession::audioReset,&app,[&]{++audioResets;},Qt::DirectConnection);
  session.saveState(0);waitFor([&]{return actions==1;});
  const QDir states(temp.path()+"/states");
  const auto files=states.entryList({"*.slot0.krs"},QDir::Files);
  require(files.size()==1,"checksum test state missing");
  auto readState=[&]{QFile state(states.filePath(files.first()));require(state.open(QIODevice::ReadOnly),"state read failed");return state.readAll();};
  const QByteArray original=readState();const int frozen=frames;
  for(bool enabled:{true,false}){
   const int completed=actions;
   session.setAutoFixChecksum(enabled);session.saveState(0);
   waitFor([&]{return actions>=completed+1;});
   require(errors==0 && audioResets==0 && frames==frozen && paused,
           "checksum toggle disturbed the running session");
   require(readState()==original,"checksum toggle changed the game state");
  }
  session.setAutoFixChecksum(true);const int completed=actions;
  session.hardResetGame();waitFor([&]{return actions>=completed+1;});
  require(audioResets>0 && errors==0,"explicit hard reset no longer works");
  session.shutdown();
  std::puts("Checksum toggles preserve exact paused state; explicit hard reset passed");
  return 0;
 }
 if(fastOnly){
  std::atomic<bool> reversed{false};
  QObject::connect(&session,&EmulationSession::rewindingChanged,&app,[&](bool value){reversed=value;},Qt::DirectConnection);
  session.setFastForwardFactor(0);session.setFastForward(true);
  const int initial=frames;waitFor([&]{return frames>=initial+8;});
  session.setPaused(true);waitFor([&]{return paused;});
  session.setFastForward(false);session.setRewind(true);
  const int boundary=frames;QThread::msleep(100);QCoreApplication::processEvents();
  require(frames==boundary && !reversed,"rewind crossed fast-forward gap or recorded accelerated frames");
  session.setRewind(false);session.setPaused(false);waitFor([&]{return !paused && frames>=boundary+8;});
  session.setPaused(true);waitFor([&]{return paused;});
  session.setRewind(true);waitFor([&]{return reversed.load();});
  require(frames>boundary+8,"rewind did not resume after fast-forward");
  session.setRewind(false);session.shutdown();require(errors==0,"fast-forward session error");
  std::puts("Unlimited fast-forward, no history across gap, normal rewind recording resumed: passed");
  return 0;
 }
 const int before=frames;QThread::msleep(180);QCoreApplication::processEvents();
 require(frames>before+5,"GUI stall stopped emulation worker");require(ready==1,"video notifications accumulated without consumer");
 QImage image;double aspect=0;require(session.takeVideoFrame(image,aspect)&&!image.isNull(),"latest frame unavailable");
 require(!session.takeVideoFrame(image,aspect),"same delivery consumed twice");waitFor([&]{return ready==2;});
 session.setPaused(true);waitFor([&]{return paused;});const int frozen=frames;QThread::msleep(80);require(frames==frozen,"manual pause runs frames");
 session.setBackgroundPaused(true);session.setPaused(false);QThread::msleep(80);require(frames==frozen,"background pause lost to manual resume");
 session.setBackgroundPaused(false);waitFor([&]{return !paused && frames>frozen;});
 for(int region:{2,3}){
  session.setRegion(region);QElapsedTimer settle;settle.start();while(settle.elapsed()<150){QCoreApplication::processEvents();QThread::msleep(2);}
  const int begin=frames;QElapsedTimer timer;timer.start();
  while(timer.elapsed()<2000){session.requestSlotStatus(0);QCoreApplication::processEvents();QThread::msleep(4);}
  const double measured=(frames.load()-begin)*1000.0/timer.elapsed();
  require(measured>(region==2?57:47) && measured<(region==2?63:53),"slot queries perturb frame cadence");
  std::printf("Session region %d cadence %.2f fps under slot queries\n",region,measured);
 }
 session.resetGame();waitFor([&]{return actions==1;});session.hardResetGame();waitFor([&]{return actions==2;});
 require(errors==0,"session reported error");session.shutdown();
 std::puts("Session: coalesced latest frame, GUI stall isolation, pause reasons, PAL/NTSC cadence and resets passed");
 return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
