#include "core/LibretroHost.h"
#include "core/RewindBuffer.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QElapsedTimer>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <vector>
struct Legacy {
 std::deque<QByteArray> states,pixels;qsizetype bytes=0;size_t cap=1801;
 void capture(const QByteArray& state,const QImage& frame) {
  const QImage rgb=frame.convertToFormat(QImage::Format_RGB32);
  auto s=qCompress(state,1),p=qCompress(rgb.constBits(),rgb.sizeInBytes(),1);
  while(!states.empty() && (states.size()>=cap || bytes+s.size()+p.size()>128*1024*1024)){
   bytes-=states.front().size()+pixels.front().size();states.pop_front();pixels.pop_front();
  }
  bytes+=s.size()+p.size();states.push_back(std::move(s));pixels.push_back(std::move(p));
 }
};
double percentile(std::vector<qint64> v,double p) {std::sort(v.begin(),v.end());return v.empty()?0:v[size_t((v.size()-1)*p)]/1000.0;}
int main(int argc,char** argv) {
 QCoreApplication app(argc,argv);
 if(argc<3){std::fprintf(stderr,"Usage: rewind_benchmark core ROM [ROM...]\n");return 2;}
 std::puts("game,fps,frames,legacy_MiB,delta_MiB,legacy_seconds,delta_seconds,legacy_capture_mean_us,delta_capture_mean_us,legacy_p99_us,delta_p99_us,restore_p99_us");
 for(int a=2;a<argc;++a){
  QTemporaryDir data;LibretroHost host(QString::fromLocal8Bit(argv[1]),data.path());QString error;
  if(!host.load(QString::fromLocal8Bit(argv[a]),error)){std::fprintf(stderr,"%s\n",qPrintable(error));return 1;}
  RewindBuffer delta;Legacy legacy;delta.configure(true);legacy.cap=size_t(60*host.fps()/2)+1;
  std::vector<qint64> oldTimes,newTimes,restoreTimes;QElapsedTimer timer;
  const int frames=int(host.fps()*65);
  for(int i=0;i<frames;++i){
   // Identical per-frame input schedule for both histories: enter menus and hold right/A intermittently.
   host.setButtons(0,(i>=180 && i<184?1U<<RETRO_DEVICE_ID_JOYPAD_START:0) |
      (i>=600 && (i/180)%2?1U<<RETRO_DEVICE_ID_JOYPAD_RIGHT:0) |
      (i>=600 && i%90<8?1U<<RETRO_DEVICE_ID_JOYPAD_Y:0));
   host.runFrame();if(i%2)continue;
   const auto state=host.captureRewindState();
   timer.start();legacy.capture(state,host.frame());oldTimes.push_back(timer.nsecsElapsed());
   timer.restart();if(!delta.capture(state,host.frame(),host.aspectRatio()))return 3;newTimes.push_back(timer.nsecsElapsed());
  }
  const auto deltaBytes=delta.bytesUsed();const auto deltaSize=delta.size();
  QByteArray restored;QImage image;double aspect;
  for(int i=0;i<300;++i){timer.start();if(!delta.stepBack(restored,image,aspect))break;restoreTimes.push_back(timer.nsecsElapsed());if(!host.restoreRewindState(restored))return 4;}
  double oldSum=0,newSum=0;for(auto t:oldTimes)oldSum+=t/1000.0;for(auto t:newTimes)newSum+=t/1000.0;
  std::printf("%s,%.3f,%d,%.3f,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",qPrintable(QFileInfo(QString::fromLocal8Bit(argv[a])).fileName()),host.fps(),frames,
    legacy.bytes/1048576.0,deltaBytes/1048576.0,(legacy.states.size()-1)*2/host.fps(),(deltaSize-1)*2/host.fps(),
    oldSum/oldTimes.size(),newSum/newTimes.size(),percentile(oldTimes,.99),percentile(newTimes,.99),percentile(restoreTimes,.99));
 }
}
