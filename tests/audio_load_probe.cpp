// Explicit real-ROM/device probe; uses the production session, queue, sink and pacer.
// CORE ROM OUTPUT_JSON FRAMES [GUI_STALL_MS] [BURN_THREADS] [LATENCY_PROFILE] [VOLUME]
// No desktop interaction. Default volume is zero; explicit volume is for an isolated capture sink.
#include "core/EmulationSession.h"
#include "ui/AudioOutput.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMediaDevices>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>

int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    if(argc<5) return 2;
    const int frames=QByteArray(argv[4]).toInt();
    const int stall=argc>5?QByteArray(argv[5]).toInt():0;
    const int burners=argc>6?QByteArray(argv[6]).toInt():0;
    const int profile=argc>7?QByteArray(argv[7]).toInt():0;
    const int volume=argc>8?QByteArray(argv[8]).toInt():0;
    if(frames<60||frames>36000||stall<0||stall>500||burners<0||burners>8||profile<0||profile>2||volume<0||volume>100) return 2;
    if(QMediaDevices::defaultAudioOutput().isNull()) return 77;
    QTemporaryDir temp;
    if(!temp.isValid()) return 2;
    AudioOutput audio;
    audio.setVolume(volume);
    audio.setLatencyProfile(AudioQueue::normalizeProfile(profile));
    EmulationSession session(QString::fromLocal8Bit(argv[1]),temp.path());
    QElapsedTimer elapsed;elapsed.start();
    std::atomic<int> chunks{0}; std::atomic<bool> done{false},stopLoad{false};
    std::atomic<double> loadResult{0};
    double fps=0; int invalidFeedback=0,stallCount=0; bool loaded=false;
    QJsonArray errors;
    QObject::connect(&audio,&AudioOutput::unavailable,&app,[&](const QString& s){errors.append(s);});
    QObject::connect(&session,&EmulationSession::errorOccurred,&app,[&](const QString&s){errors.append(s);done=true;});
    QObject::connect(&session,&EmulationSession::gameLoaded,&app,[&](const QString&,double rate,const QString&,ConsoleProfile){loaded=true;fps=rate;});
    QObject::connect(&session,&EmulationSession::audioSamples,&audio,[&](const QByteArray& pcm,int rate){
        if(done.load())return;
        audio.play(pcm,rate);
        if(++chunks>=frames){done=true;session.setPaused(true);}
    },Qt::DirectConnection);
    QObject::connect(&session,&EmulationSession::audioReset,&audio,[&]{if(!done.load()) audio.clear();},Qt::DirectConnection);
    QObject::connect(&audio,&AudioOutput::clockFeedback,&session,&EmulationSession::updateAudioClock,Qt::DirectConnection);
    QObject::connect(&audio,&AudioOutput::clockFeedback,&app,[&](qint64,qint64,bool valid){if(!valid&&loaded&&!done.load())++invalidFeedback;});
    QObject::connect(&session,&EmulationSession::videoFrameReady,&app,[&]{QImage frame;double aspect;session.takeVideoFrame(frame,aspect);});
    session.configureAchievements(false,false);
    session.setNukedYm2612(true);
    session.setRegion(2); // NTSC-U, same as the controlled core comparison.
    session.setRewindEnabled(true);
    session.setRewindMemoryMiB(128);
    session.loadGame(QString::fromLocal8Bit(argv[2]));
    std::vector<std::thread> load;
    for(int i=0;i<burners;++i) load.emplace_back([&,i]{
        double x=0.1+i;
        while(!stopLoad.load(std::memory_order_relaxed)) {
            for(int n=0;n<10000;++n)x=x*0.999999+0.00001;
            loadResult.store(x,std::memory_order_relaxed);
        }
    });
    QTimer stallTimer;
    QObject::connect(&stallTimer,&QTimer::timeout,&app,[&]{
        if(loaded&&!done.load()&&elapsed.elapsed()>3000){++stallCount;std::this_thread::sleep_for(std::chrono::milliseconds(stall));}
    });
    if(stall)stallTimer.start(500);
    QTimer poll;poll.setInterval(10);
    qint64 endedAt=0;
    QObject::connect(&poll,&QTimer::timeout,&app,[&]{
        if(elapsed.elapsed()>qint64(frames)*1000/45+20000){errors.append("Timeout");done=true;}
        if(!done.load())return;
        if(!endedAt)endedAt=elapsed.elapsed();
        const auto state=audio.diagnosticSnapshot();
        if(state["pending_bytes"].toDouble()>0&&elapsed.elapsed()-endedAt<2000)return;
        auto report=state;
        report["qt_version"]=qVersion();report["loaded"]=loaded;report["nominal_fps"]=fps;
        report["frames"]=chunks.load();report["elapsed_ms"]=double(elapsed.elapsed());
        report["gui_stall_ms"]=stall;report["gui_stalls"]=stallCount;
        report["burn_threads"]=burners;report["latency_profile"]=profile;
        report["invalid_feedback"]=invalidFeedback;report["errors"]=errors;
        report["backend"]=QString::fromLocal8Bit(qgetenv("QT_MEDIA_BACKEND"));
        report["pulse_server"]=QString::fromLocal8Bit(qgetenv("PULSE_SERVER"));
        report["scope"]="Real session with rewind and CPU video rendering, no GPU/UI drawing; QAudioSink (muted unless explicit volume); hashes before system mixer.";
        const auto data=QJsonDocument(report).toJson(QJsonDocument::Indented);
        QFile file(QString::fromLocal8Bit(argv[3]));
        if(!file.open(QIODevice::WriteOnly)||file.write(data)!=data.size()){app.exit(2);return;}
        std::fwrite(data.constData(),1,size_t(data.size()),stdout);
        app.exit(errors.isEmpty()?0:1);
    });
    poll.start();
    const int result=app.exec();
    stopLoad=true;for(auto& t:load)t.join();
    session.shutdown();audio.shutdown();return result;
}
