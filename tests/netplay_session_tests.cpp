#include "net/NetplayController.h"
#include "net/NetplayTimeline.h"
#include "core/EmulationSession.h"
#include "ui/NetplayLobby.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QSettings>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <QJsonDocument>
#include <QtEndian>
#include <cstdio>
#include <stdexcept>
class NetplayScenario {
public:
    static QByteArray debug(NetplayController& c){return QJsonDocument(QJsonObject{{"status",c.status_},{"manifestBytes",c.manifest_.size()},{"localBytes",c.localManifest_.size()},{"download",c.downloading_},{"allowDownload",c.allowDownloads_},{"path",c.gamePath_},{"ready",c.gameReady_},{"appliedEpoch",QString::number(c.applyingEpoch_)}}).toJson(QJsonDocument::Compact);}
    static void create(NetplayController& c){c.startHost(0,QStringLiteral("127.0.0.1"),{});}
    static void join(NetplayController& c,const QString& invitation){c.startJoin(invitation,{});}
    static void share(NetplayController& c){c.lobby_->allowDownloadsRequested(true);}
    static bool needsDownload(NetplayController& c){return !c.hosting_&&!c.manifest_.isEmpty()&&c.manifest_!=c.localManifest_&&!c.downloading_;}
    static void download(NetplayController& c){c.lobby_->downloadRomRequested();}
    static QString invitation(NetplayController& c){return c.invite_;}
    static quint64 ready(NetplayController& c){return c.readyPeers_.isEmpty()?0:*c.readyPeers_.begin();}
    static int role(NetplayController& c,quint64 id){return c.participants_.value(id).value("role").toString()=="spectator"?0:1;}
    static void assign(NetplayController& c,quint64 id){c.transport_.setRole(id,kega::net::Role::Player1);}
};
static void write(const QString& path,const QByteArray& bytes){QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("fixture write failed");}
static QByteArray read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return f.readAll();}
static QByteArray rom(){QByteArray b(65536,char(0xff));qToBigEndian<quint32>(0xff0000,b.data());qToBigEndian<quint32>(0x200,b.data()+4);b.replace(0x100,16,"SEGA GENESIS    ");b.replace(0x120,16,"NET SESSION     ");b.replace(0x180,2,"GM");qToBigEndian<quint32>(0,b.data()+0x1a0);qToBigEndian<quint32>(65535,b.data()+0x1a4);b[0x1f0]='U';b[0x200]=char(0x60);b[0x201]=char(0xfe);return b;}
int main(int argc,char** argv){
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    const auto args=app.arguments();
    if(args.size()==2){
        QTemporaryDir scratch; if(!scratch.isValid())return 1;
        write(scratch.filePath("host.md"),rom());write(scratch.filePath("renamed.md"),rom());
        auto other=rom();other[0x1000]='X';write(scratch.filePath("other.md"),other);
        QProcess host,client;
        host.start(args[0],{"--host",args[1],scratch.path()});
        if(!host.waitForStarted(5000)){std::fprintf(stderr,"host did not start\n");return 1;}
        QByteArray hostOut;
        for(int i=0;i<20&&!hostOut.contains("READY");++i){host.waitForReadyRead(500);hostOut+=host.readAllStandardOutput();}
        if(!hostOut.contains("READY")){std::fprintf(stderr,"host startup failed: %s\n",host.readAllStandardError().constData());host.kill();host.waitForFinished();return 1;}
        client.start(args[0],{"--client",args[1],scratch.path()});
        const bool hostDone=host.waitForFinished(22000);const bool clientDone=client.waitForFinished(5000);
        const auto hostError=host.readAllStandardError(),clientError=client.readAllStandardError();
        if(!hostDone)host.kill();if(!clientDone)client.kill();
        if(!hostDone||!clientDone||host.exitCode()!=0||client.exitCode()!=0){std::fprintf(stderr,"network session failed\nHOST: %s\nCLIENT: %s\n",hostError.constData(),clientError.constData());return 1;}
        std::puts("netplay session: independent processes, spectator default, shared role OR input, pause, reset, save/load, rewind, game change and explicit download passed");return 0;
    }
    if(args.size()!=4)return 2;
    const bool hosting=args[1]=="--host";const QString root=args[3],data=root+(hosting?"/host-data":"/client-data");QDir().mkpath(data);
    QSettings settings(data+"/settings.ini",QSettings::IniFormat);settings.setValue("netplay/nickname",hosting?"Host":"Client");
    EmulationSession session(args[2],data);
    NetplayController controller(session,data,settings,nullptr);
    QObject::connect(&controller,&NetplayController::openRomRequested,&session,[&](const QString& p){session.loadGame(p);});
    QObject::connect(&session,&EmulationSession::errorOccurred,&app,[&](const QString& e){std::fprintf(stderr,"CORE: %s\n",qPrintable(e));app.exit(1);});
    QObject::connect(&controller,&NetplayController::notice,&app,[&](const QString& e){std::fprintf(stderr,"NOTICE: %s\n",qPrintable(e));});
    int phase=0,combined=0,frames=0,applied=0,resyncs=0;bool pausedSeen=false,resumed=false,downloaded=false;
    QObject::connect(&session,&EmulationSession::networkResyncRequested,&app,[&]{++resyncs;});
    QObject::connect(&session,&EmulationSession::networkSnapshotApplied,&app,[&](bool ok,const QString& e){if(ok)++applied;else{std::fprintf(stderr,"STATE: %s\n",qPrintable(e));app.exit(1);}});
    QObject::connect(&session,&EmulationSession::videoFrameReady,&app,[&]{QImage image;double aspect;session.takeVideoFrame(image,aspect);++frames;});
    QObject::connect(&session,&EmulationSession::networkFrameReady,&app,[&](const QByteArray& b){MovieInput input;if(NetplayTimeline::parseInput(b.mid(16,30),input)&&input.buttons[0]==0x11)++combined;});
    QObject::connect(&session,&EmulationSession::pausedChanged,&app,[&](bool p){
        if(!hosting&&p&&!pausedSeen){pausedSeen=true;QTimer::singleShot(150,&session,[&]{session.setPaused(false);});}
        if(hosting&&phase==2&&!p)resumed=true;
    });
    QTimer poll;poll.setInterval(50);
    QObject::connect(&poll,&QTimer::timeout,&app,[&]{
        if(hosting){
            if(phase>0)session.setButtons(0,0x10);
            const auto peer=NetplayScenario::ready(controller);
            if(phase==0&&peer){
                if(NetplayScenario::role(controller,peer)!=0){std::fprintf(stderr,"joiner not spectator\n");app.exit(1);return;}
                NetplayScenario::assign(controller,peer);session.setButtons(0,0x10);phase=1;
            } else if(phase==1&&combined>=8){phase=2;session.setPaused(true);}
            else if(phase==2&&resumed&&peer){phase=3;session.saveState(0);QTimer::singleShot(150,&session,[&]{session.resetGame();});}
            else if(phase==3&&peer){phase=4;QTimer::singleShot(350,&session,[&]{session.loadState(0);});}
            else if(phase==4&&peer&&combined>=45){phase=5;session.setRewind(true);QTimer::singleShot(200,&session,[&]{session.setRewind(false);});}
            else if(phase==5&&peer&&combined>=60){phase=6;NetplayScenario::share(controller);session.closeGame();QTimer::singleShot(100,&session,[&]{session.loadGame(root+"/other.md");});}
            else if(phase==6&&peer&&combined>=90){write(root+"/done",QByteArray("ok"));std::fprintf(stderr,"host combined=%d\n",combined);app.exit(0);}
        } else {
            session.setButtons(0,1);
            if(pausedSeen&&!downloaded&&NetplayScenario::needsDownload(controller)){downloaded=true;NetplayScenario::download(controller);}
            if(QFile::exists(root+"/done")){
                std::fprintf(stderr,"client frames=%d snapshots=%d resyncs=%d paused=%d\n",frames,applied,resyncs,pausedSeen);
                app.exit(frames>20&&applied>=5&&pausedSeen&&downloaded&&resyncs<=1?0:1);
            }
        }
    });poll.start();
    QTimer::singleShot(18000,&app,[&]{std::fprintf(stderr,"TIMEOUT phase=%d combined=%d applied=%d resyncs=%d\n",phase,combined,applied,resyncs);std::fprintf(stderr,"DETAIL: %s\n",NetplayScenario::debug(controller).constData());app.exit(1);});
    if(hosting){NetplayScenario::create(controller);write(root+"/invite",NetplayScenario::invitation(controller).toUtf8());session.loadGame(root+"/host.md");std::puts("READY");std::fflush(stdout);}
    else {NetplayScenario::join(controller,QString::fromUtf8(read(root+"/invite")));session.loadGame(root+"/renamed.md");}
    const int result=app.exec();controller.leaveSession();session.shutdown();return result;
}
