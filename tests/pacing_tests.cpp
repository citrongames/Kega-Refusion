#include "core/FramePacer.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <initializer_list>
void require(bool ok,const char* reason){if(!ok){std::fprintf(stderr,"FAIL: %s\n",reason);std::exit(1);}}
int main(){
 FramePacer p;const qint64 start=1000000000,period=16683350;p.reset(start);
 for(int i=1;i<=36000;++i){const qint64 ideal=start+i*period;const auto d=p.next(ideal-period+(i%7)*100000,period);require(d==ideal,"scheduler jitter accumulated into clock");}
 const qint64 late=start+40000*period;
 require(p.next(late,period)==late,"long stall causes catchup burst");
 require(p.next(late,period)==late+period,"deadline did not recover");
 for(int i=0;i<1000;++i){auto x=p.audioPeriodScale(late,late,100000,35000,true);require(x>=1 && x<=1.005,"full audio queue correction wrong");}
 for(int i=0;i<1000;++i){auto x=p.audioPeriodScale(late,late,0,35000,true);require(x>=.995 && x<=1.005,"empty audio queue correction unbounded");}
 require(p.audioPeriodScale(late+300000000,late,100000,35000,true)==1,"stale clock feedback changed speed");
 require(p.audioPeriodScale(late,late,100000,35000,false)==1,"missing device changed speed");
 // Simulate a device clock 0.2% faster/slower for ten minutes at PAL/NTSC.
 for(double fps:{50.0,59.9227})for(double drift:{-.002,.002}) {
  FramePacer sim;double queued=35000; qint64 now=start;
  for(int i=0;i<int(fps*600);++i){
   const double scale=sim.audioPeriodScale(now,now,qint64(queued),35000,true);
   queued+=1000000.0/fps-(1000000.0/fps)*scale*(1+drift);
   now+=qint64(1000000000.0/fps*scale);
   require(queued>10000 && queued<60000,"audio-clock drift escaped bounded queue");
  }
  require(std::abs(queued-35000)<5000,"clock drift failed to settle");
 }
 std::puts("Frame pacing: jitter, stall recovery, PAL/NTSC device drift and feedback fallback passed");
}
