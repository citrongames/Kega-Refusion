#include "core/LibretroHost.h"
#include "core/RewindBuffer.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <cstdio>
#include <vector>
int main(int argc,char** argv) {
 QCoreApplication app(argc,argv);
 if(argc!=3){std::fprintf(stderr,"Usage: speed_benchmark core ROM\n");return 2;}
 std::puts("blargg,history,mode,emulated_frames,seconds,frames_per_second,speed_x,rendered_frames,history_MiB");
 for(int filter:{0,2})for(int history:{0,1}){
  std::vector<QImage> reference;
  for(bool optimized:{false,true}){
   QTemporaryDir data;LibretroHost host(QString::fromLocal8Bit(argv[1]),data.path());QString error;
   if(!host.setBlarggFilter(filter,error)||!host.load(QString::fromLocal8Bit(argv[2]),error)){std::fprintf(stderr,"%s\n",qPrintable(error));return 1;}
   RewindBuffer rewind;rewind.configure(history != 0);
   // Warm up outside the timed section; identical input and emulated frame count.
   for(int i=0;i<180;++i)host.runFrame();
   QElapsedTimer clock;clock.start();qint64 nextPresent=0;int rendered=0;
   constexpr int count=1800;
   for(int i=0;i<count;++i){
    host.setButtons(0,(i<4?1U<<RETRO_DEVICE_ID_JOYPAD_START:0)|(i>180 && (i/180)%2?1U<<RETRO_DEVICE_ID_JOYPAD_RIGHT:0));
    const bool checkpoint=!optimized && history && i%2==1;
    const bool present=clock.nsecsElapsed()>=nextPresent || i%60==59;
    const bool render=!optimized||checkpoint||present;
    host.runFrame(render,!optimized);rendered+=render;
    if(optimized && !host.audio().isEmpty()){std::fprintf(stderr,"Muted fast-forward copied PCM\n");return 5;}
    if(present)nextPresent=clock.nsecsElapsed()+16666667;
    if(checkpoint && !rewind.capture(host.captureRewindState(),host.frame(),host.aspectRatio()))return 3;
    // Samples are forced rendered in both modes: check actual future pixels.
    if(i%60==59){
     if(!optimized)reference.push_back(host.frame());
     else if(host.frame()!=reference[size_t(i/60)]){std::fprintf(stderr,"Skipped rendering changed future image at%d\n",i);return 4;}
    }
   }
   const double seconds=clock.nsecsElapsed()/1e9;
   host.runFrame();
   if(host.audio().isEmpty()){std::fprintf(stderr,"Audio failed to resume after fast-forward\n");return 6;}
   if(!optimized)reference.push_back(host.frame());
   else if(host.frame()!=reference.back()){std::fprintf(stderr,"Normal frame after fast-forward differs\n");return 7;}
   std::printf("%d,%d,%s,%d,%.4f,%.1f,%.2f,%d,%.3f\n",filter,history,optimized?"optimized":"full-output",count,seconds,count/seconds,count/seconds/host.fps(),rendered,rewind.bytesUsed()/1048576.0);
   std::fflush(stdout);
  }
 }
}
