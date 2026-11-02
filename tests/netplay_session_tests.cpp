#include "net/NetplayController.h"
#include "net/NetplayTimeline.h"
#include "core/EmulationSession.h"
#include "i18n/Localization.h"
#include "ui/NetplayLobby.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QSettings>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <QElapsedTimer>
#include <QSslSocket>
#include <QJsonDocument>
#include <QtEndian>
#include <cstdio>
#include <cstdlib>
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
    static void assign(NetplayController& c,quint64 id,bool link=false){c.transport_.setRole(id,link?kega::net::Role::Player2:kega::net::Role::Player1);}
    static void linkCable(NetplayController& c,bool enabled){c.lobby_->linkCableRequested(enabled);}
    static bool linked(NetplayController& c){return c.linkCable_;}
    static bool active(NetplayController& c){return c.active_;}
    static void spectate(NetplayController& c,quint64 id){c.transport_.setRole(id,kega::net::Role::Spectator);}
    static bool kick(NetplayController& c,quint64 id){return c.transport_.kick(id,false,false);}
    static int localRole(NetplayController& c){return c.localRole_;}
    static bool waiting(NetplayController& c){return c.connectionWaiting_;}
    static void stopWaiting(NetplayController& c){c.lobby_->stopWaitingRequested();}
    static bool abortConnection(NetplayController& c){
        for(auto* socket:c.transport_.findChildren<QSslSocket*>())
            if(socket->state()==QAbstractSocket::ConnectedState&&socket->isEncrypted()){socket->abort();return true;}
        return false;
    }
};
static void write(const QString& path,const QByteArray& bytes){QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("fixture write failed");}
static QByteArray read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return f.readAll();}
// Original 68000 fixture: P1 Up gives a green backdrop and PSG tone; P1 B
// gives red and silence. The linked consoles therefore have distinct outputs.
static QByteArray rom(bool outputs=false){
    QByteArray b(65536,char(0xff));qToBigEndian<quint32>(0xff0000,b.data());qToBigEndian<quint32>(0x200,b.data()+4);
    b.replace(0x100,16,"SEGA GENESIS    ");b.replace(0x120,16,"NET SESSION     ");b.replace(0x180,2,"GM");
    qToBigEndian<quint32>(0,b.data()+0x1a0);qToBigEndian<quint32>(65535,b.data()+0x1a4);b[0x1f0]='U';
    if(!outputs){b[0x200]=char(0x60);b[0x201]=char(0xfe);return b;}
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
int main(int argc,char** argv){
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    const auto args=app.arguments();
    const bool linkScenario=args.contains("--link");
    if(args.size()==2 || (args.size()==3 && linkScenario)){
        QTemporaryDir scratch; if(!scratch.isValid())return 1;
        write(scratch.filePath("host.md"),rom(linkScenario));write(scratch.filePath("renamed.md"),rom(linkScenario));
        auto other=rom(linkScenario);other[0x1000]='X';write(scratch.filePath("other.md"),other);
        QProcess host,client;
        QStringList hostArguments{"--host",args[1],scratch.path()};if(linkScenario)hostArguments.append("--link");
        host.start(args[0],hostArguments);
        if(!host.waitForStarted(5000)){std::fprintf(stderr,"host did not start\n");return 1;}
        QByteArray hostOut;
        for(int i=0;i<20&&!hostOut.contains("READY");++i){host.waitForReadyRead(500);hostOut+=host.readAllStandardOutput();}
        if(!hostOut.contains("READY")){std::fprintf(stderr,"host startup failed: %s\n",host.readAllStandardError().constData());host.kill();host.waitForFinished();return 1;}
        QStringList clientArguments{"--client",args[1],scratch.path()};if(linkScenario)clientArguments.append("--link");
        client.start(args[0],clientArguments);
        const bool hostDone=host.waitForFinished(29000);const bool clientDone=client.waitForFinished(5000);
        const auto hostError=host.readAllStandardError(),clientError=client.readAllStandardError();
        if(!hostDone)host.kill();if(!clientDone)client.kill();
        if(!hostDone||!clientDone||host.exitCode()!=0||client.exitCode()!=0){std::fprintf(stderr,"network session failed\nHOST: %s\nCLIENT: %s\n",hostError.constData(),clientError.constData());return 1;}
        std::puts(linkScenario?"link session: paired GGPO inputs, role2/spectator, reset, save/load, rewind, game change, live and rapid toggles, guest policy and disconnect cleanup passed":"netplay session: independent processes, spectator default, shared role OR input, pause, reset, save/load, rewind, game change and explicit download passed");return 0;
    }
    if(args.size()!=4 && !(args.size()==5 && linkScenario))return 2;
    const bool hosting=args[1]=="--host";const QString root=args[3],data=root+(hosting?"/host-data":"/client-data");QDir().mkpath(data);
    QSettings settings(data+"/settings.ini",QSettings::IniFormat);settings.setValue("netplay/nickname",hosting?"Host":"Client");
    EmulationSession session(args[2],data);
    NetplayController controller(session,data,settings,nullptr);
    QObject::connect(&controller,&NetplayController::openRomRequested,&session,[&](const QString& p){session.loadGame(p);});
    QObject::connect(&session,&EmulationSession::errorOccurred,&app,[&](const QString& e){std::fprintf(stderr,"CORE: %s\n",qPrintable(e));app.exit(1);});
    QObject::connect(&controller,&NetplayController::notice,&app,[&](const QString& e){std::fprintf(stderr,"NOTICE: %s\n",qPrintable(e));});
    int phase=hosting?-1:0,combined=0,frames=0,applied=0,resyncs=0,pausedFrames=0;
    int aloneStartFrames=0,leaveStartFrames=0,postLeaveFrames=0,kickStartFrames=0,postKickFrames=0,abortStartFrames=0,rejoinCombined=0,activeRewindRole=0,rewindStarted=0,rewindProgressed=0,rewindStopped=0;
    int playerStopFrame=0,spectatorStopFrame=0,restoredFrame=0,rewindProgressSignals=0;
    qint64 lastRewindRemaining=-1,lastRewindTotal=-1;
    bool pausedSeen=false,resumed=false,downloaded=false,hostCreated=false,stateLoaded=false,playerRewindAck=false,spectatorReady=false,spectatorRewindAck=false;
    bool clientLeft=false,clientGameClosed=false,clientRejoined=false,afterKickLeaving=false,afterKickRejoined=false,socketAborted=false;
    bool linkEverEnabled=false,guestToggleRejected=false,linkRetainedAcrossGames=false;
    int enabledLinkSnapshots=0,disabledLinkSnapshots=0,toggleStartFrames=0;
    int ownColorFrames=0,spectatorColorFrames=0,ownAudioFrames=0,spectatorAudioFrames=0;
    int rewindOwnColorFrames=0,rewindSpectatorColorFrames=0;
    QElapsedTimer aloneTimer,leaveTimer,kickTimer,abortTimer,cancelTimer;
    QObject::connect(&session,&EmulationSession::networkResyncRequested,&app,[&]{++resyncs;});
    QObject::connect(&session,&EmulationSession::networkLinkCableChanged,&app,[&](bool enabled){
        if(enabled){linkEverEnabled=true;++enabledLinkSnapshots;}
        else if(linkEverEnabled)++disabledLinkSnapshots;
        if(linkScenario && !hosting && enabled && !guestToggleRejected) {
            NetplayScenario::linkCable(controller,false);
            guestToggleRejected=NetplayScenario::linked(controller);
            if(!guestToggleRejected){std::fprintf(stderr,"guest changed Link Cable setting\n");app.exit(1);}
        }
    });
    QObject::connect(&session,&EmulationSession::networkSnapshotApplied,&app,[&](bool ok,const QString& e){if(ok)++applied;else{std::fprintf(stderr,"STATE: %s\n",qPrintable(e));app.exit(1);}});
    QObject::connect(&session,&EmulationSession::videoFrameReady,&app,[&]{
        QImage image;double aspect;session.takeVideoFrame(image,aspect);++frames;
        if(linkScenario&&NetplayScenario::linked(controller)&&!image.isNull()) {
            const int role=NetplayScenario::localRole(controller);const QRgb pixel=image.pixel(image.width()/2,image.height()/2);
            if(role==2 && qRed(pixel)>qGreen(pixel)+8){++ownColorFrames;if(activeRewindRole==1)++rewindOwnColorFrames;}
            if(role==1 && qGreen(pixel)>qRed(pixel)+8)++ownColorFrames;
            if(role==0 && qGreen(pixel)>qRed(pixel)+8){++spectatorColorFrames;if(activeRewindRole==2)++rewindSpectatorColorFrames;}
        }
    });
    QObject::connect(&session,&EmulationSession::audioSamples,&app,[&](const QByteArray& pcm,int){
        if(!linkScenario||!NetplayScenario::linked(controller))return;
        bool audible=false;for(qsizetype i=0;i+1<pcm.size();i+=2)if(std::abs(int(qFromLittleEndian<qint16>(pcm.constData()+i)))>64){audible=true;break;}
        const int role=NetplayScenario::localRole(controller);
        if((role==1&&audible)||(role==2&&!audible))++ownAudioFrames;
        if(role==0&&audible)++spectatorAudioFrames;
    });
    QObject::connect(&session,&EmulationSession::gameLoaded,&app,[&](const QString&,double,const QString&){
        if(!hosting||hostCreated)return;
        NetplayScenario::create(controller);
        const QString invite=NetplayScenario::invitation(controller);
        if(invite.isEmpty()){std::fprintf(stderr,"host invite unavailable\n");app.exit(1);return;}
        write(root+"/invite",invite.toUtf8());
        aloneStartFrames=frames;aloneTimer.start();hostCreated=true;
    });
    QObject::connect(&session,&EmulationSession::gameClosed,&app,[&]{if(!hosting&&clientLeft)clientGameClosed=true;});
    QObject::connect(&session,&EmulationSession::rewindingChanged,&app,[&](bool enabled){
        if(hosting)return;
        if(enabled){activeRewindRole=NetplayScenario::localRole(controller)==0?2:1;rewindStarted|=activeRewindRole;}
        else if(activeRewindRole){
            rewindStopped|=activeRewindRole;
            if(activeRewindRole==1)playerStopFrame=frames;else spectatorStopFrame=frames;
            activeRewindRole=0;
        }
    });
    QObject::connect(&session,&EmulationSession::rewindProgress,&app,[&](qint64 remaining,qint64 total){
        if(hosting)return;
        ++rewindProgressSignals;lastRewindRemaining=remaining;lastRewindTotal=total;
        if(activeRewindRole&&total>0&&remaining>=0&&remaining<=total)rewindProgressed|=activeRewindRole;
    });
    QObject::connect(&session,&EmulationSession::networkFrameReady,&app,[&](const QByteArray& b){MovieInput input;if(NetplayTimeline::parseInput(b.mid(16,30),input)&&
        (linkScenario?(input.buttons[0]==0x10 && input.buttons[1]==1):input.buttons[0]==0x11))++combined;});
    QObject::connect(&session,&EmulationSession::actionCompleted,&app,[&](const QString& text){
        if(hosting&&phase==4&&text==Localization::text(QStringLiteral("Загружено: слот %1")).arg(0)){
            stateLoaded=true;restoredFrame=frames;
        }
    });
    QObject::connect(&session,&EmulationSession::pausedChanged,&app,[&](bool p){
        if(!hosting&&p&&!pausedSeen){
            pausedSeen=true;const int before=frames;
            QTimer::singleShot(100,&session,[&]{session.advanceFrame();session.setFrameAdvanceHeld(true);});
            QTimer::singleShot(1000,&session,[&,before]{pausedFrames=frames-before;session.setFrameAdvanceHeld(false);session.setPaused(false);});
        }
        if(hosting&&phase==2&&!p)resumed=true;
    });
    QTimer poll;poll.setInterval(50);
    QObject::connect(&poll,&QTimer::timeout,&app,[&]{
        if(hosting){
            if(phase==-1){
                if(!hostCreated)return;
                if(frames-aloneStartFrames>=8){
                    phase=0;std::puts("READY");std::fflush(stdout);
                } else if(aloneTimer.elapsed()>1000){
                    std::fprintf(stderr,"host did not advance eight frames alone\n");app.exit(1);
                }
                return;
            }
            if(phase>0)session.setButtons(0,0x10);
            const auto peer=NetplayScenario::ready(controller);
            if(phase==0&&peer){
                if(NetplayScenario::role(controller,peer)!=0){std::fprintf(stderr,"joiner not spectator\n");app.exit(1);return;}
                if(linkScenario)NetplayScenario::linkCable(controller,true);
                NetplayScenario::assign(controller,peer,linkScenario);session.setButtons(0,0x10);phase=1;
            } else if(phase==1&&combined>=8){phase=2;session.setPaused(true);}
            else if(phase==2&&resumed&&peer){phase=3;session.saveState(0);QTimer::singleShot(150,&session,[&]{session.resetGame();});}
            else if(phase==3&&peer){phase=4;QTimer::singleShot(350,&session,[&]{session.loadState(0);});}
            else if(phase==4&&peer&&stateLoaded&&frames-restoredFrame>=20){phase=5;session.setRewind(true);QTimer::singleShot(200,&session,[&]{session.setRewind(false);});}
            else if(phase==5&&peer&&QFile::exists(root+"/player-rewind")&&combined>=60){phase=6;NetplayScenario::spectate(controller,peer);}
            else if(phase==6&&peer&&QFile::exists(root+"/spectator-ready")){phase=7;session.setRewind(true);QTimer::singleShot(200,&session,[&]{session.setRewind(false);});}
            else if(phase==7&&peer&&QFile::exists(root+"/spectator-rewind")){phase=8;NetplayScenario::assign(controller,peer,linkScenario);NetplayScenario::share(controller);session.closeGame();QTimer::singleShot(100,&session,[&]{session.loadGame(root+"/other.md");});}
            else if(phase==8&&peer&&combined>=90){
                if(linkScenario) {
                    linkRetainedAcrossGames=NetplayScenario::linked(controller);
                    if(!linkRetainedAcrossGames){std::fprintf(stderr,"Link Cable not retained after game change\n");app.exit(1);return;}
                    toggleStartFrames=frames;NetplayScenario::linkCable(controller,false);phase=80;
                } else {write(root+"/leave-request",QByteArray("ok"));leaveStartFrames=-1;leaveTimer.start();phase=9;}
            } else if(phase==80&&peer&&disabledLinkSnapshots>0&&frames-toggleStartFrames>=8) {
                if(NetplayScenario::linked(controller)||!NetplayScenario::active(controller)){std::fprintf(stderr,"Link Cable disable lost lobby\n");app.exit(1);return;}
                toggleStartFrames=frames;NetplayScenario::linkCable(controller,true);phase=81;
            } else if(phase==81&&peer&&NetplayScenario::linked(controller)&&frames-toggleStartFrames>=8) {
                toggleStartFrames=frames;
                NetplayScenario::linkCable(controller,false);NetplayScenario::linkCable(controller,true);
                NetplayScenario::linkCable(controller,false);NetplayScenario::linkCable(controller,true);phase=82;
            } else if(phase==82&&peer&&NetplayScenario::linked(controller)&&frames-toggleStartFrames>=8) {
                write(root+"/leave-request",QByteArray("ok"));leaveStartFrames=-1;leaveTimer.start();phase=9;
            } else if(phase==9){
                if(leaveStartFrames<0&&NetplayScenario::ready(controller)==0)leaveStartFrames=frames;
                if(leaveStartFrames>=0&&frames-leaveStartFrames>=8&&leaveTimer.elapsed()<=1000){
                    postLeaveFrames=frames-leaveStartFrames;write(root+"/rejoin-request",QByteArray("ok"));phase=10;
                } else if(leaveTimer.elapsed()>1000){
                    std::fprintf(stderr,"host did not advance eight frames after voluntary leave\n");app.exit(1);
                }
            } else if(phase==10&&peer){
                if(NetplayScenario::role(controller,peer)!=0){std::fprintf(stderr,"rejoiner not spectator\n");app.exit(1);return;}
                NetplayScenario::assign(controller,peer,linkScenario);rejoinCombined=combined;phase=11;
            } else if(phase==11&&peer&&combined>=rejoinCombined+8){
                if(!NetplayScenario::kick(controller,peer)){std::fprintf(stderr,"host kick failed\n");app.exit(1);return;}
                kickStartFrames=-1;kickTimer.start();phase=12;
            } else if(phase==12){
                if(kickStartFrames<0&&NetplayScenario::ready(controller)==0)kickStartFrames=frames;
                if(kickStartFrames>=0&&frames-kickStartFrames>=8&&kickTimer.elapsed()<=1000){
                    postKickFrames=frames-kickStartFrames;write(root+"/rejoin-after-kick",QByteArray("ok"));phase=13;
                } else if(kickTimer.elapsed()>1000){
                    std::fprintf(stderr,"host did not advance eight frames after kick\n");app.exit(1);
                }
            } else if(phase==13&&peer){
                if(NetplayScenario::role(controller,peer)!=0){std::fprintf(stderr,"second rejoiner not spectator\n");app.exit(1);return;}
                NetplayScenario::assign(controller,peer,linkScenario);rejoinCombined=combined;phase=14;
            } else if(phase==14&&peer&&combined>=rejoinCombined+8){
                write(root+"/abort-request",QByteArray("ok"));abortTimer.start();phase=15;
            } else if(phase==15){
                if(NetplayScenario::waiting(controller)){
                    NetplayScenario::stopWaiting(controller);
                    if(NetplayScenario::waiting(controller)){std::fprintf(stderr,"stop-waiting did not clear waiting\n");app.exit(1);return;}
                    abortStartFrames=frames;cancelTimer.start();phase=16;
                } else if(abortTimer.elapsed()>2000){
                    std::fprintf(stderr,"host did not enter connection waiting after abrupt loss\n");app.exit(1);
                }
            } else if(phase==16){
                if(frames-abortStartFrames>=8&&cancelTimer.elapsed()<=1000){
                    write(root+"/done",QByteArray("ok"));std::fprintf(stderr,"host combined=%d post-leave=%d post-kick=%d post-cancel=%d\n",combined,postLeaveFrames,postKickFrames,frames-abortStartFrames);app.exit(0);
                } else if(cancelTimer.elapsed()>1000){
                    std::fprintf(stderr,"host did not advance eight frames after stop-waiting\n");app.exit(1);
                }
            }
        } else {
            session.setButtons(0,1);
            if(pausedSeen&&!downloaded&&NetplayScenario::needsDownload(controller)){downloaded=true;NetplayScenario::download(controller);}
            const int rewindComplete=rewindStarted&rewindProgressed&rewindStopped;
            if(!playerRewindAck&&(rewindComplete&1)&&frames-playerStopFrame>=8){
                write(root+"/player-rewind",QByteArray("ok"));playerRewindAck=true;
            }
            if(playerRewindAck&&!spectatorReady&&NetplayScenario::localRole(controller)==0){
                write(root+"/spectator-ready",QByteArray("ok"));spectatorReady=true;
            }
            if(!spectatorRewindAck&&(rewindComplete&2)&&frames-spectatorStopFrame>=8){
                write(root+"/spectator-rewind",QByteArray("ok"));spectatorRewindAck=true;
            }
            if(!clientLeft&&QFile::exists(root+"/leave-request")){
                clientLeft=true;controller.leaveSession();
            }
            if(clientLeft&&clientGameClosed&&!clientRejoined&&QFile::exists(root+"/rejoin-request")){
                clientRejoined=true;NetplayScenario::join(controller,QString::fromUtf8(read(root+"/invite")));session.loadGame(root+"/other.md");
            }
            if(!afterKickLeaving&&QFile::exists(root+"/rejoin-after-kick")){
                afterKickLeaving=true;clientGameClosed=false;controller.leaveSession();
            }
            if(afterKickLeaving&&clientGameClosed&&!afterKickRejoined){
                afterKickRejoined=true;NetplayScenario::join(controller,QString::fromUtf8(read(root+"/invite")));session.loadGame(root+"/other.md");
            }
            if(!socketAborted&&QFile::exists(root+"/abort-request")){
                if(!NetplayScenario::abortConnection(controller)){std::fprintf(stderr,"client socket unavailable for abrupt loss\n");app.exit(1);return;}
                socketAborted=true;
            }
            if(QFile::exists(root+"/done")){
                std::fprintf(stderr,"client frames=%d snapshots=%d resyncs=%d paused=%d rewind=%d/%d/%d link=%d/%d color=%d/%d audio=%d/%d\n",frames,applied,resyncs,pausedSeen,rewindStarted,rewindProgressed,rewindStopped,enabledLinkSnapshots,disabledLinkSnapshots,ownColorFrames,spectatorColorFrames,ownAudioFrames,spectatorAudioFrames);
                app.exit(frames>20&&applied>=2&&pausedSeen&&downloaded&&pausedFrames>=2&&resyncs==0&&spectatorRewindAck&&clientRejoined&&afterKickRejoined&&socketAborted&&rewindComplete==3&&
                    (!linkScenario || (enabledLinkSnapshots>=2&&disabledLinkSnapshots>=1&&guestToggleRejected&&ownColorFrames>=8&&spectatorColorFrames>=1&&ownAudioFrames>=8&&spectatorAudioFrames>=1&&rewindOwnColorFrames>=1&&rewindSpectatorColorFrames>=1))?0:1);
            }
        }
    });poll.start();
    QTimer::singleShot(25000,&app,[&]{
        std::fprintf(stderr,"TIMEOUT phase=%d combined=%d applied=%d resyncs=%d frames=%d stateLoaded=%d sinceRestore=%d rewind=%d/%d/%d activeRole=%d playerStopFrame=%d spectatorStopFrame=%d progressSignals=%d lastProgress=%lld/%lld\n",
                     phase,combined,applied,resyncs,frames,stateLoaded,frames-restoredFrame,rewindStarted,rewindProgressed,rewindStopped,activeRewindRole,playerStopFrame,spectatorStopFrame,rewindProgressSignals,
                     static_cast<long long>(lastRewindRemaining),static_cast<long long>(lastRewindTotal));
        std::fprintf(stderr,"DETAIL: %s\n",NetplayScenario::debug(controller).constData());app.exit(1);
    });
    if(hosting)session.loadGame(root+"/host.md");
    else {NetplayScenario::join(controller,QString::fromUtf8(read(root+"/invite")));session.loadGame(root+"/renamed.md");}
    const int result=app.exec();controller.leaveSession();session.shutdown();return result;
}
