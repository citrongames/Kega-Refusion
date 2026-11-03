#include "core/EmulationSession.h"
#include "core/LibretroHost.h"
#include "core/MovieSession.h"
#include "core/VideoRecorder.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <utility>
namespace {
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
void waitFor(const std::function<bool()>& done) {
    QElapsedTimer time;time.start();
    while(!done() && time.elapsed()<20000) {QCoreApplication::processEvents();QThread::msleep(1);}
    require(done(),"video operation timed out");
}
QByteArray run(const QString& command,const QStringList& args) {
    QProcess process;process.start(command,args);
    require(process.waitForStarted() && process.waitForFinished(20000),"media verifier unavailable");
    const auto errors=process.readAllStandardError();
    if(process.exitCode()!=0) std::fprintf(stderr,"%s",errors.constData());
    require(process.exitStatus()==QProcess::NormalExit && process.exitCode()==0,"media verification failed");
    return process.readAllStandardOutput();
}
QByteArray pixels(const QImage& image) {
    const auto rgb=image.convertToFormat(QImage::Format_RGB888);
    QByteArray result;result.reserve(rgb.width()*rgb.height()*3);
    for(int y=0;y<rgb.height();++y)result.append(reinterpret_cast<const char*>(rgb.constScanLine(y)),rgb.width()*3);
    return result;
}
QJsonObject probe(const QString& path) {
    return QJsonDocument::fromJson(run("ffprobe",{"-v","error","-show_streams","-show_format","-of","json",path})).object();
}
QByteArray decode(const QString& path) {
    return run("ffmpeg",{"-v","error","-i",path,"-map","0:v:0","-fps_mode","passthrough","-f","rawvideo","-pix_fmt","rgb24","pipe:1"});
}
QByteArray decodeRecording(const QString& path) {
    QByteArray frames=decode(path);
    const QFileInfo info(path);
    const auto parts=info.dir().entryList({info.completeBaseName()+".part*.mov"},QDir::Files,QDir::Name);
    for(const auto& name:parts)frames+=decode(info.dir().filePath(name));
    return frames;
}
void checkAudio(const QString& path) {
    const auto pcm=run("ffmpeg",{"-v","error","-i",path,"-map","0:a:0","-f","s16le","pipe:1"});
    require(pcm.size()>1000,"audio missing");
    require(pcm.count(char(0))<pcm.size()*0.99,"sound was replaced by silence");
}
void putLong(QByteArray& data, int at, quint32 value) { qToBigEndian(value, data.data()+at); }

// Original 68000 program: sample port A every VBlank, store its value in
// cartridge SRAM and system RAM, and use it as the next backdrop colour.
QByteArray makeInputRom() {
    QByteArray rom(64 * 1024, char(0xff));
    putLong(rom,0,0x00ff0000); putLong(rom,4,0x00000200);
    rom.replace(0x100,16,"SEGA GENESIS    ");
    rom.replace(0x120,16,"MOVIE INPUT TEST");
    rom.replace(0x150,16,"MOVIE INPUT TEST");
    rom.replace(0x180,2,"GM");
    putLong(rom,0x1a0,0); putLong(rom,0x1a4,quint32(rom.size()-1));
    putLong(rom,0x1a8,0x00ff0000); putLong(rom,0x1ac,0x00ffffff);
    rom.replace(0x1f0,1,"U");
    rom.replace(0x1b0,2,"RA"); rom[0x1b2]=char(0xf8); rom[0x1b3]=char(0x20);
    putLong(rom,0x1b4,0x00200000); putLong(rom,0x1b8,0x0020ffff);
    int pc=0x200;
    const auto word=[&](quint16 v) { rom[pc++]=char(v>>8); rom[pc++]=char(v); };
    // VDP display and backdrop, then port A TH high.
    word(0x33fc);word(0x8004);word(0x00c0);word(0x0004);
    word(0x33fc);word(0x8144);word(0x00c0);word(0x0004);
    word(0x33fc);word(0x8700);word(0x00c0);word(0x0004);
    word(0x13fc);word(0x0040);word(0x00a1);word(0x0009);
    word(0x13fc);word(0x0040);word(0x00a1);word(0x0003);
    for (quint16 value : {0x84,0x10,0x90}) {
        word(0x13fc); word(value); word(0x00c0); word(0x0011);
    }
    word(0x7000);
    const int waitOn=pc;
    word(0x3239);word(0x00c0);word(0x0004);
    word(0x0241);word(0x0008);
    const int branchOn=pc;word(0x6700);
    word(0x23fc);word(0xc000);word(0x0000);word(0x00c0);word(0x0004);
    word(0x33c0);word(0x00c0);word(0x0000);
    // MOVE.B $A10003,D0; copy it into RAM and SRAM for input assertions.
    word(0x1039);word(0x00a1);word(0x0003);
    word(0x33c0);word(0x00ff);word(0x0000);
    word(0x13c0);word(0x0020);word(0x0000);
    const int waitOff=pc;
    word(0x3239);word(0x00c0);word(0x0004);
    word(0x0241);word(0x0008);
    const int branchOff=pc;word(0x6600);
    const int branchAgain=pc;word(0x6000);
    for (const auto [from,to] : {std::pair{branchOn,waitOn},
                                 std::pair{branchOff,waitOff},
                                 std::pair{branchAgain,waitOn}}) {
        const int displacement=to-(from+2);
        require(displacement >= -128 && displacement <= 127 && displacement != 0,
                "fixture branch out of range");
        rom[from+1]=char(displacement & 0xff);
    }
    return rom;
}

void writerRoundTrip(const QString& dir) {
    VideoRecorder recorder;QString error;
    const QString path=dir+"/цветной тест.mov";
    require(recorder.start(path,error),qPrintable(error));
    QByteArray expected;
    for(int n=0;n<65;++n) {
        QImage image(64,48,QImage::Format_RGB32);
        for(int y=0;y<48;++y)for(int x=0;x<64;++x)
            image.setPixel(x,y,qRgb((x*9+n)%256,(y*5+n*3)%256,(x*y+n*7)%256));
        expected+=pixels(image);
        QByteArray pcm(800*4,Qt::Uninitialized);
        for(int i=0;i<800;++i) {
            const auto sample=qint16(12000*std::sin((n*800+i)*0.0576));
            qToLittleEndian(sample,pcm.data()+i*4);qToLittleEndian(sample,pcm.data()+i*4+2);
        }
        waitFor([&]{return recorder.capacityAvailable() || recorder.completed();});
        require(recorder.submit(image,pcm,48000,60),"cannot submit native frame");
    }
    recorder.finish();waitFor([&]{return recorder.completed();});
    const auto result=recorder.result();require(result.error.isEmpty(),qPrintable(result.error));
    require(result.frames==65 && result.paths.size()==1,"wrong encoded frame count");
    require(decode(path)==expected,"video pixels changed during lossless encoding");
    checkAudio(path);
    const auto streams=probe(path).value("streams").toArray();
    require(streams.size()==2 && streams[0].toObject().value("codec_name")=="png" &&
            streams[1].toObject().value("codec_name")=="aac","wrong video/audio codecs");
    require(!recorder.start(path,error),"existing output file must not be overwritten");
    require(decode(path)==expected,"existing output was damaged");
    std::printf("MOV: exact RGB pixels, AAC sound, Unicode path and existing-file protection passed\n");
}
void sessionExport(const QString& core,const QString& dir) {
    const QString romPath=dir+"/fixture.md",moviePath=dir+"/fixture.krm",videoPath=dir+"/export.mov";
    QFile rom(romPath);const auto bytes=makeInputRom();
    require(rom.open(QIODevice::WriteOnly)&&rom.write(bytes)==bytes.size(),"fixture write failed");rom.close();
    QString error;QByteArray expected;double rate=0;
    {
        LibretroHost host(core,dir);require(host.load(romPath,error),qPrintable(error));
        require(host.setRegion(3,error),qPrintable(error)); // PAL verifies non-60-Hz timestamps.
        MovieSession movie(host,dir);require(movie.startRecording(moviePath,error),qPrintable(error));
        rate=movie.frameRate();require(rate>49 && rate<51,"PAL movie frequency invalid");
        for(int i=0;i<90;++i) {
            MovieInput input;input.buttons[0]=(i%2)?1<<4:1<<5;
            require(movie.beforeFrame(input,error),qPrintable(error));
            host.runFrame();movie.afterFrame();expected+=pixels(host.frame());
        }
        require(movie.finish(true,error),qPrintable(error));
    }
    EmulationSession session(core,dir);std::atomic_bool loaded{false};bool finished=false,active=false;int movieMode=0;
    quint64 done=0,total=0;QString failure;std::atomic_int frames{0};
    QObject::connect(&session,&EmulationSession::gameLoaded,&session,[&]{loaded=true;},Qt::DirectConnection);
    QObject::connect(&session,&EmulationSession::videoFrame,&session,[&](const QImage&,double){++frames;},Qt::DirectConnection);
    QObject::connect(&session,&EmulationSession::videoRecordingChanged,QCoreApplication::instance(),[&](bool a,bool,const QString&){active=a;});
    QObject::connect(&session,&EmulationSession::videoRecordingFinished,QCoreApplication::instance(),[&](const QString&,const QString& e){failure=e;finished=true;});
    QObject::connect(&session,&EmulationSession::videoExportProgress,QCoreApplication::instance(),[&](quint64 a,quint64 b){done=a;total=b;});
    QObject::connect(&session,&EmulationSession::movieStatusChanged,QCoreApplication::instance(),[&](int mode,const QString&,quint64,quint64,double){movieMode=mode;});
    session.loadGame(romPath);waitFor([&]{return loaded && frames>=2;});
    session.setBackgroundPaused(true);session.setBlarggFilter(2);
    session.exportMovieVideo(moviePath,videoPath);waitFor([&]{return finished;});
    require(failure.isEmpty(),qPrintable(failure));require(!active&&movieMode==3&&done==90&&total==90,"export did not finish/stop on last input");
    const auto actual=decodeRecording(videoPath);
    if(actual!=expected) {
        QFile dump(dir+"/expected.rgb");dump.open(QIODevice::WriteOnly);dump.write(expected);
        std::fprintf(stderr,"native expected %lld bytes, export %lld bytes; artifacts %s\n",(long long)expected.size(),(long long)actual.size(),qPrintable(dir));
    }
    require(actual==expected,"movie export differs from native unfiltered playback");
    checkAudio(videoPath);
    const auto video=probe(dir+"/export.part002.mov").value("streams").toArray()[0].toObject();
    const auto fractions=video.value("avg_frame_rate").toString().split('/');
    require(fractions.size()==2 && std::abs(fractions[0].toDouble()/fractions[1].toDouble()-rate)<0.001,"export ignored recorded PAL cadence");
    session.stopMovie();waitFor([&]{return movieMode==0;});
    finished=false;session.setBackgroundPaused(false);session.setPaused(true);
    session.startVideoRecording(dir+"/live.mov");waitFor([&]{return active||finished;});require(active,qPrintable(failure));
    for(int i=0;i<3;++i){const int previous=frames;session.advanceFrame();waitFor([&]{return frames>previous;});}
    session.stopVideoRecording();waitFor([&]{return finished;});require(failure.isEmpty(),qPrintable(failure));
    require(probe(dir+"/live.mov").value("streams").toArray().size()==2,"live video lacks sound");
    checkAudio(dir+"/live.mov");
    // Cancel after the first accepted export frame, keeping the playable partial MOV.
    finished=false;done=0;session.exportMovieVideo(moviePath,dir+"/cancel.mov");
    waitFor([&]{return done>0||finished;});session.stopVideoRecording();waitFor([&]{return finished;});
    require(failure.isEmpty(),qPrintable(failure));require(!decode(dir+"/cancel.mov").isEmpty(),"cancelled export lost accepted frames");
    session.shutdown();
    std::printf("Real core: PAL movie pixels/sound, Blargg bypass, background export, live stepping and cancel passed\n");
}
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    try {require(argc==2,"core path required");QTemporaryDir dir;require(dir.isValid(),"temporary directory failed");
        writerRoundTrip(dir.path());sessionExport(QString::fromLocal8Bit(argv[1]),dir.path());return 0;
    } catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
