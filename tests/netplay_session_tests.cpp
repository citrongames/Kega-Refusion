#include "net/NetplayController.h"
#include "net/NetplayTimeline.h"
#include "core/EmulationSession.h"
#include "input/ControllerTopology.h"
#include "i18n/Localization.h"
#include "ui/NetplayLobby.h"
#include <QApplication>
#include <QComboBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QSettings>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <QElapsedTimer>
#include <QDateTime>
#include <QSslSocket>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QSet>
#include <QFileSystemWatcher>
#include <memory>
#include <functional>
#include <algorithm>
#include <QtEndian>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
class NetplayScenario {
public:
    static void verifyRoleCapacity(NetplayController& c) {
        const auto require=[](bool value,const char* text){if(!value)throw std::runtime_error(text);};
        c.active_=c.hosting_=true;
        c.participants_.insert(0,QJsonObject{{"id","0"},{"role","player1"}});
        c.participants_.insert(7,QJsonObject{{"id","7"},{"role","player8"}});
        c.participants_.insert(8,QJsonObject{{"id","8"},{"role","player4"}});
        c.playerCount_=8;
        c.setControllerTopology(1,5);
        require(c.activePlayerCount()==5 && c.participants_.value(7).value("role").toString()=="spectator" &&
                c.participants_.value(8).value("role").toString()=="player4","capacity shrink did not demote only invalid roles");
        c.lobby_->roleRequested("0","player6");
        require(c.participants_.value(0).value("role").toString()=="player1","unavailable role was assigned");
        c.lobby_->roleRequested("0","player5");
        require(c.participants_.value(0).value("role").toString()=="player5","available role was rejected");
        c.applyLinkCableConfiguration(true);
        require(!c.linkCable_,"Link Cable accepted a multitap adapter");
        c.setControllerTopology(0,2);
        require(c.participants_.value(0).value("role").toString()=="spectator" &&
                c.participants_.value(8).value("role").toString()=="spectator","adapter removal retained invalid role");
        c.leaveSession();
        c.active_=true;c.hosting_=false;c.localId_=5;
        const auto malformed=[](const QString& role,int count) {
            return QJsonDocument(QJsonObject{{"op","lobby"},{"playerCount",count},
                {"participants",QJsonArray{QJsonObject{{"id","5"},{"role",role}}}}}).toJson(QJsonDocument::Compact);
        };
        c.receive(0,kega::net::MessageKind::Control,malformed("player9",8));
        require(c.participants_.isEmpty(),"out-of-range network role accepted");
        c.receive(0,kega::net::MessageKind::Control,malformed("player8",2));
        require(c.participants_.isEmpty(),"role above advertised controller capacity accepted");
        c.receive(0,kega::net::MessageKind::Control,malformed("player2",9));
        require(c.participants_.isEmpty(),"invalid network controller capacity accepted");
        c.receive(0,kega::net::MessageKind::Control,
            QJsonDocument(QJsonObject{{"op","lobby"},{"playerCount",5},{"jcart",true},{"jcartBase",2},
                {"participants",QJsonArray{QJsonObject{{"id","5"},{"role","player5"}}}}}).toJson(QJsonDocument::Compact));
        require(c.participants_.isEmpty(),"inconsistent J-Cart role map accepted");
        c.receive(0,kega::net::MessageKind::Control,malformed("player5",5));
        require(c.localRole_==5 && c.playerCount_==5,"expanded host role was not applied");
        c.leaveSession();
    }
    static void verifyReadinessWaiting(NetplayController& c) {
        const auto require=[](bool value,const char* text){if(!value)throw std::runtime_error(text);};
        QObject observer;int changes=0;bool waiting=false;
        QObject::connect(&c,&NetplayController::readinessWaitingChanged,&observer,[&](bool value){waiting=value;++changes;});
        c.active_=c.hosting_=true;c.gameReady_=true;c.gamePath_=QStringLiteral("readiness-fixture.md");
        c.participants_.insert(7,QJsonObject{{"id","7"},{"role","spectator"},{"readiness","rom-missing"}});
        c.updateWaiting();c.refreshLobby();
        require(!waiting&&changes==0,"missing ROM spectator unexpectedly blocks readiness");
        c.participants_[7].insert("role","player2");c.updateWaiting();
        require(waiting&&changes==1&&!c.connectionWaiting_,"unloaded player2 has no readiness indication or is labelled disconnected");
        for(int i=0;i<5;++i){c.updateWaiting();c.refreshLobby();}
        require(changes==1,"unchanged readiness emitted repeated signals");
        c.readyPeers_.insert(7);c.updateWaiting();
        require(!waiting&&changes==2,"ready player did not clear waiting");
        c.readyPeers_.remove(7);c.updateWaiting();
        require(waiting&&changes==3,"player becoming unready did not restore waiting");
        c.participants_[7].insert("role","spectator");c.updateWaiting();
        require(!waiting&&changes==4,"spectator reassignment did not clear waiting");
        c.outgoingSnapshots_.insert(7,QByteArray("snapshot"));c.updateWaiting();
        require(waiting&&changes==5,"snapshot synchronization lacks waiting indication");
        c.outgoingSnapshots_.clear();c.updateWaiting();
        require(!waiting&&changes==6,"snapshot completion did not clear waiting");
        c.participants_[7].insert("role","player2");c.disconnectedAt_.insert(7,QDateTime::currentMSecsSinceEpoch());
        c.updateWaiting();c.refreshLobby();
        require(!waiting&&c.connectionWaiting_,"real disconnect should use connection loss rather than loading status");
        c.disconnectedAt_.clear();c.updateWaiting();c.refreshLobby();
        require(waiting&&!c.connectionWaiting_,"reconnected unready player did not return to loading status");
        c.participants_.remove(7);c.updateWaiting();
        require(!waiting,"participant exit did not clear waiting");
        c.participants_.insert(7,QJsonObject{{"id","7"},{"role","player2"}});c.updateWaiting();
        require(waiting,"player wait missing before closing host ROM");
        c.gamePath_.clear();c.refreshLobby();require(!waiting,"closed host ROM retained readiness OSD");
        c.gamePath_=QStringLiteral("readiness-fixture.md");c.refreshLobby();
        require(waiting,"player wait missing before leaving lobby");
        c.leaveSession();require(!waiting,"lobby exit did not clear readiness OSD");
        c.active_=true;c.hosting_=false;
        const auto lobby=[](bool ready){return QJsonDocument(QJsonObject{{"op","lobby"},{"participants",QJsonArray{}},
            {"readinessWaiting",ready},{"connectionWaiting",false},{"playerCount",2}}).toJson(QJsonDocument::Compact);};
        c.receive(0,kega::net::MessageKind::Control,lobby(true));
        require(waiting&&!c.connectionWaiting_,"client did not receive host readiness waiting");
        const int before=changes;c.receive(0,kega::net::MessageKind::Control,lobby(true));
        require(changes==before,"repeated lobby packet repeated readiness signal");
        c.receive(0,kega::net::MessageKind::Control,lobby(false));
        require(!waiting,"client did not clear host readiness waiting");c.leaveSession();
    }
    struct DatagramDelay {
        int pending=0, maximumPending=0;
        quint64 sequence=0;
        bool overflow=false;
    };
    static std::shared_ptr<DatagramDelay> delayDatagrams(NetplayController& c) {
        auto state=std::make_shared<DatagramDelay>();
        QObject::disconnect(&c.session_,&EmulationSession::networkDatagramReady,&c,nullptr);
        QObject::connect(&c.session_,&EmulationSession::networkDatagramReady,&c,
            [&c,state](quint64 peer,const QByteArray& packet) {
                c.session_.acknowledgeNetworkDatagram();
                if(!c.active_)return;
                if(state->pending>=128){state->overflow=true;return;}
                ++state->pending;state->maximumPending=std::max(state->maximumPending,state->pending);
                const int delayMs=80+int(state->sequence++%5)*5;
                QTimer::singleShot(delayMs,&c,[&c,state,peer,packet] {
                    --state->pending;
                    if(c.active_)c.datagrams_.send(peer,packet);
                });
            });
        return state;
    }
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
    static void assignRole(NetplayController& c,quint64 id,int role){c.lobby_->roleRequested(QString::number(id),QStringLiteral("player%1").arg(role));}
    static void linkCable(NetplayController& c,bool enabled){c.lobby_->linkCableRequested(enabled);}
    static bool linked(NetplayController& c){return c.linkCable_;}
    static bool active(NetplayController& c){return c.active_;}
    static void spectate(NetplayController& c,quint64 id){c.transport_.setRole(id,kega::net::Role::Spectator);}
    static bool kick(NetplayController& c,quint64 id){return c.transport_.kick(id,false,false);}
    static kega::net::NetTransport& transport(NetplayController& c){return c.transport_;}
    static bool finalState(NetplayController& c) {
        return !c.active_ && !c.lobby_->isVisible() && !c.reconnectTimer_.isActive() &&
            !c.inputTimer_.isActive() && !c.snapshotTimer_.isActive() && !c.connectionLost_ &&
            !c.connectionWaiting_ && !c.readinessWaiting_ && !c.transport_.isConnected();
    }
    static void moderate(NetplayController& c,quint64 id,int round) {
        if(round==0)c.lobby_->kickRequested(QString::number(id));
        else c.lobby_->banRequested(QString::number(id),round==3);
    }
    static bool unexpectedLossReconnects(NetplayController& c) {
        c.active_=true;c.hosting_=false;
        c.transport_.peerLeft(0,QStringLiteral("Connection reset"));
        const bool reconnects=c.active_&&c.connectionLost_&&c.reconnectTimer_.isActive();
        c.leaveSession();return reconnects;
    }
    static int localRole(NetplayController& c){return c.localRole_;}
    static int capacity(NetplayController& c){return c.activePlayerCount();}
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
// Separate from the normal/link regression scenarios: real wall-clock GGPO
// quality replies and bounded UDP delay run for one short, uninterrupted game.
static int qualityScenario(QApplication& app,const QStringList& args) {
    const auto resultPath=[](const QString& root,bool hosting) {
        return root+(hosting?"/quality-host.json":"/quality-client.json");
    };
    if(args.size()==3 && args[2]=="--quality") {
        QTemporaryDir scratch;if(!scratch.isValid())return 1;
        write(scratch.filePath("host.md"),rom(true));write(scratch.filePath("renamed.md"),rom(true));
        QProcess host,client;
        const auto diagnose=[](const char* stage,QProcess& process,QByteArray output=QByteArray{}) {
            output+=process.readAllStandardOutput();
            const auto errorOutput=process.readAllStandardError();
            std::fprintf(stderr,"quality %s: program=%s args=[%s] state=%d exitStatus=%d exitCode=%d(0x%08x) error=%d errorString=%s\nSTDOUT: %s\nSTDERR: %s\n",
                stage,qPrintable(process.program()),qPrintable(process.arguments().join(" | ")),
                int(process.state()),int(process.exitStatus()),process.exitCode(),unsigned(process.exitCode()),
                int(process.error()),qPrintable(process.errorString()),output.constData(),errorOutput.constData());
        };
        const QString executable=QCoreApplication::applicationFilePath();
        host.start(executable,{"--quality-host",args[1],scratch.path(),"--quality"});
        if(!host.waitForStarted(5000)){diagnose("host launch failed",host);return 1;}
        QByteArray startup;
        QElapsedTimer readiness;readiness.start();
        while(!startup.contains("READY") && readiness.elapsed()<10000) {
            startup+=host.readAllStandardOutput();
            if(startup.contains("READY") || host.state()==QProcess::NotRunning)break;
            const qint64 remaining=10000-readiness.elapsed();
            if(remaining<=0)break;
            host.waitForReadyRead(int(std::min<qint64>(250,remaining)));
        }
        startup+=host.readAllStandardOutput();
        if(!startup.contains("READY")){
            std::fprintf(stderr,"quality host readiness deadline elapsed=%lldms\n",static_cast<long long>(readiness.elapsed()));
            diagnose("host startup failed",host,startup);
            if(host.state()!=QProcess::NotRunning){host.kill();host.waitForFinished();}
            return 1;
        }
        client.start(executable,{"--quality-client",args[1],scratch.path(),"--quality"});
        if(!client.waitForStarted(5000)){
            diagnose("client launch failed",client);host.kill();host.waitForFinished();return 1;
        }
        const bool hostDone=host.waitForFinished(22000),clientDone=client.waitForFinished(5000);
        const auto hostError=host.readAllStandardError(),clientError=client.readAllStandardError();
        if(!hostDone){host.kill();host.waitForFinished();}
        if(!clientDone){client.kill();client.waitForFinished();}
        if(!hostDone||!clientDone||host.exitStatus()!=QProcess::NormalExit||client.exitStatus()!=QProcess::NormalExit||
           host.exitCode()!=0||client.exitCode()!=0) {
            std::fprintf(stderr,"quality session failed\nHOST: %s\nCLIENT: %s\n",hostError.constData(),clientError.constData());return 1;
        }
        const auto hostResult=QJsonDocument::fromJson(read(resultPath(scratch.path(),true))).object();
        const auto clientResult=QJsonDocument::fromJson(read(resultPath(scratch.path(),false))).object();
        const QString digest=hostResult.value("finalDigest").toString();
        if(digest.isEmpty()||digest!=clientResult.value("finalDigest").toString()) {
            std::fprintf(stderr,"quality session final real-core video output diverged\nHOST: %s\nCLIENT: %s\n",
                         QJsonDocument(hostResult).toJson(QJsonDocument::Compact).constData(),
                         QJsonDocument(clientResult).toJson(QJsonDocument::Compact).constData());return 1;
        }
        std::fprintf(stderr,"quality evidence HOST: %s\nCLIENT: %s\n",
                     QJsonDocument(hostResult).toJson(QJsonDocument::Compact).constData(),
                     QJsonDocument(clientResult).toJson(QJsonDocument::Compact).constData());
        std::puts("quality session: 80-100ms one-way UDP, alternating input, measured RTT, applied input-delay increase, rollback activity, no resync and converged real-core output passed");
        return 0;
    }
    if(args.size()!=5 || (args[1]!="--quality-host"&&args[1]!="--quality-client"))return 2;
    const bool hosting=args[1]=="--quality-host";
    const QString root=args[3],data=root+(hosting?"/quality-host-data":"/quality-client-data");
    std::fprintf(stderr,"quality child role=%s core=%s scratch=%s\n",hosting?"host":"client",qPrintable(args[2]),qPrintable(root));
    std::fflush(stderr);
    QDir().mkpath(data);
    QSettings settings(data+"/settings.ini",QSettings::IniFormat);
    settings.setValue("netplay/nickname",hosting?"Quality Host":"Quality Client");
    std::fprintf(stderr,"quality stage settings ready\n");std::fflush(stderr);
    EmulationSession session(args[2],data);
    std::fprintf(stderr,"quality stage worker created\n");std::fflush(stderr);
    NetplayController controller(session,data,settings,nullptr);
    std::fprintf(stderr,"quality stage controller created\n");std::fflush(stderr);
    const auto delayed=NetplayScenario::delayDatagrams(controller);
    QElapsedTimer running;
    int frames=0,startFrames=0,snapshots=0,snapshotsAtAssign=0,phase=0,resyncs=0;
    int qualitySamples=0,validRttSamples=0,maxPing=0,maxDelay=0,rollbackEvents=0,maxRollbackFrames=0,greenFrames=0;
    bool created=false,written=false;
    QSet<QRgb> observedColors;
    QByteArray finalDigest;
    QObject::connect(&session,&EmulationSession::errorOccurred,&app,[&](const QString& error) {
        std::fprintf(stderr,"quality core error: %s\n",qPrintable(error));app.exit(1);
    });
    QObject::connect(&controller,&NetplayController::notice,&app,[&](const QString& text) {
        std::fprintf(stderr,"quality notice: %s\n",qPrintable(text));
    });
    QObject::connect(&session,&EmulationSession::networkResyncRequested,&app,[&]{++resyncs;});
    QObject::connect(&session,&EmulationSession::networkSnapshotReady,&app,[&](const QByteArray&){++snapshots;});
    QObject::connect(&session,&EmulationSession::networkSnapshotApplied,&app,[&](bool ok,const QString& error) {
        if(!ok){std::fprintf(stderr,"quality snapshot failed: %s\n",qPrintable(error));app.exit(1);}
    });
    QObject::connect(&session,&EmulationSession::gameLoaded,&app,[&](const QString&,double,const QString&) {
        if(!hosting||created)return;
        created=true;NetplayScenario::create(controller);
        const QString invite=NetplayScenario::invitation(controller);
        if(invite.isEmpty()){std::fprintf(stderr,"quality host invite unavailable\n");app.exit(1);return;}
        write(root+"/invite",invite.toUtf8());std::puts("READY");std::fflush(stdout);
    });
    QObject::connect(&session,&EmulationSession::networkQualityChanged,&app,[&](const QJsonObject& quality) {
        if(!running.isValid()||!quality.value("active").toBool()||written)return;
        ++qualitySamples;
        const int ping=quality.value("pingMs").toInt(-1);
        if(ping>=0){++validRttSamples;maxPing=std::max(maxPing,ping);}
        maxDelay=std::max(maxDelay,quality.value("delayFrames").toInt(-1));
        rollbackEvents+=quality.value("rollbackEvents").toInt();
        maxRollbackFrames=std::max(maxRollbackFrames,quality.value("maxRollbackFrames").toInt());
    });
    QObject::connect(&session,&EmulationSession::videoFrameReady,&app,[&] {
        QImage image;double aspect;session.takeVideoFrame(image,aspect);++frames;
        if(!running.isValid()||image.isNull()||written)return;
        const auto elapsed=running.elapsed();
        const QRgb pixel=image.pixel(image.width()/2,image.height()/2);
        if(elapsed<11000)observedColors.insert(pixel);
        if(elapsed>=11000 && qGreen(pixel)>qRed(pixel)+8) {
            ++greenFrames;
            if(elapsed>=12500 && greenFrames>=12 && finalDigest.isEmpty()) {
                const QImage canonical=image.convertToFormat(QImage::Format_RGB32);
                finalDigest=QCryptographicHash::hash(
                    QByteArray::fromRawData(reinterpret_cast<const char*>(canonical.constBits()),canonical.sizeInBytes()),
                    QCryptographicHash::Sha256).toHex();
            }
        }
    });
    QTimer poll;poll.setInterval(50);
    QObject::connect(&poll,&QTimer::timeout,&app,[&] {
        if(delayed->overflow){std::fprintf(stderr,"quality injected packet queue exceeded 128 entries\n");app.exit(1);return;}
        if(!running.isValid()) {
            if(hosting) {
                const auto peer=NetplayScenario::ready(controller);
                if(phase==0 && peer) {
                    snapshotsAtAssign=snapshots;NetplayScenario::assign(controller,peer);phase=1;
                } else if(phase==1 && peer && snapshots>snapshotsAtAssign) {
                    running.start();startFrames=frames;write(root+"/quality-start",QByteArray("start"));
                }
            } else if(QFile::exists(root+"/quality-start")) {running.start();startFrames=frames;}
            return;
        }
        const auto elapsed=running.elapsed();
        const bool pressed=(elapsed/75)%2!=0;
        session.setButtons(0,elapsed<11000?(pressed?(hosting?0x10:1):0):(hosting?0x10:0));
        if(elapsed<13000)return;
        if(!written) {
            const QJsonObject result{{"frames",frames-startFrames},{"qualitySamples",qualitySamples},
                {"validRttSamples",validRttSamples},{"maxPingMs",maxPing},{"maxDelayFrames",maxDelay},
                {"rollbackEvents",rollbackEvents},{"maxRollbackFrames",maxRollbackFrames},{"resyncs",resyncs},
                {"maxPendingDatagrams",delayed->maximumPending},{"differentColors",observedColors.size()},
                {"settledGreenFrames",greenFrames},{"finalDigest",QString::fromLatin1(finalDigest)}};
            write(resultPath(root,hosting),QJsonDocument(result).toJson(QJsonDocument::Compact));written=true;
            if(frames-startFrames<240 || qualitySamples<6 || validRttSamples<3 || maxPing<120 ||
               maxDelay<1 || maxDelay>4 || rollbackEvents<=0 || maxRollbackFrames<=0 || resyncs!=0 ||
               observedColors.size()<2 || greenFrames<12 || finalDigest.isEmpty()) {
                std::fprintf(stderr,"quality requirements failed: %s\n",QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
                app.exit(1);return;
            }
        }
        if(QFile::exists(resultPath(root,!hosting)))app.exit(0);
    });
    poll.start();
    QTimer::singleShot(19000,&app,[&] {
        std::fprintf(stderr,"quality timeout role=%s phase=%d frames=%d samples=%d delay=%d rollback=%d resync=%d detail=%s\n",
                     hosting?"host":"client",phase,frames,qualitySamples,maxDelay,rollbackEvents,resyncs,
                     NetplayScenario::debug(controller).constData());app.exit(1);
    });
    QTimer::singleShot(3000,&app,[&]{std::fprintf(stderr,"quality stage event loop alive worker=%d loaded=%d frames=%d\n",int(session.isRunning()),int(session.hasGame()),frames);std::fflush(stderr);});
    std::fprintf(stderr,"quality stage requesting game\n");std::fflush(stderr);
    if(hosting)session.loadGame(root+"/host.md");
    else {NetplayScenario::join(controller,QString::fromUtf8(read(root+"/invite")));session.loadGame(root+"/renamed.md");}
    std::fprintf(stderr,"quality stage event loop starting\n");std::fflush(stderr);
    const int result=app.exec();controller.leaveSession();session.shutdown();return result;
}

// One short real-core GGPO session: the host P1 jack is disconnected, while
// the client local P1 drives logical P3 and the host local P1 drives P4.
static int multitapScenario(QApplication& app,const QStringList& args) {
    if(args.size()==3 && args[2]=="--multitap") {
        QTemporaryDir scratch;if(!scratch.isValid())return 1;
        write(scratch.filePath("host.md"),rom());write(scratch.filePath("client.md"),rom());
        const QString executable=QCoreApplication::applicationFilePath();
        QProcess host,client;
        host.start(executable,{"--multitap-host",args[1],scratch.path(),"--multitap"});
        if(!host.waitForStarted(5000)){std::fprintf(stderr,"multitap host launch failed\n");return 1;}
        QByteArray startup;QElapsedTimer ready;ready.start();
        while(!startup.contains("READY") && ready.elapsed()<10000 && host.state()!=QProcess::NotRunning) {
            host.waitForReadyRead(250);startup+=host.readAllStandardOutput();
        }
        startup+=host.readAllStandardOutput();
        if(!startup.contains("READY")) {
            std::fprintf(stderr,"multitap host not ready: %s\n",host.readAllStandardError().constData());
            if(host.state()!=QProcess::NotRunning){host.kill();host.waitForFinished();}return 1;
        }
        client.start(executable,{"--multitap-client",args[1],scratch.path(),"--multitap"});
        if(!client.waitForStarted(5000)){host.kill();host.waitForFinished();return 1;}
        const bool hostDone=host.waitForFinished(24000),clientDone=client.waitForFinished(5000);
        const auto hostError=host.readAllStandardError(),clientError=client.readAllStandardError();
        if(!hostDone){host.kill();host.waitForFinished();}
        if(!clientDone){client.kill();client.waitForFinished();}
        if(!hostDone||!clientDone||host.exitStatus()!=QProcess::NormalExit||client.exitStatus()!=QProcess::NormalExit||
           host.exitCode()!=0||client.exitCode()!=0) {
            std::fprintf(stderr,"multitap session failed\nHOST: %s\nCLIENT: %s\n",hostError.constData(),clientError.constData());return 1;
        }
        std::puts("multitap: disconnected host P1, remote P3/local P4, role demotion and chained Justifier GGPO packets passed");
        return 0;
    }
    if(args.size()!=5 || (args[1]!="--multitap-host" && args[1]!="--multitap-client"))return 2;
    const bool hosting=args[1]=="--multitap-host";
    const QString root=args[3],data=root+(hosting?"/multitap-host-data":"/multitap-client-data");
    QDir().mkpath(data);
    QSettings settings(data+"/settings.ini",QSettings::IniFormat);
    EmulationSession session(args[2],data);
    NetplayController controller(session,data,settings,nullptr);
    quint64 peerId=0;
    int phase=0,comboFrames=0,gunFrames=0,frames=0;
    bool gameLoaded=false,created=false,configSeen=false,gunConfigSeen=false,clientReported=false,demotedReported=false,gunReported=false;
    std::function<void()> maybeCreate=[&] {
        if(!hosting || created || !gameLoaded || session.controllerAdapter()!=int(ControllerAdapter::TeamPlayerPort1))return;
        created=true;NetplayScenario::create(controller);
        const auto invitation=NetplayScenario::invitation(controller);
        if(invitation.isEmpty()){std::fprintf(stderr,"multitap invitation unavailable\n");app.exit(1);return;}
        write(root+"/invite",invitation.toUtf8());std::puts("READY");std::fflush(stdout);
    };
    QObject::connect(&session,&EmulationSession::gameLoaded,&app,[&](const QString&,double,const QString&){gameLoaded=true;maybeCreate();});
    QObject::connect(&session,&EmulationSession::controllerTopologyChanged,&app,[&](int,unsigned){maybeCreate();});
    QObject::connect(&session,&EmulationSession::errorOccurred,&app,[&](const QString& error){
        std::fprintf(stderr,"multitap core error: %s\n",qPrintable(error));app.exit(1);
    });
    QObject::connect(&session,&EmulationSession::networkModeFailed,&app,[&](const QString& error){
        std::fprintf(stderr,"multitap network error: %s\n",qPrintable(error));app.exit(1);
    });
    QObject::connect(&session,&EmulationSession::networkSnapshotApplied,&app,[&](bool ok,const QString& error){
        if(!ok){std::fprintf(stderr,"multitap snapshot error: %s\n",qPrintable(error));app.exit(1);}
    });
    QObject::connect(&session,&EmulationSession::networkSnapshotReady,&app,[&](const QByteArray& state){
        if(!hosting || state.size()<8)return;
        const quint32 length=qFromBigEndian<quint32>(state.constData());
        if(length>4*1024*1024 || quint64(length)+8>quint64(state.size()))return;
        const auto config=QJsonDocument::fromJson(state.mid(4,length)).object().value("configuration").toObject();
        const auto ports=config.value("controllers").toArray();
        if(config.value("adapter").toInt(-1)==int(ControllerAdapter::TeamPlayerPort1) &&
           config.value("playerCount").toInt()==5 && ports.size()==int(MaxControllerPlayers) &&
           !ports[0].toObject().value("connected").toBool())configSeen=true;
        if(config.value("adapter").toInt(-1)==0 && config.value("playerCount").toInt()==3 &&
           ports.size()==int(MaxControllerPlayers) &&
           ports[1].toObject().value("kind").toInt(-1)==int(ControllerType::Justifier) &&
           ports[2].toObject().value("kind").toInt(-1)==int(ControllerType::Justifier) &&
           ports[1].toObject().value("connected").toBool() &&
           ports[2].toObject().value("connected").toBool())gunConfigSeen=true;
    });
    QObject::connect(&session,&EmulationSession::networkFrameReady,&app,[&](const QByteArray& packet){
        if(packet.size()!=56)return;
        MovieInput input;
        if(NetplayTimeline::parseInput(packet.mid(16,40),input) &&
           input.buttons[0]==0 && input.buttons[2]==1 && input.buttons[3]==0x10)
            ++comboFrames;
        if(input.buttons[1]==0x02 && input.buttons[2]==0x08 &&
           qint16(input.aim[1])==123 && qint16(input.aim[1]>>16)==-45 &&
           !(input.aim[1]&(quint64(1)<<32)) && (input.aim[2]&(quint64(1)<<32)))
            ++gunFrames;
        ++frames;
    });
    QTimer poll;poll.setInterval(50);
    QObject::connect(&poll,&QTimer::timeout,&app,[&] {
        const int role=NetplayScenario::localRole(controller);
        if(hosting && role==3 && session.controllerPlayerCount()==3) {
            session.setButtons(0,0);session.setButtons(1,0x08);
            session.setGunAim(1,-333,444,true);
        } else if(!hosting && role==2 && session.controllerPlayerCount()==3) {
            session.setButtons(0,0);session.setButtons(1,0x02);
            session.setGunAim(1,123,-45,false);
        } else session.setButtons(0,hosting?0x10:1);
        if(hosting) {
            if(phase==0 && (peerId=NetplayScenario::ready(controller))) {
                NetplayScenario::assignRole(controller,0,4);
                NetplayScenario::assignRole(controller,peerId,3);
                phase=1;
            } else if(phase==1 && configSeen && comboFrames>=12 && QFile::exists(root+"/multitap-p3")) {
                session.setControllerAdapter(ControllerAdapter::None);phase=2;
            } else if(phase==2 && session.controllerAdapter()==0 &&
                      NetplayScenario::localRole(controller)==0 && NetplayScenario::role(controller,peerId)==0 &&
                      QFile::exists(root+"/multitap-demoted")) {
                session.setControllerKind(1,ControllerType::Justifier);
                session.setControllerKind(2,ControllerType::Justifier);
                session.setControllerConnected(1,true);
                session.setControllerConnected(2,true);
                phase=3;
            } else if(phase==3 && session.controllerPlayerCount()==3 &&
                      NetplayScenario::capacity(controller)==3) {
                NetplayScenario::assignRole(controller,0,3);
                NetplayScenario::assignRole(controller,peerId,2);
                phase=4;
            } else if(phase==4 && gunConfigSeen && gunFrames>=12 &&
                      QFile::exists(root+"/multitap-gun")) {
                write(root+"/multitap-gun-done",QByteArray("ok"));app.exit(0);
            }
        } else {
            if(!clientReported && role==3 &&
               session.controllerAdapter()==int(ControllerAdapter::TeamPlayerPort1) &&
               comboFrames>=12) {
                write(root+"/multitap-p3",QByteArray("ok"));clientReported=true;
            }
            if(clientReported && !demotedReported && role==0 &&
               session.controllerAdapter()==0 && session.controllerPlayerCount()==2 &&
               session.networkActive() && session.hasGame()) {
                write(root+"/multitap-demoted",QByteArray("ok"));demotedReported=true;
            }
            if(demotedReported && !gunReported && role==2 &&
               session.controllerPlayerCount()==3 && gunFrames>=12) {
                write(root+"/multitap-gun",QByteArray("ok"));gunReported=true;
            }
            if(gunReported && QFile::exists(root+"/multitap-gun-done"))app.exit(0);
        }
    });
    poll.start();
    QTimer::singleShot(20000,&app,[&] {
        std::fprintf(stderr,"multitap timeout role=%s phase=%d frames=%d merged=%d gun=%d config=%d/%d localRole=%d adapter=%d detail=%s\n",
            hosting?"host":"client",phase,frames,comboFrames,gunFrames,configSeen,gunConfigSeen,
            NetplayScenario::localRole(controller),session.controllerAdapter(),
            NetplayScenario::debug(controller).constData());app.exit(1);
    });
    if(hosting) {
        session.setControllerAdapter(ControllerAdapter::TeamPlayerPort1);
        session.setControllerConnected(0,false);
        session.loadGame(root+"/host.md");
    } else {
        NetplayScenario::join(controller,QString::fromUtf8(read(root+"/invite")));
        session.loadGame(root+"/client.md");
    }
    const int result=app.exec();controller.leaveSession();session.shutdown();return result;
}

// One actual TLS lobby covers every terminal host action. Unique round markers
// wake directory watchers; they are not read or written from frame callbacks.
static int moderationScenario(QApplication& app,const QStringList& args) {
    if(args.size()==3 && args[2]=="--moderation") {
        QTemporaryDir scratch;if(!scratch.isValid())return 1;
        write(scratch.filePath("host.md"),rom());write(scratch.filePath("client.md"),rom());
        QProcess host,client;
        const QString executable=QCoreApplication::applicationFilePath();
        host.start(executable,{"--moderation-host",args[1],scratch.path(),"--moderation"});
        if(!host.waitForStarted(5000)){std::fprintf(stderr,"moderation host launch failed: %s\n",qPrintable(host.errorString()));return 1;}
        QByteArray startup;QElapsedTimer ready;ready.start();
        while(!startup.contains("READY")&&ready.elapsed()<10000&&host.state()!=QProcess::NotRunning) {
            host.waitForReadyRead(250);startup+=host.readAllStandardOutput();
        }
        startup+=host.readAllStandardOutput();
        if(!startup.contains("READY")) {
            std::fprintf(stderr,"moderation host not ready state=%d exit=%d error=%s stdout=%s stderr=%s\n",
                int(host.state()),host.exitCode(),qPrintable(host.errorString()),startup.constData(),host.readAllStandardError().constData());
            if(host.state()!=QProcess::NotRunning){host.kill();host.waitForFinished();}return 1;
        }
        client.start(executable,{"--moderation-client",args[1],scratch.path(),"--moderation"});
        if(!client.waitForStarted(5000)){
            std::fprintf(stderr,"moderation client launch failed: %s\n",qPrintable(client.errorString()));
            host.kill();host.waitForFinished();return 1;
        }
        const bool hostDone=host.waitForFinished(25000),clientDone=client.waitForFinished(5000);
        const auto hostError=host.readAllStandardError(),clientError=client.readAllStandardError();
        if(!hostDone){host.kill();host.waitForFinished();}
        if(!clientDone){client.kill();client.waitForFinished();}
        if(!hostDone||!clientDone||host.exitStatus()!=QProcess::NormalExit||client.exitStatus()!=QProcess::NormalExit||
           host.exitCode()!=0||client.exitCode()!=0) {
            std::fprintf(stderr,"moderation session failed\nHOST: %s\nCLIENT: %s\n",hostError.constData(),clientError.constData());return 1;
        }
        std::puts("moderation: kick, session/permanent bans, banned rejoins and host stop finalize once, unload client ROM, hide lobby, stop retries, retain host progress; unexpected loss still reconnects");
        return 0;
    }
    if(args.size()!=5 || (args[1]!="--moderation-host"&&args[1]!="--moderation-client"))return 2;
    const bool hosting=args[1]=="--moderation-host";
    const QString root=args[3],data=root+(hosting?"/moderation-host-data":"/moderation-client-data");
    QDir().mkpath(data);
    QSettings settings(data+"/settings.ini",QSettings::IniFormat);
    settings.setValue("netplay/nickname",hosting?"Moderation Host":"Moderation Client");
    EmulationSession session(args[2],data);
    NetplayController controller(session,data,settings,nullptr);
    const QStringList causes{"kicked","session-ban","session-ban","permanent-ban","permanent-ban","host-stopped"};
    const QStringList messages{
        Localization::text(QStringLiteral("Хост отключил вас от сеанса.")),
        Localization::text(QStringLiteral("Хост запретил вам вход до конца этого сеанса.")),
        Localization::text(QStringLiteral("Хост запретил вам вход до конца этого сеанса.")),
        Localization::text(QStringLiteral("Хост заблокировал ваши дальнейшие подключения.")),
        Localization::text(QStringLiteral("Хост заблокировал ваши дальнейшие подключения.")),
        Localization::text(QStringLiteral("Хост закрыл сеанс."))};
    bool failed=false,created=false,actionSent=false,reportReceived=false,terminal=false,reported=false;
    int round=0,frames=0,actionFrame=0,resyncs=0,resyncAtFinal=0,connections=0,connectionsAtFinal=0;
    int finalSignals=0,transportFinals=0,roundConnections=0,hostContinuations=0;
    QString lastCause,lastMessage;
    QElapsedTimer finalWait,actionWait;
    const auto check=[&](bool condition,const char* reason) {
        if(condition)return true;
        if(!failed)std::fprintf(stderr,"moderation %s round=%d failed: %s; detail=%s\n",
            hosting?"host":"client",round,reason,NetplayScenario::debug(controller).constData());
        failed=true;app.exit(1);return false;
    };
    const auto marker=[&](const char* kind,int index) {
        return root+QStringLiteral("/moderation-%1-%2").arg(QString::fromLatin1(kind)).arg(index);
    };
    const auto joinRound=[&] {
        terminal=reported=false;lastCause.clear();lastMessage.clear();roundConnections=connections;
        NetplayScenario::join(controller,QString::fromUtf8(read(root+"/invite")));
        session.loadGame(root+"/client.md");
    };
    QFileSystemWatcher watcher(QStringList{root});
    const auto readRoundMarker=[&] {
        if(failed)return;
        if(hosting) {
            if(!reportReceived && QFile::exists(marker("ended",round))) {
                const auto report=QJsonDocument::fromJson(read(marker("ended",round))).object();
                if(!check(report.value("round").toInt(-1)==round && report.value("cause").toString()==causes[round] &&
                          report.value("message").toString()==messages[round] && report.value("unloaded").toBool(),
                          "client terminal report was missing, wrong or incomplete"))return;
                reportReceived=true;
            }
        } else if(reported && round+1<causes.size() && QFile::exists(marker("join",round+1))) {
            ++round;joinRound();
        }
    };
    QObject::connect(&watcher,&QFileSystemWatcher::directoryChanged,&app,[&](const QString&){readRoundMarker();});
    QObject::connect(&session,&EmulationSession::errorOccurred,&app,[&](const QString& error) {
        std::fprintf(stderr,"moderation core error: %s\n",qPrintable(error));failed=true;app.exit(1);
    });
    QObject::connect(&session,&EmulationSession::networkSnapshotApplied,&app,[&](bool ok,const QString& error) {
        if(!ok){std::fprintf(stderr,"moderation snapshot error: %s\n",qPrintable(error));failed=true;app.exit(1);}
    });
    QObject::connect(&session,&EmulationSession::networkResyncRequested,&app,[&]{++resyncs;});
    QObject::connect(&session,&EmulationSession::videoFrameReady,&app,[&] {
        QImage image;double aspect;session.takeVideoFrame(image,aspect);++frames;
    });
    QObject::connect(&session,&EmulationSession::gameLoaded,&app,[&](const QString&,double,const QString&) {
        if(!hosting||created)return;
        created=true;NetplayScenario::create(controller);
        const QString invitation=NetplayScenario::invitation(controller);
        if(!check(!invitation.isEmpty(),"host invitation unavailable"))return;
        write(root+"/invite",invitation.toUtf8());std::puts("READY");std::fflush(stdout);
    });
    QObject::connect(&NetplayScenario::transport(controller),&kega::net::NetTransport::connected,&app,
        [&](const kega::net::PeerInfo&){++connections;});
    QObject::connect(&NetplayScenario::transport(controller),&kega::net::NetTransport::sessionEnded,&app,
        [&](const QString& cause,const QString&) {
            if(hosting){check(false,"host received its own terminal client event");return;}
            ++transportFinals;lastCause=cause;
            check(transportFinals==round+1 && cause==causes[round],"terminal transport cause was wrong or duplicated");
        });
    QObject::connect(&controller,&NetplayController::sessionEnded,&app,[&](const QString& message) {
        if(hosting){check(false,"host controller ended as a client");return;}
        ++finalSignals;lastMessage=message;
        if(!check(finalSignals==round+1 && message==messages[round],"friendly terminal message was wrong or duplicated") ||
           !check(resyncs==0,"client requested resynchronization before terminal cleanup") ||
           !check(NetplayScenario::finalState(controller)&&!session.networkActive(),
                  "sessionEnded was emitted before lobby/transport/timers stopped"))return;
        terminal=true;resyncAtFinal=resyncs;connectionsAtFinal=connections;finalWait.start();
    });
    QTimer poll;poll.setInterval(50);
    QObject::connect(&poll,&QTimer::timeout,&app,[&] {
        if(failed)return;
        if(hosting) {
            if(!actionSent) {
                if(round==2 || round==4) {
                    actionSent=true;actionFrame=frames;actionWait.start();
                } else if(const auto peer=NetplayScenario::ready(controller)) {
                    actionSent=true;actionFrame=frames;actionWait.start();
                    if(round==5)controller.leaveSession();
                    else NetplayScenario::moderate(controller,peer,round);
                }
            }
            if(actionSent && round!=5 && actionWait.elapsed()>1000 && frames-actionFrame<8) {
                check(false,"host failed to advance eight frames after moderation");return;
            }
            if(!reportReceived || !actionSent || (round!=5 && frames-actionFrame<8))return;
            if(!check(resyncs==0,"host requested resynchronization after moderation"))return;
            if(round!=5)++hostContinuations;
            if(round==2 || round==4) {
                const auto bans=controller.bannedPeers();
                if(!check(bans.size()==1 && bans[0].toObject().value("persistent").toBool()==(round==4),
                          "fixture ban persistence was wrong"))return;
                const QString fingerprint=bans[0].toObject().value("fingerprint").toString();
                const auto persisted=QJsonDocument::fromJson(read(data+"/netplay-bans.json")).object();
                if(!check(persisted.contains(fingerprint)==(round==4),"permanent ban was not persisted exclusively"))return;
                controller.unban(fingerprint);
                if(!check(controller.bannedPeers().isEmpty(),"fixture ban was not removed"))return;
            }
            if(round==5) {
                if(check(hostContinuations==5,"not every kick/ban/rejection retained host progress"))app.exit(0);
                return;
            }
            ++round;actionSent=reportReceived=false;
            write(marker("join",round),QByteArray("join"));
        } else if(terminal && !reported) {
            if(!check(NetplayScenario::finalState(controller)&&!session.networkActive(),
                      "final session revived a lobby, timer or transport") ||
               !check(resyncs==resyncAtFinal && connections==connectionsAtFinal,
                      "final session attempted resync or automatic reconnection"))return;
            if(session.hasGame()) {
                if(finalWait.elapsed()>1000)check(false,"final session did not unload client game");
                return;
            }
            const int holdMs=round==5?5200:250;
            if(finalWait.elapsed()<holdMs)return;
            if(!check(lastCause==causes[round] && finalSignals==round+1 && transportFinals==round+1,
                      "terminal notifications did not remain exactly once") ||
               !check(connections==roundConnections+((round==2||round==4)?0:1),
                      "banned rejoin unexpectedly authenticated"))return;
            const QJsonObject report{{"round",round},{"cause",lastCause},{"message",lastMessage},{"unloaded",!session.hasGame()}};
            write(marker("ended",round),QJsonDocument(report).toJson(QJsonDocument::Compact));reported=true;
            if(round==5) {
                if(check(NetplayScenario::unexpectedLossReconnects(controller),"unexpected loss no longer permits reconnect"))app.exit(0);
            }
        }
    });
    poll.start();
    QTimer::singleShot(21000,&app,[&] {
        std::fprintf(stderr,"moderation timeout role=%s round=%d action=%d report=%d terminal=%d finals=%d/%d frames=%d\n",
            hosting?"host":"client",round,actionSent,reportReceived,terminal,finalSignals,transportFinals,frames);
        app.exit(1);
    });
    if(hosting)session.loadGame(root+"/host.md");else joinRound();
    const int result=app.exec();controller.leaveSession();session.shutdown();return result;
}

void verifyGunRoleLabels() {
    const auto require=[](bool value,const char* text){if(!value)throw std::runtime_error(text);};
    NetplayLobby lobby;
    QJsonObject state{{"active",true},{"host",true},{"playerCount",3},
        {"roleKinds",QJsonArray{0,3,3}},
        {"participants",QJsonArray{QJsonObject{{"id","1"},{"role","player2"}},
                                    QJsonObject{{"id","2"},{"role","player3"}}}}};
    lobby.setLobbyState(state);
    auto* table=lobby.findChild<QTableWidget*>("netplayParticipants");
    require(table && table->rowCount()==2,"gun role table missing");
    auto* combo=qobject_cast<QComboBox*>(table->cellWidget(0,1));
    require(combo && combo->itemText(2).contains("Justifier 1") &&
            combo->itemText(3).contains("Justifier 2") &&
            table->item(1,1)->text().contains("Justifier 2"),
            "chained Justifier role labels missing");
    state.insert("playerCount",2);
    state.insert("roleKinds",QJsonArray{1,1});
    state.insert("participants",QJsonArray{QJsonObject{{"id","1"},{"role","player1"}},
                                           QJsonObject{{"id","2"},{"role","player2"}}});
    lobby.setLobbyState(state);
    combo=qobject_cast<QComboBox*>(table->cellWidget(0,1));
    require(combo && combo->itemText(1).contains("Light Phaser") &&
            combo->itemText(1).contains("1") && combo->itemText(2).contains("Light Phaser") &&
            combo->itemText(2).contains("2"),"SMS gun port labels missing");
    state.insert("roleKinds",QJsonArray{0,2});
    lobby.setLobbyState(state);
    combo=qobject_cast<QComboBox*>(table->cellWidget(0,1));
    require(combo && combo->itemText(2).contains("Menacer") && combo->itemText(2).contains("2"),
            "Menacer port label missing after kind update");
    state.insert("roleKinds",QJsonArray{4,5});
    lobby.setLobbyState(state);
    combo=qobject_cast<QComboBox*>(table->cellWidget(0,1));
    require(combo && combo->itemText(1).contains("MD Mouse") &&
            combo->itemText(1).contains("1") && combo->itemText(2).contains("SMS Paddle") &&
            combo->itemText(2).contains("2"),"mouse and paddle port labels missing");
    state.insert("playerCount",4);
    state.insert("jcart",true);state.insert("jcartBase",2);
    state.insert("roleKinds",QJsonArray{0,0,0,0});
    lobby.setLobbyState(state);
    combo=qobject_cast<QComboBox*>(table->cellWidget(0,1));
    require(combo && combo->itemText(3).contains("J-Cart") &&
            combo->itemText(3).contains("1") && combo->itemText(4).contains("J-Cart") &&
            combo->itemText(4).contains("2"),"ordinary-port J-Cart role labels missing");
    state.insert("playerCount",7);
    state.insert("jcartBase",5);
    state.insert("roleKinds",QJsonArray{0,0,0,0,0,0,0});
    lobby.setLobbyState(state);
    combo=qobject_cast<QComboBox*>(table->cellWidget(0,1));
    require(combo && !combo->itemText(5).contains("J-Cart") &&
            combo->itemText(6).contains("J-Cart") && combo->itemText(7).contains("J-Cart"),
            "single-adapter J-Cart role labels missing");
}

void verifyMultitapFrames() {
    const auto require=[](bool value,const char* text){if(!value)throw std::runtime_error(text);};
    const auto device=[](ControllerType kind,bool connected) {
        return QJsonObject{{"kind",int(kind)},{"connected",connected}};
    };
    const auto config=[](int adapter,const QJsonArray& controllers) {
        return QJsonObject{{"adapter",adapter},{"controllers",controllers}};
    };
    const auto pad=device(ControllerType::Pad,false);
    const auto phaser=device(ControllerType::LightPhaser,true);
    const auto menacer=device(ControllerType::Menacer,true);
    const auto justifier=device(ControllerType::Justifier,true);
    const auto mouse=device(ControllerType::Mouse,true);
    const auto paddle=device(ControllerType::Paddle,true);
    const auto mdMouse=config(0,QJsonArray{pad,mouse,pad});
    const auto smsPaddle=config(0,QJsonArray{paddle,paddle,pad});
    const auto smsBoth=config(0,QJsonArray{phaser,phaser,pad});
    const auto smsSecond=config(0,QJsonArray{pad,phaser,pad});
    const auto mdMenacer=config(0,QJsonArray{pad,menacer,pad});
    const auto mdJustifiers=config(0,QJsonArray{pad,justifier,justifier});
    const auto mdSecondOnly=config(0,QJsonArray{pad,device(ControllerType::Justifier,false),justifier});
    const auto adapter=config(int(ControllerAdapter::TeamPlayerPort1),QJsonArray{pad,pad,pad,pad});
    require(NetplayTimeline::localInputSource(1,smsBoth)==0 &&
            NetplayTimeline::localInputSource(2,smsBoth)==0 &&
            NetplayTimeline::localInputSource(2,smsSecond)==1 &&
            NetplayTimeline::localInputSource(2,mdMenacer)==1 &&
            NetplayTimeline::localInputSource(2,mdJustifiers)==1 &&
            NetplayTimeline::localInputSource(3,mdJustifiers)==1 &&
            NetplayTimeline::localInputSource(3,mdSecondOnly)==2 &&
            NetplayTimeline::localInputSource(4,adapter)==0 &&
            NetplayTimeline::localInputSource(2,config(0,QJsonArray{pad,pad,pad}))==0 &&
            NetplayTimeline::localInputSource(2,mdMouse)==1 &&
            NetplayTimeline::localInputSource(2,smsPaddle)==0,
            "pointer source must use first connected matching local device; pads use local P1");
    MovieInput first; first.buttons[0]=0x01;first.aim[0]=0x00010002;
    MovieInput another; another.buttons[0]=0x04;another.aim.fill(quint64(1)<<32);
    MovieInput fourth; fourth.buttons[0]=0x08;
    MovieInput sixth; sixth.buttons[0]=0x20;
    MovieInput seventh; seventh.buttons[0]=0x40;
    MovieInput eighth; eighth.buttons[0]=0x10;
    const auto merged=NetplayTimeline::merge({{3,first},{3,another},{4,fourth},{6,sixth},{7,seventh},
                                              {8,eighth},{9,first},{0,first}});
    require(merged.buttons[0]==0 && merged.buttons[2]==5 && merged.buttons[3]==8 &&
            merged.buttons[5]==32 && merged.buttons[6]==64 && merged.buttons[7]==16 &&
            merged.aim[2]==first.aim[0],"P3/P4/J-Cart P6/P7/P8 role merging failed");
    const auto bytes=NetplayTimeline::inputBytes(merged);
    require(bytes.size()==40 && qFromBigEndian<quint16>(bytes.constData()+4)==5 &&
            qFromBigEndian<quint16>(bytes.constData()+14)==16,"canonical 40-byte input layout failed");
    MovieInput parsed;require(NetplayTimeline::parseInput(bytes,parsed) && parsed==merged,"canonical input round-trip failed");
    require(!NetplayTimeline::parseInput(bytes.left(30),parsed) &&
            !NetplayTimeline::parseInput(bytes+QByteArray(1,'x'),parsed),"malformed input length accepted");
    auto badAim=bytes;badAim[16]=char(0x80);
    require(!NetplayTimeline::parseInput(badAim,parsed),"malformed aim bits accepted");
    require(NetplayTimeline::frameBytes(1,2,merged).size()==56 &&
            NetplayTimeline::frameBytes(1,2,merged,QByteArray(32,'x')).size()==88,
            "network frame layout failed");
    const auto relative=[](int x,int y) {
        return quint64(quint16(qint16(x))) | (quint64(quint16(qint16(y)))<<16);
    };
    MovieInput mouseA;mouseA.buttons[0]=0x02;mouseA.aim[0]=relative(200,-120);
    MovieInput mouseB;mouseB.buttons[0]=0x01;mouseB.aim[0]=relative(100,-200);
    MovieInput paddleA;paddleA.buttons[0]=0x01;paddleA.aim[0]=relative(197,0);
    MovieInput paddleB;paddleB.buttons[0]=0x08;paddleB.aim[0]=relative(42,0);
    const auto pointer=NetplayTimeline::merge({{1,mouseA},{1,mouseB},{2,paddleA},{2,paddleB}},1u);
    require(pointer.buttons[0]==0x03 && pointer.buttons[1]==0x09 &&
            qint16(pointer.aim[0])==255 && qint16(pointer.aim[0]>>16)==-255 &&
            pointer.aim[1]==paddleB.aim[0],
            "relative mouse sum or absolute paddle priority failed");
    MovieInput mouseC;mouseC.aim[0]=relative(-200,200);
    const auto cancellation=NetplayTimeline::merge({{1,mouseA},{1,mouseA},{1,mouseC}},1u);
    require(qint16(cancellation.aim[0])==200 && qint16(cancellation.aim[0]>>16)==-40,
            "mouse deltas were clamped before all peers were summed");
    const auto neutralMouse=NetplayTimeline::merge({},1u);
    require(neutralMouse.aim[0]==0 && neutralMouse.aim[1]==(quint64(1)<<32),
            "missing relative input must have neutral movement");
    const auto pointerBytes=NetplayTimeline::inputBytes(pointer);
    require(NetplayTimeline::parseInput(pointerBytes,parsed) && parsed==pointer,
            "mouse and paddle pointer frame failed to round-trip");
}

int main(int argc,char** argv){
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    const auto args=app.arguments();
    if(args.contains("--multitap"))return multitapScenario(app,args);
    if(args.contains("--quality"))return qualityScenario(app,args);
    if(args.contains("--moderation"))return moderationScenario(app,args);
    if(args.size()==2 && args[1]=="--waiting-only") {
        QTemporaryDir scratch;if(!scratch.isValid())return 1;
        QSettings settings(scratch.filePath("settings.ini"),QSettings::IniFormat);
        EmulationSession session(QString{},scratch.path());int result=0;
        {
            NetplayController controller(session,scratch.path(),settings,nullptr);
            try {verifyMultitapFrames();verifyGunRoleLabels();NetplayScenario::verifyRoleCapacity(controller);NetplayScenario::verifyReadinessWaiting(controller);std::puts("player readiness transitions, role release, synchronization, exit, no duplicate signals and remote status passed");}
            catch(const std::exception& error){std::fprintf(stderr,"readiness test failed: %s\n",error.what());result=1;}
        }
        session.shutdown();return result;
    }
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
    int forcedSessionEnds=0;bool finalKickObserved=false;
    bool linkEverEnabled=false,guestToggleRejected=false,linkRetainedAcrossGames=false;
    int enabledLinkSnapshots=0,disabledLinkSnapshots=0,toggleStartFrames=0;
    int ownColorFrames=0,spectatorColorFrames=0,ownAudioFrames=0,spectatorAudioFrames=0;
    int rewindOwnColorFrames=0,rewindSpectatorColorFrames=0;
    QElapsedTimer aloneTimer,leaveTimer,kickTimer,abortTimer,cancelTimer;
    QObject::connect(&session,&EmulationSession::networkResyncRequested,&app,[&]{++resyncs;});
    QObject::connect(&controller,&NetplayController::sessionEnded,&app,[&](const QString& message){
        if(hosting)return;
        ++forcedSessionEnds;
        finalKickObserved=forcedSessionEnds==1 && NetplayScenario::finalState(controller) &&
            message==Localization::text(QStringLiteral("Хост отключил вас от сеанса."));
        if(!finalKickObserved){std::fprintf(stderr,"kick did not finalize the client lobby once\n");app.exit(1);return;}
        afterKickLeaving=true;clientGameClosed=false;
    });
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
    QObject::connect(&session,&EmulationSession::networkFrameReady,&app,[&](const QByteArray& b){MovieInput input;if(NetplayTimeline::parseInput(b.mid(16,40),input)&&
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
            if(afterKickLeaving&&clientGameClosed&&!afterKickRejoined&&QFile::exists(root+"/rejoin-after-kick")){
                afterKickRejoined=true;NetplayScenario::join(controller,QString::fromUtf8(read(root+"/invite")));session.loadGame(root+"/other.md");
            }
            if(!socketAborted&&QFile::exists(root+"/abort-request")){
                if(!NetplayScenario::abortConnection(controller)){std::fprintf(stderr,"client socket unavailable for abrupt loss\n");app.exit(1);return;}
                socketAborted=true;
            }
            if(QFile::exists(root+"/done")){
                std::fprintf(stderr,"client frames=%d snapshots=%d resyncs=%d paused=%d rewind=%d/%d/%d link=%d/%d color=%d/%d audio=%d/%d\n",frames,applied,resyncs,pausedSeen,rewindStarted,rewindProgressed,rewindStopped,enabledLinkSnapshots,disabledLinkSnapshots,ownColorFrames,spectatorColorFrames,ownAudioFrames,spectatorAudioFrames);
                app.exit(frames>20&&applied>=2&&pausedSeen&&downloaded&&pausedFrames>=2&&resyncs==0&&spectatorRewindAck&&clientRejoined&&afterKickRejoined&&forcedSessionEnds==1&&finalKickObserved&&socketAborted&&rewindComplete==3&&
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
