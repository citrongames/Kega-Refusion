#include "core/RewindBuffer.h"
#include <QCoreApplication>
#include <QRandomGenerator>
#include <cstdio>
#include <cstdlib>
#include <vector>
void require(bool ok, const char* reason) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",reason); std::exit(1); } }
struct Shot { QByteArray state; QImage frame; double aspect; };
int main(int argc,char** argv) {
 QCoreApplication app(argc,argv);
 QRandomGenerator random(314159U);
 std::vector<Shot> shots;
 RewindBuffer buffer; buffer.configure(true);
 QByteArray state(8192,'\0'); QImage frame(64,48,QImage::Format_RGB32); frame.fill(Qt::black);
 for(int i=0;i<160;++i) {
   if(i==50) { state.resize(9001); frame=QImage(65,49,QImage::Format_RGB32);frame.fill(Qt::red); }
   if(i==100) { state.resize(8192);frame=QImage(64,48,QImage::Format_RGB32);frame.fill(Qt::blue); }
   for(int n=0;n<(i%9==0?state.size():10);++n)state[random.bounded(state.size())]=char(random.generate());
   frame.setPixel(random.bounded(frame.width()),random.bounded(frame.height()),random.generate()|0xff000000U);
   shots.push_back({state,frame, i<80?4.0/3.0:1.5});
   require(buffer.capture(state,frame,shots.back().aspect),"capture failed");
 }
 require(buffer.size()==shots.size(),"history count");
 for(int i=int(shots.size())-2;i>=0;--i) {
   QByteArray actual;QImage image;double aspect;
   require(buffer.stepBack(actual,image,aspect),"reverse step failed");
   require(actual==shots[i].state && image==shots[i].frame && aspect==shots[i].aspect,"reverse snapshot mismatch");
 }
 QByteArray actual;QImage image;double aspect;
 require(!buffer.stepBack(actual,image,aspect),"rewound past oldest");
 require(buffer.capture(shots[5].state,shots[5].frame,shots[5].aspect),"branch capture");
 require(buffer.stepBack(actual,image,aspect) && actual==shots[0].state,"future branch leaked");
 RewindBuffer bounded(45000);bounded.configure(true);
 for(const auto& shot:shots) { require(bounded.capture(shot.state,shot.frame,shot.aspect),"bounded capture");require(bounded.bytesUsed()<=45000,"memory cap exceeded"); }
 while(bounded.stepBack(actual,image,aspect))require(bounded.bytesUsed()<=45000,"reverse memory cap exceeded");
 // Reserved capacity is retained memory even when logical data is small.
 QByteArray reserved(16,'r');reserved.reserve(128*1024);
 RewindBuffer reservedBudget(64*1024);reservedBudget.configure(true);
 require(!reservedBudget.capture(reserved,shots[0].frame,1.0),"reserved anchor bypassed memory budget");
 QByteArray large(2*1024*1024,'q');const auto compressed=qCompress(large,1);
 RewindBuffer compressedBudget(4*1024*1024);compressedBudget.configure(true);
 QImage tiny(1,1,QImage::Format_RGB32);tiny.fill(Qt::black);
 require(compressedBudget.capture(large,tiny,1.0),"large capacity fixture capture");
 QByteArray small(1,'s');
 require(compressedBudget.capture(small,tiny,1.0),"size-changing capacity fixture capture");
 require(compressedBudget.bytesUsed()>=compressed.capacity()+small.capacity()+tiny.sizeInBytes(),
         "compressed patch capacity was omitted from memory accounting");
 require(compressedBudget.stepBack(actual,image,aspect) && actual==large,"capacity-aware history restore");
 require(compressedBudget.bytesUsed()>=large.capacity()+tiny.sizeInBytes(),"restored anchor capacity was omitted");
 RewindBuffer identical;identical.configure(true);
 for(int i=0;i<2001;++i)require(identical.capture(shots[0].state,shots[0].frame,shots[0].aspect),"identical capture");
 require(identical.size()==2001,"history stopped at a duration cap");
 require(identical.bytesUsed()<512*1024,"unchanged frames repeated full storage");
 identical.configure(false);require(identical.capture(shots[0].state,shots[0].frame,1.0) && identical.size()==0 && identical.bytesUsed()==0,"disabled history retained bytes");
 buffer.configure(true);for(int i=0;i<10;++i)buffer.capture(shots[i].state,shots[i].frame,1.0);
 require(buffer.size()==10,"history was limited by checkpoint count");
 require(!buffer.capture({},shots[0].frame,1.0),"empty state accepted");
 require(!buffer.capture(shots[0].state,{},1.0),"empty image accepted");
 std::puts("Reverse deltas: sparse/dense changes, dimensions, branches, budgets and disabled history passed");
}
