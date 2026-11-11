#include "core/LibretroHost.h"
#include "core/RewindBuffer.h"
#include "net/NetRollbackSession.h"
#include "net/NetplayTimeline.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
void checked(bool value,const QString& error) {if(!value)throw std::runtime_error(error.toStdString());}
QByteArray pixels(const QImage& image) {
    const auto rgb=image.convertToFormat(QImage::Format_RGB32);
    return QByteArray(reinterpret_cast<const char*>(rgb.constBits()),rgb.sizeInBytes());
}
void reply(const QJsonObject& value) {
    const auto line=QJsonDocument(value).toJson(QJsonDocument::Compact)+'\n';
    std::fwrite(line.constData(),1,size_t(line.size()),stdout);std::fflush(stdout);
}
QByteArray fixture(){
    QByteArray b(65536,char(0xff));qToBigEndian<quint32>(0xff0000,b.data());qToBigEndian<quint32>(0x200,b.data()+4);
    b.replace(0x100,16,"SEGA GENESIS    ");b.replace(0x120,16,"NET SESSION     ");b.replace(0x180,2,"GM");
    qToBigEndian<quint32>(0,b.data()+0x1a0);qToBigEndian<quint32>(65535,b.data()+0x1a4);b[0x1f0]='U';
    int pc=0x200;
    const auto word=[&](quint16 value){qToBigEndian(value,b.data()+pc);pc+=2;};
    const auto immediate=[&](quint16 opcode,quint16 value,quint32 address){word(opcode);word(value);word(quint16(address>>16));word(quint16(address));};
    const auto branch=[&](int at,int target){const int displacement=target-at-2;if(!displacement||displacement<-128||displacement>127)throw std::runtime_error("fixture branch");b[at+1]=char(displacement);};
    word(0x46fc);word(0x2700); // Mask interrupts.
    immediate(0x33fc,0x8004,0xc00004);immediate(0x33fc,0x8144,0xc00004);immediate(0x33fc,0x8700,0xc00004);
    immediate(0x13fc,0x40,0xa10009);immediate(0x13fc,0x40,0xa10003); // P1 TH output, high.
    for(quint16 value:{0x80,0x10,0xbf,0xdf,0xff})immediate(0x13fc,value,0xc00011);
    const int wait=pc;
    word(0x3239);word(0x00c0);word(4);word(0x0241);word(8);
    const int waitBranch=pc;word(0x6700);
    word(0x7000);word(0x1039);word(0x00a1);word(3); // Read P1 pad.
    word(0x0800);word(0);const int silentBranch=pc;word(0x6600);
    immediate(0x13fc,0x90,0xc00011);const int volumeDoneBranch=pc;word(0x6000);
    const int silent=pc;immediate(0x13fc,0x9f,0xc00011);const int volumeDone=pc;
    word(0x0240);word(0x11);word(0xe348); // Up/B electrical bits select CRAM green/red.
    word(0x23fc);word(0xc000);word(0);word(0x00c0);word(4);
    word(0x33c0);word(0x00c0);word(0);
    const int endWait=pc;word(0x3239);word(0x00c0);word(4);word(0x0241);word(8);
    const int endBranch=pc;word(0x6600);const int repeat=pc;word(0x6000);
    branch(waitBranch,wait);branch(silentBranch,silent);branch(volumeDoneBranch,volumeDone);branch(endBranch,endWait);branch(repeat,wait);
    return b;
}

MovieInput localInput(int id,quint64 frame) {
    MovieInput input; input.aim.fill(quint64(1)<<32);
    if(id==0 && ((frame/7)%3)!=1) input.buttons[0]=0x10; // Up / green and tone.
    if(id==1 && ((frame/5)%4)==1) input.buttons[0]=1; // B / red and silence.
    return input;
}
MovieInput expectedInput(quint64 frame,bool shared) {
    return NetplayTimeline::merge({{1,localInput(0,frame)},{shared?1:2,localInput(1,frame)}});
}
struct Baseline {
    std::array<QImage,2> image;
    InputDisplayState input;
};
QImage combine(const NetRollbackSession::RewindFrame& frame) {
    if(frame.secondImage.isNull())return frame.image;
    QImage image(std::max(frame.image.width(),frame.secondImage.width()),
                 frame.image.height()+frame.secondImage.height(),QImage::Format_RGB32);
    image.fill(Qt::black);
    for(int y=0;y<frame.image.height();++y)
        std::memcpy(image.scanLine(y),frame.image.constScanLine(y),size_t(frame.image.width())*4);
    for(int y=0;y<frame.secondImage.height();++y)
        std::memcpy(image.scanLine(frame.image.height()+y),frame.secondImage.constScanLine(y),size_t(frame.secondImage.width())*4);
    return image;
}
QJsonObject verify(LibretroHost& host,NetRollbackSession& net,const QByteArray& initial,
                   const std::vector<NetRollbackSession::RewindFrame>& checkpoints,bool shared,bool link) {
    check(checkpoints.size()>120,"insufficient contiguous confirmed rewind history");
    net.stop();
    QString error;
    std::vector<Baseline> baseline(checkpoints.back().position+2);
    checked(host.restoreMovieState(initial,error,true),error);
    host.setLinkPresentationRole(1);
    for(size_t position=1;position<baseline.size();++position) {
        NetplayTimeline::apply(host,expectedInput(position-1,shared));host.runFrame(true,false);
        auto& expected=baseline[position];expected.input=host.inputDisplay();
        expected.image[0]=host.frameForLinkRole(1);
        if(link)expected.image[1]=host.frameForLinkRole(2);
    }
    RewindBuffer history(64*1024*1024),screenshots(64*1024*1024);
    history.configure(true);screenshots.configure(true);
    int regenerated=0;
    for(size_t i=0;i<checkpoints.size();++i) {
        const auto& frame=checkpoints[i];
        const auto position=frame.position;
        check(position==i+1,"confirmed history gap or duplicate");
        check(frame.controls==expectedInput(position-1,shared),"history retained speculative/shared-role input");
        check(frame.input==baseline[position].input,"confirmed input display differs from final merged input");
        checked(host.restoreMovieState(frame.state,error,true),error);
        check(host.runClock().frames==position,"confirmed state clock differs from checkpoint position");
        if(frame.replay) {
            check(i>0 && frame.image.isNull() && frame.secondImage.isNull(),"replay entry retained pixels");
            checked(host.restoreMovieState(checkpoints[i-1].state,error,true),error);
            NetplayTimeline::apply(host,frame.controls);host.runRewindPreviewFrame();
            check(host.runClock().frames==position,"preview advanced an incorrect number of frames");
            for(int role=1;role<=(link?2:1);++role)
                check(pixels(host.frameForLinkRole(role))==pixels(baseline[position].image[size_t(role-1)]),
                      "regenerated confirmed picture differs from original output");
            check(host.audio().isEmpty(),"preview retained audio for playback");
            ++regenerated;
            check(history.captureForReplay(frame.state,frame.aspect,frame.controls,&frame.input),"replay capture failed");
        } else check(history.capture(frame.state,combine(frame),frame.aspect,&frame.input),"boundary capture failed");
        auto exact=frame;exact.image=baseline[position].image[0];exact.secondImage=link?baseline[position].image[1]:QImage();
        // The old network path kept every second post-frame screenshot.
        if(position%2==0)check(screenshots.capture(frame.state,combine(exact),frame.aspect,&frame.input),"screenshot capture failed");
        // Compare against a direct restore of the SAME native checkpoint.
        // Existing native CPU/raster fixups can differ from an uninterrupted
        // run; the preview path must not add another difference on resume.
        for(int role=1;role<=(link?2:1);++role) {
            checked(host.restoreMovieState(frame.state,error,true),error);
            host.setLinkPresentationRole(role);
            NetplayTimeline::apply(host,expectedInput(position,shared));host.runFrame(true,true);
            const auto controlState=host.serialize(),controlPcm=host.audio();
            const auto controlImage=host.frameForLinkRole(role);
            if(frame.replay) {
                checked(host.restoreMovieState(checkpoints[i-1].state,error,true),error);
                NetplayTimeline::apply(host,frame.controls);host.runRewindPreviewFrame();
            }
            checked(host.restoreMovieState(frame.state,error,true),error);
            host.setLinkPresentationRole(role);
            NetplayTimeline::apply(host,expectedInput(position,shared));host.runFrame(true,true);
            if(host.serialize()!=controlState) {
                const auto actual=host.serialize();int differences=0;
                for(qsizetype offset=0;offset<std::min(actual.size(),controlState.size());++offset)
                    if(actual[offset]!=controlState[offset] && ++differences<=24)
                        std::fprintf(stderr,"paired diff offset=%lld actual=%02x expected=%02x\n",static_cast<long long>(offset),quint8(actual[offset]),quint8(controlState[offset]));
                std::fprintf(stderr,"canonical resume mismatch position=%llu role=%d link=%d shared=%d differences=%d size=%lld\n",static_cast<unsigned long long>(position),role,link,shared,differences,static_cast<long long>(actual.size()));
                throw std::runtime_error("preview changed next-frame core state after canonical restore");
            }
            check(pixels(host.frameForLinkRole(role))==pixels(controlImage),"preview changed next-frame picture after canonical restore");
            check(host.audio()==controlPcm,"preview changed next-frame PCM after canonical restore");
        }
    }
    check(regenerated>100,"confirmed history did not use compact regeneration");
    const auto compactBytes=history.bytesUsed(),screenshotBytes=screenshots.bytesUsed();
    size_t position=checkpoints.back().position;
    while(history.size()>1) {
        QByteArray state;QImage image;double aspect=0;InputDisplayState input;RewindBuffer::ReplayFrame replay;
        check(history.stepBack(state,image,aspect,1,&input,&replay),"history step failed");--position;
        check(state==checkpoints[position-1].state && input==baseline[position].input,"rewind landed on wrong confirmed frame/input");
        if(!replay.previousState.isEmpty()) {
            checked(host.restoreMovieState(replay.previousState,error,true),error);
            NetplayTimeline::apply(host,replay.controls);host.runRewindPreviewFrame();
            check(host.runClock().frames==position,"buffer replay predecessor/input incorrect");
        }
        checked(host.restoreMovieState(state,error,true),error);
    }
    check(position==1,"oldest confirmed frame lost");
    return {{"verified",int(checkpoints.size())},{"regenerated",regenerated},
            {"compactBytes",double(compactBytes)},{"oldHalfRateScreenshotBytes",double(screenshotBytes)}};
}
int worker(const QString& core,const QString& rom,const QString& data,int id,bool shared,bool link) {
    LibretroHost host(core,data);QString error;
    checked(host.load(rom,error),error);host.setControllerConnected(1,true);
    if(link)checked(host.setLinkCableEnabled(true,error),error);
    const auto initial=host.captureMovieState(false);
    NetRollbackSession net;QJsonArray outgoing;
    const auto send=[&](quint64 peer,const QByteArray& packet) {
        check(outgoing.size()<256,"outgoing queue overflow");
        outgoing.append(QJsonObject{{"to",QString::number(peer)},{"data",QString::fromLatin1(packet.toBase64())}});
    };
    const QJsonArray roster{QJsonObject{{"id","0"},{"role",1}},QJsonObject{{"id","1"},{"role",shared?1:2}}};
    checked(net.start(host,1,roster,quint64(id),send,error),error);net.setHistoryEnabled(true);
    std::vector<NetRollbackSession::RewindFrame> checkpoints;int wrongPredictions=0;reply({{"ready",true}});
    std::string line;
    while(std::getline(std::cin,line)) {
        const auto command=QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        if(command.value("verify").toBool()) {
            if(id==0) {
                check(wrongPredictions>0,"test did not execute incorrect speculative input");
                auto result=verify(host,net,initial,checkpoints,shared,link);
                result.insert("wrongPredictions",wrongPredictions);reply(result);
            } else reply({{"verified",true}});
            return 0;
        }
        for(const auto& packet:command.value("packets").toArray()) {
            const auto item=packet.toObject();net.receive(item.value("from").toString().toULongLong(),QByteArray::fromBase64(item.value("data").toString().toLatin1()));
        }
        checked(net.idle(error),error);MovieInput merged;
        const auto decision=net.beforeFrame(localInput(id,net.position()),merged,error);
        check(decision!=NetRollbackSession::Failed,qPrintable(error));
        if(decision==NetRollbackSession::Run) {
            if(merged!=expectedInput(net.position(),shared))++wrongPredictions;
            NetplayTimeline::apply(host,merged);host.runFrame(true,false);checked(net.afterFrame(error),error);
        }
        NetRollbackSession::RewindFrame frame;
        while(net.takeConfirmedHistory(frame)) {
            check(id==0,"non-host exported rewind history");check(checkpoints.size()<512,"rewind test queue overflow");
            checkpoints.push_back(frame);
        }
        reply({{"out",outgoing}});outgoing={};
    }
    return 0;
}
QJsonObject readReply(QProcess& process) {
    QElapsedTimer timer;timer.start();
    while(!process.canReadLine()) {
        check(process.state()==QProcess::Running,"worker exited early");check(timer.elapsed()<60000,"worker reply timeout");
        process.waitForReadyRead(100);
    }
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(process.readLine(),&error);
    check(error.error==QJsonParseError::NoError && doc.isObject(),"invalid worker reply");return doc.object();
}
struct Packet {int to,from,due;QByteArray data;};
void run(const QString& executable,const QString& core,bool shared,bool link) {
    QTemporaryDir temp;check(temp.isValid(),"temporary directory failed");
    const auto rom=temp.filePath("probe.md");QFile file(rom);const auto contents=fixture();
    check(file.open(QIODevice::WriteOnly)&&file.write(contents)==contents.size(),"ROM fixture write failed");file.close();
    std::array<QProcess,2> workers;
    struct Cleanup {std::array<QProcess,2>& workers;~Cleanup(){for(auto& worker:workers)if(worker.state()!=QProcess::NotRunning){worker.closeWriteChannel();if(!worker.waitForFinished(2000)){worker.kill();worker.waitForFinished();}}}} cleanup{workers};
    for(int id=0;id<2;++id) {
        const auto copy=temp.filePath(QStringLiteral("core%1.so").arg(id));
        check(QFile::copy(core,copy),"isolated core copy failed");
        if(link)check(QFile::copy(core,temp.filePath(QStringLiteral("core%1_linkpeer.so").arg(id))),"link peer core copy failed");
        workers[id].setProcessChannelMode(QProcess::ForwardedErrorChannel);
        workers[id].start(executable,{"--worker",copy,rom,temp.filePath(QStringLiteral("data%1").arg(id)),QString::number(id),shared?"shared":"split",link?"link":"single"});
        check(workers[id].waitForStarted(10000),"worker start failed");check(readReply(workers[id]).value("ready").toBool(),"worker not ready");
    }
    std::deque<Packet> delayed;
    for(int tick=0;tick<230;++tick) {
        std::array<QJsonArray,2> inbox;
        for(auto it=delayed.begin();it!=delayed.end();) {
            if(it->due>tick){++it;continue;}
            inbox[it->to].append(QJsonObject{{"from",QString::number(it->from)},{"data",QString::fromLatin1(it->data.toBase64())}});it=delayed.erase(it);
        }
        for(int id=0;id<2;++id) {
            const auto command=QJsonDocument(QJsonObject{{"packets",inbox[id]}}).toJson(QJsonDocument::Compact)+'\n';
            check(workers[id].write(command)==command.size(),"worker command failed");
        }
        for(int id=0;id<2;++id)for(const auto& value:readReply(workers[id]).value("out").toArray()) {
            const auto packet=value.toObject();const int to=packet.value("to").toString().toInt();
            check(to==1-id && delayed.size()<256,"relay queue overflow");
            delayed.push_back({to,id,tick+3+tick%2,QByteArray::fromBase64(packet.value("data").toString().toLatin1())});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    check(workers[0].write("{\"verify\":true}\n")>0,"verify command failed");
    const auto result=readReply(workers[0]);check(result.value("verified").toInt()>120,"verification failed");
    std::printf("netplay rewind %s/%s: %s\n",shared?"shared":"split",link?"link":"single",QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        if(argc==8 && std::strcmp(argv[1],"--worker")==0)
            return worker(QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3]),QString::fromLocal8Bit(argv[4]),std::atoi(argv[5]),std::strcmp(argv[6],"shared")==0,std::strcmp(argv[7],"link")==0);
        check(argc==2 || (argc==3 && std::strcmp(argv[2],"--link-only")==0),"usage: netplay_rewind_tests <core> [--link-only]");
        const auto core=QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
        if(argc==2) {
            run(QCoreApplication::applicationFilePath(),core,false,false);
            run(QCoreApplication::applicationFilePath(),core,true,false);
        }
        run(QCoreApplication::applicationFilePath(),core,false,true);
        std::puts("netplay_rewind_tests PASS");return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"netplay_rewind_tests FAIL: %s\n",e.what());return 1;}
}
