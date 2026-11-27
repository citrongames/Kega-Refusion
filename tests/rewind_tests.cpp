#include "core/RewindBuffer.h"
#include <QCoreApplication>
#include <QRandomGenerator>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
void require(bool ok, const char* reason) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",reason); std::exit(1); } }
struct Shot { QByteArray state; QImage frame; double aspect; };

void checkAuxiliary(const std::vector<Shot>& shots, const QImage& tiny) {
    RewindBuffer history;
    history.configure(true);
    const QByteArray binary("\xff\0Lua", 5);
    const QByteArray longer("different length");
    const std::vector<QByteArray> auxiliary{{}, {}, binary, binary, longer, {}, {}, binary};
    for (size_t i = 0; i < auxiliary.size(); ++i) {
        require(history.capture(shots[i].state, shots[i].frame, 1.0, nullptr, auxiliary[i]),
                "capture with independent auxiliary data");
    }

    QByteArray actual, restored;
    QImage image;
    double aspect;
    require(history.stepBack(actual, image, aspect, 1) && actual == shots[6].state,
            "rewind updates auxiliary anchor without output pointer");
    require(history.stepBack(actual, image, aspect, 2, nullptr, nullptr, &restored) &&
            actual == shots[4].state && restored == longer, "variable-length auxiliary rewind");
    require(history.capture(shots[9].state, shots[9].frame, 1.0, nullptr, binary),
            "capture auxiliary on new branch");
    require(history.stepBack(actual, image, aspect, 1, nullptr, nullptr, &restored) &&
            actual == shots[4].state && restored == longer, "auxiliary branch preserves timeline");
    require(history.stepBack(actual, image, aspect, 2, nullptr, nullptr, &restored) &&
            actual == shots[2].state && restored == binary, "binary auxiliary data was altered");
    require(history.stepBack(actual, image, aspect, 20, nullptr, nullptr, &restored) &&
            actual == shots[0].state && restored.isEmpty(), "pre-declaration auxiliary checkpoint");
    restored = binary;
    require(!history.stepBack(actual, image, aspect, 1, nullptr, nullptr, &restored) && restored.isEmpty(),
            "failed rewind clears auxiliary output");

    history.clear();
    require(history.capture(shots[0].state, shots[0].frame, 1.0, nullptr, binary), "replay seed with auxiliary data");
    MovieInput controls;
    for (size_t i = 1; i < auxiliary.size(); ++i) {
        controls.buttons[0] = quint16(i);
        require(history.captureForReplay(shots[i].state, 1.0, controls, nullptr, true, auxiliary[i]),
                "woven replay capture with auxiliary data");
    }
    RewindBuffer::ReplayFrame replay;
    require(history.stepBack(actual, image, aspect, 3, nullptr, &replay, &restored) &&
            actual == shots[4].state && restored == longer && replay.previousState == shots[2].state &&
            replay.firstFieldControls && replay.firstFieldControls->buttons[0] == 3 &&
            replay.controls.buttons[0] == 4, "replay prerequisites use native state only");

    RewindBuffer bounded(6000);
    bounded.configure(true);
    QByteArray native(1024, 'n');
    std::vector<QByteArray> many;
    for (int i = 0; i < 100; ++i) {
        many.emplace_back(i % 10 == 0 ? 0 : 900 + i * 4, char(i));
        native[0] = char(i);
        require(bounded.capture(native, tiny, 1.0, nullptr, many.back()), "bounded auxiliary capture");
        require(bounded.bytesUsed() <= 6000, "auxiliary capture exceeded shared budget");
    }
    const auto retained = bounded.size();
    require(retained > 2 && retained < many.size(), "auxiliary budget should evict old checkpoints");
    for (size_t i = 1; i < retained; ++i) {
        require(bounded.stepBack(actual, image, aspect, 1, nullptr, nullptr, &restored) &&
                restored == many[many.size() - 1 - i] && actual.at(0) == char(many.size() - 1 - i),
                "budget eviction misaligned sparse auxiliary deltas");
        require(bounded.bytesUsed() <= 6000, "auxiliary rewind exceeded shared budget");
    }
    bounded.setByteLimit(0);
    require(bounded.size() == 0 && bounded.bytesUsed() == 0, "zero budget released auxiliary history");

    const QByteArray maximum(256 * 1024, 'm');
    RewindBuffer plain, unchanged;
    plain.configure(true);
    unchanged.configure(true);
    for (int i = 0; i < 200; ++i) {
        native[0] = char(i);
        require(plain.capture(native, tiny, 1.0) &&
                unchanged.capture(native, tiny, 1.0, nullptr, maximum), "unchanged auxiliary capture");
    }
    require(unchanged.bytesUsed() == plain.bytesUsed() + maximum.capacity(),
            "unchanged snapshots retain only one auxiliary value");
    const QByteArray oversized(maximum.size() + 1, 'x');
    require(!unchanged.capture(native, tiny, 1.0, nullptr, oversized) &&
            !unchanged.captureForReplay(native, 1.0, controls, nullptr, false, oversized) &&
            unchanged.size() == plain.size(), "oversized auxiliary data bypassed the hard limit or damaged history");
    require(unchanged.capture(native, tiny, 1.0) &&
            unchanged.stepBack(actual, image, aspect, 20, nullptr, nullptr, &restored) &&
            restored == maximum, "restore the maximum auxiliary payload from an empty anchor");

    QByteArray reserved(1, 'r');
    reserved.reserve(8 * 1024);
    RewindBuffer capacityLimited(4 * 1024);
    capacityLimited.configure(true);
    require(!capacityLimited.capture(native, tiny, 1.0, nullptr, reserved) &&
            !capacityLimited.bytesUsed(), "reserved auxiliary capacity escaped accounting");
}

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
 buffer.clear();
 for(const auto& shot:shots)require(buffer.capture(shot.state,shot.frame,shot.aspect),"batch fixture capture");
 for(int pos=int(shots.size())-1;pos>0;) {
   pos=std::max(0,pos-16);
   require(buffer.stepBack(actual,image,aspect,16),"accelerated step failed");
   require(actual==shots[pos].state && image==shots[pos].frame && aspect==shots[pos].aspect,"wrong accelerated landing frame");
 }
 require(!buffer.stepBack(actual,image,aspect,16),"accelerated rewind passed oldest frame");
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
 require(compressedBudget.bytesUsed()>=compressed.size()+small.capacity()+tiny.sizeInBytes(),
         "compressed patch was omitted from memory accounting");
 require(compressedBudget.bytesUsed()<compressed.capacity(),
         "compressed patch retained qCompress allocation slack");
 require(compressedBudget.stepBack(actual,image,aspect) && actual==large,"capacity-aware history restore");
 require(compressedBudget.bytesUsed()>=large.capacity()+tiny.sizeInBytes(),"restored anchor capacity was omitted");
 RewindBuffer identical;identical.configure(true);
 for(int i=0;i<2001;++i)require(identical.capture(shots[0].state,shots[0].frame,shots[0].aspect),"identical capture");
 require(identical.size()==2001,"history stopped at a duration cap");
 require(identical.bytesUsed()<512*1024,"unchanged frames repeated full storage");
 InputDisplayState held;held.players[0].device=InputDisplayDevice::MegaDrive6;
 held.players[0].buttons=0x31;
 RewindBuffer heldHistory;heldHistory.configure(true);
 for(int i=0;i<2001;++i)require(heldHistory.capture(shots[0].state,shots[0].frame,shots[0].aspect,&held),"held input capture");
 require(heldHistory.size()==identical.size(),"input history changed frame count");
 require(heldHistory.bytesUsed()<=identical.bytesUsed()+4*qsizetype(sizeof(InputDisplayState)),
         "identical input was copied per frame");
 InputDisplayState restored;
 require(heldHistory.stepBack(actual,image,aspect,1000,&restored) && restored==held,"held input rewind");
 identical.configure(false);require(identical.capture(shots[0].state,shots[0].frame,1.0) && identical.size()==0 && identical.bytesUsed()==0,"disabled history retained bytes");
 restored=held;
 require(!identical.stepBack(actual,image,aspect,1,&restored) && restored==InputDisplayState{},
         "unavailable input was not neutral");
 RewindBuffer inputHistory;inputHistory.configure(true);
 InputDisplayState aim=held;aim.players[0].device=InputDisplayDevice::LightPhaser;
 aim.players[0].x=123;aim.players[0].y=-27;aim.players[0].offscreen=true;
 require(inputHistory.capture(shots[0].state,shots[0].frame,1.0),"untracked input capture");
 require(inputHistory.capture(shots[1].state,shots[1].frame,1.0,&held),"first tracked input capture");
 require(inputHistory.capture(shots[2].state,shots[2].frame,1.0,&held),"repeated input capture");
 require(inputHistory.capture(shots[3].state,shots[3].frame,1.0),"missing input capture");
 require(inputHistory.capture(shots[4].state,shots[4].frame,1.0,&aim),"changed input capture");
 require(inputHistory.stepBack(actual,image,aspect,1,&restored) && actual==shots[3].state &&
         restored==InputDisplayState{},"missing input did not rewind to neutral");
 require(inputHistory.stepBack(actual,image,aspect,1,&restored) && actual==shots[2].state &&
         restored==held,"held input was not preserved");
 require(inputHistory.stepBack(actual,image,aspect,2,&restored) && actual==shots[0].state &&
         restored==InputDisplayState{},"pre-tracking input was not neutral");
 require(inputHistory.capture(shots[5].state,shots[5].frame,1.0,&aim),"tracked branch capture");
 require(inputHistory.stepBack(actual,image,aspect,1,&restored) && actual==shots[0].state &&
         restored==InputDisplayState{},"future input leaked into branched history");
 RewindBuffer boundedInput(4000);boundedInput.configure(true);
 for(int i=0;i<100;++i) {
   InputDisplayState snapshot=held;snapshot.players[0].buttons=quint16(i+1);
   require(boundedInput.capture(small,tiny,1.0,&snapshot),"bounded input capture");
   require(boundedInput.bytesUsed()<=4000,"bounded input memory cap exceeded");
 }
 const size_t retained=boundedInput.size();
 require(retained>1 && retained<100,"input history was not budgeted");
 for(size_t i=1;i<retained;++i) {
   require(boundedInput.stepBack(actual,image,aspect,1,&restored),"bounded input rewind");
   require(restored.players[0].buttons==quint16(100-i),"bounded input landed on wrong frame");
   require(boundedInput.bytesUsed()<=4000,"bounded input reverse memory cap exceeded");
 }
 buffer.clear();for(int i=0;i<10;++i)buffer.capture(shots[i].state,shots[i].frame,1.0);
 require(buffer.size()==10,"history was limited by checkpoint count");
 const auto used=buffer.bytesUsed();
 buffer.configure(true);
 buffer.setByteLimit(used*2);
 require(buffer.size()==10,"increasing budget or reapplying enabled state discarded history");
 buffer.setByteLimit(used-1);
 require(buffer.size()==9 && buffer.bytesUsed()<used,"smaller budget did not evict only oldest checkpoint");
 require(buffer.stepBack(actual,image,aspect,20) && actual==shots[1].state,"resize lost retained timeline");
 buffer.setByteLimit(0);
 require(!buffer.size() && !buffer.bytesUsed(),"zero budget retained the anchor");
 require(!buffer.capture({},shots[0].frame,1.0),"empty state accepted");
 require(!buffer.capture(shots[0].state,{},1.0),"empty image accepted");
 // Mix a seed picture with replay-only checkpoints, then evict the seed.
 RewindBuffer replayBuffer;
 replayBuffer.configure(true);
 require(replayBuffer.capture(shots[0].state,shots[0].frame,1.0),"seed for replay");
 MovieInput controls;
 for(int i=1;i<=12;++i) {
   controls.buttons[1]=quint16(i);
   require(replayBuffer.captureForReplay(shots[i].state,1.0,controls),"append replay state");
 }
 RewindBuffer::ReplayFrame replay;
 require(replayBuffer.stepBack(actual,image,aspect,3,nullptr,&replay) &&
         actual==shots[9].state && replay.previousState==shots[8].state &&
         replay.controls.buttons[1]==9 && image.isNull(),"wrong replay landing state or input");
 replayBuffer.setByteLimit(replayBuffer.bytesUsed()-1);
 require(replayBuffer.size()==8,"evicted predecessor counted as a rewind step");
 require(replayBuffer.stepBack(actual,image,aspect,20,nullptr,&replay) &&
         actual==shots[2].state && replay.previousState==shots[1].state &&
         replay.controls.buttons[1]==2,"oldest replay frame lost predecessor or input");
 require(!replayBuffer.stepBack(actual,image,aspect,1,nullptr,&replay),"rewound to unavailable predecessor");
 replayBuffer.setByteLimit(128*1024);
 require(replayBuffer.capture(shots[7].state,shots[7].frame,1.5),"capture picture on branched timeline");
 require(replayBuffer.captureForReplay(shots[8].state,1.0,controls),"return to replay after picture");
 require(replayBuffer.stepBack(actual,image,aspect,1,nullptr,&replay) &&
         image==shots[7].frame && replay.previousState.isEmpty() && aspect==1.5,"stored branch picture lost");
 require(replayBuffer.stepBack(actual,image,aspect,1,nullptr,&replay) &&
         actual==shots[2].state && replay.previousState==shots[1].state,"branch corrupted replay");
 // A woven target uses exactly its two recorded inputs, even on a multi-step jump.
 replayBuffer.clear();
 require(replayBuffer.capture(shots[0].state,shots[0].frame,1.0),"woven seed");
 for(int i=1;i<8;++i) {
   controls.buttons[0]=quint16(i);
   require(replayBuffer.captureForReplay(shots[i].state,1.0,controls,nullptr,true),"woven capture");
 }
 require(replayBuffer.stepBack(actual,image,aspect,3,nullptr,&replay) && actual==shots[4].state &&
         replay.previousState==shots[2].state && replay.firstFieldControls &&
         replay.firstFieldControls->buttons[0]==3 && replay.controls.buttons[0]==4 &&
         !replay.singleFieldFallback && image.isNull(),"wrong pair of fields");
 replayBuffer.setByteLimit(replayBuffer.bytesUsed()-1);
 require(replayBuffer.stepBack(actual,image,aspect,20,nullptr,&replay) && actual==shots[2].state &&
         replay.previousState==shots[1].state && !replay.firstFieldControls &&
         replay.singleFieldFallback && image.isNull(),"missing oldest-field fallback");
 checkAuxiliary(shots,tiny);
 std::puts("Reverse deltas: sparse/dense changes, dimensions, branches, budgets, input and auxiliary history passed");
}
