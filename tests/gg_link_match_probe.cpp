// Optional real-ROM probe, deliberately excluded from CTest. No game data is bundled.
// Usage: gg_link_match_probe CORE COLUMNS_GG_ROM OUTPUT_DIRECTORY
#include "core/EmulationSession.h"
#include "core/LibretroHost.h"
#include "net/NetplayController.h"
#include "net/NetplayTimeline.h"
#include "ui/NetplayLobby.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <array>
#include <cstdio>
#include <stdexcept>

class NetplayScenario {
public:
    static void host(NetplayController& c) { c.startHost(0,QStringLiteral("127.0.0.1"),{}); }
    static void join(NetplayController& c,const QString& invite) { c.startJoin(invite,{}); }
    static QString invite(NetplayController& c) { return c.invite_; }
    static quint64 ready(NetplayController& c) { return c.readyPeers_.isEmpty()?0:*c.readyPeers_.begin(); }
    static void link(NetplayController& c) { c.lobby_->linkCableRequested(true); }
    static void player2(NetplayController& c,quint64 id) { c.lobby_->roleRequested(QString::number(id),QStringLiteral("player2")); }
    static int role(NetplayController& c) { return c.localRole_; }
};
static void require(bool ok,const QString& why) { if(!ok)throw std::runtime_error(why.toStdString()); }
static QByteArray read(const QString& path) { QFile f(path);require(f.open(QIODevice::ReadOnly),f.errorString());return f.readAll(); }
static void write(const QString& path,const QByteArray& bytes) { QSaveFile f(path);require(f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size()&&f.commit(),QStringLiteral("Cannot write %1").arg(path)); }
static QJsonObject metadata(const QByteArray& bytes) {
    require(bytes.size()>=4,"Truncated network snapshot");const auto n=qFromBigEndian<quint32>(bytes.constData());
    require(n<=4*1024*1024&&quint64(n)+4<quint64(bytes.size()),"Invalid snapshot metadata size");
    return QJsonDocument::fromJson(bytes.mid(4,n)).object();
}
static QByteArray imageHash(const QImage& input) {
    const auto image=input.convertToFormat(QImage::Format_RGB32);
    return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(image.constBits()),image.sizeInBytes()),QCryptographicHash::Sha256).toHex();
}
static unsigned controls(quint64 frame,bool hosting) {
    const auto pulse=[&](quint64 start){return frame>=start&&frame<start+12;};
    unsigned mask=0;
    if(pulse(500))mask|=1u<<5; // Versus becomes available only with a powered peer.
    if(pulse(hosting?550:650)||pulse(1100)||(hosting&&pulse(850)))mask|=1u<<3;
    if(!hosting&&frame>=1300)mask|=1u<<5; // Stack one column to finish both rounds.
    return mask;
}
static int participant(QApplication& app,const QStringList& args) {
    const bool hosting=args[1]=="--host";
    const QString core=args[2],rom=args[3],out=args[4],prefix=hosting?"host":"client";
    QTemporaryDir data;require(data.isValid(),"Cannot create session data directory");
    QSettings settings(data.filePath("settings.ini"),QSettings::IniFormat);
    settings.setValue("netplay/nickname",hosting?"GG match host":"GG match guest");
    EmulationSession session(core,data.path());session.setExpansionDevices(false,false);session.setRewindEnabled(false);
    NetplayController controller(session,data.path(),settings,nullptr);
    int phase=0,frames=0,snapshots=0,resyncs=0,combined=0;quint64 position=0;
    bool created=false,paused=false,finished=false;QImage latest;QElapsedTimer elapsed;elapsed.start();
    const auto fail=[&](const QString& reason){std::fprintf(stderr,"%s: %s\n",qPrintable(prefix),qPrintable(reason));app.exit(1);};
    QObject::connect(&session,&EmulationSession::errorOccurred,&app,fail);
    QObject::connect(&session,&EmulationSession::networkModeFailed,&app,fail);
    QObject::connect(&session,&EmulationSession::networkResyncRequested,&app,[&]{++resyncs;});
    QObject::connect(&controller,&NetplayController::notice,&app,[&](const QString& reason){std::fprintf(stderr,"%s notice: %s\n",qPrintable(prefix),qPrintable(reason));});
    QObject::connect(&controller,&NetplayController::openRomRequested,&session,[&](const QString& p){session.loadGame(p);});
    QObject::connect(&session,&EmulationSession::gameLoaded,&app,[&]{
        if(!hosting||created)return;created=true;NetplayScenario::host(controller);NetplayScenario::link(controller);
        write(out+"/invite",NetplayScenario::invite(controller).toUtf8());std::puts("READY");std::fflush(stdout);
    });
    QObject::connect(&session,&EmulationSession::videoFrameReady,&app,[&]{
        double aspect=0;QImage frame;if(!session.takeVideoFrame(frame,aspect))return;latest=frame;++frames;
        if(hosting&&phase==1&&position>1300&&frame.size()==QSize(160,144)) {
            // The two-line GRAND CHAMP banner fills this otherwise sparse playfield.
            // Stop on the rendered outcome; jewel RNG makes fixed frame counts unreliable.
            int white=0;for(int y=45;y<83;++y)for(int x=56;x<104;++x) {
                const auto pixel=frame.pixel(x,y);white+=qRed(pixel)>230&&qGreen(pixel)>230&&qBlue(pixel)>230;
            }
            if(white>500){phase=2;session.releaseButtons();session.setPaused(true);}
        }
    });
    QObject::connect(&session,&EmulationSession::networkSnapshotApplied,&app,[&](bool ok,const QString& error){if(ok)++snapshots;else fail(error);});
    QObject::connect(&session,&EmulationSession::networkSnapshotReady,&app,[&](const QByteArray& bytes){
        if(hosting&&phase==2&&metadata(bytes).value("paused").toBool())write(out+"/final.network",bytes);
    });
    QObject::connect(&session,&EmulationSession::pausedChanged,&app,[&](bool value){paused=value;});
    QObject::connect(&session,&EmulationSession::networkFrameReady,&app,[&](const QByteArray& bytes){
        if(bytes.size()<56)return;
        position=qFromBigEndian<quint64>(bytes.constData()+8);
        MovieInput input;if(NetplayTimeline::parseInput(bytes.mid(16,40),input)&&input.buttons[1]==(1u<<5)&&input.buttons[0]==0)++combined;
        if(!session.networkLinkCable()||NetplayScenario::role(controller)!=(hosting?1:2))return;
        if(hosting&&phase!=1)return;
        session.setButtons(0,controls(position,hosting));
    });
    QTimer poll;poll.setInterval(20);
    QObject::connect(&poll,&QTimer::timeout,&app,[&]{
        if(hosting&&phase==0&&session.networkLinkCable())if(const auto peer=NetplayScenario::ready(controller)) {
            phase=1;NetplayScenario::player2(controller,peer);
            session.setFastForwardFactor(8);session.setFastForward(true);session.hardResetGame();
        }
        if(paused&&!latest.isNull()&&!finished&&QFileInfo::exists(out+"/final.network")&&
           (hosting?QFileInfo::exists(out+"/client-done"):snapshots>0)) {
            finished=true;require(latest.save(out+"/"+prefix+"-result.png"),"Cannot save result image");
            QJsonObject result{{"frames_presented",frames},{"network_position",QString::number(position)},
                {"snapshot_count",snapshots},{"resyncs",resyncs},{"second_console_input_frames",combined},
                {"role",NetplayScenario::role(controller)},{"milliseconds",double(elapsed.elapsed())},
                {"image_sha256",QString::fromLatin1(imageHash(latest))}};
            write(out+"/"+prefix+".json",QJsonDocument(result).toJson());
            write(out+"/"+prefix+"-done","ok");
            if(hosting)app.exit(0);
        }
        if(!hosting&&finished&&QFileInfo::exists(out+"/host-done"))app.exit(0);
    });poll.start();
    QTimer::singleShot(180000,&app,[&]{fail(QStringLiteral("Timeout phase=%1 position=%2 frames=%3 snapshots=%4 role=%5 paused=%6")
        .arg(phase).arg(position).arg(frames).arg(snapshots).arg(NetplayScenario::role(controller)).arg(paused));});
    if(!hosting)NetplayScenario::join(controller,QString::fromUtf8(read(out+"/invite")));
    session.loadGame(rom);
    const int result=app.exec();controller.leaveSession();session.shutdown();return result;
}
static int driver(const QStringList& args) {
    require(args.size()==4,"usage: gg_link_match_probe CORE COLUMNS_GG_ROM OUTPUT_DIRECTORY");
    const QString core=QFileInfo(args[1]).absoluteFilePath(),rom=QFileInfo(args[2]).absoluteFilePath(),out=QFileInfo(args[3]).absoluteFilePath();
    require(!QFileInfo::exists(out),"Output directory must be new");require(QDir().mkpath(out),"Cannot create output directory");
    QProcess host,client;host.start(args[0],{"--host",core,rom,out});require(host.waitForStarted(5000),host.errorString());
    QByteArray startup;QElapsedTimer ready;ready.start();
    while(!startup.contains("READY")&&ready.elapsed()<20000&&host.state()!=QProcess::NotRunning){host.waitForReadyRead(500);startup+=host.readAllStandardOutput();}
    if(!startup.contains("READY")){host.kill();host.waitForFinished();throw std::runtime_error(("Host did not start: "+host.readAllStandardError()).toStdString());}
    client.start(args[0],{"--client",core,rom,out});require(client.waitForStarted(5000),client.errorString());
    const bool hostDone=host.waitForFinished(185000),clientDone=client.waitForFinished(5000);
    const auto hostError=host.readAllStandardError(),clientError=client.readAllStandardError();
    if(!hostDone){host.kill();host.waitForFinished();}if(!clientDone){client.kill();client.waitForFinished();}
    require(hostDone&&clientDone&&host.exitStatus()==QProcess::NormalExit&&client.exitStatus()==QProcess::NormalExit&&host.exitCode()==0&&client.exitCode()==0,
        QStringLiteral("Network match failed\nHOST: %1\nGUEST: %2").arg(QString::fromUtf8(hostError),QString::fromUtf8(clientError)));
    const auto hostResult=QJsonDocument::fromJson(read(out+"/host.json")).object(),clientResult=QJsonDocument::fromJson(read(out+"/client.json")).object();
    require(hostResult.value("resyncs").toInt()==0&&clientResult.value("resyncs").toInt()==0,"Unexpected desynchronization");
    require(hostResult.value("second_console_input_frames").toInt()>100&&clientResult.value("second_console_input_frames").toInt()>100,"Independent P2 input did not reach both machines");
    const QImage first(out+"/host-result.png"),second(out+"/client-result.png");
    require(first.size()==QSize(160,144)&&second.size()==QSize(160,144)&&imageHash(first)!=imageHash(second),"The two players did not receive distinct native GG screens");
    const auto snapshot=read(out+"/final.network");const auto meta=metadata(snapshot);
    require(meta.value("paused").toBool()&&meta.value("configuration").toObject().value("linkCable").toBool(),"Final state is not a paused linked game");
    const auto preview=QImage::fromData(QByteArray::fromBase64(meta.value("image").toString().toLatin1()));
    require(preview.size()==QSize(160,288),"Missing paired snapshot preview");
    require(imageHash(first)==imageHash(preview.copy(0,0,160,144))&&imageHash(second)==imageHash(preview.copy(0,144,160,144)),"Host/guest pictures disagree with their authenticated role-specific snapshot");
    const auto jsonSize=qFromBigEndian<quint32>(snapshot.constData());
    const auto state=LibretroHost::unpackNetworkState(snapshot.mid(4+jsonSize));require(!state.isEmpty(),"Invalid compressed final state");
    QTemporaryDir data;LibretroHost verify(core,data.path());verify.setExpansionDevices(false,false);QString error;
    require(verify.setLinkCableEnabled(true,error),error);require(verify.load(rom,error),error);require(verify.restoreNetworkState(state,error),error);
    QJsonArray scores;
    // Columns' reference symbols identify RESULT/RIVAL_RESULT at GG RAM C6CC/C6CD.
    // https://www.smspower.org/Development/Columns-GG ; never writes game memory.
    for(int role=1;role<=2;++role){verify.setLinkPresentationRole(role);std::array<uint8_t,2> result{};
        require(verify.readAchievementMemory(0x6cc,result.data(),result.size())==result.size(),"Cannot inspect GG result counters");
        const int winner = result[role == 1 ? 0 : 1];
        const int runnerUp = result[role == 1 ? 1 : 0];
        require(winner == 2 && runnerUp < 2, QStringLiteral("Best-of-three role %1 ended at %2:%3").arg(role).arg(result[0]).arg(result[1]));
        if (role == 2)
            require(runnerUp == scores[0].toObject().value("losses").toInt(), "Final scores differ between consoles");
        uint8_t mode=0;require(verify.readAchievementMemory(0x18,&mode,1)==1&&mode==0x15,"Game is not on its versus result screen");
        scores.append(QJsonObject{{"role",role},{"wins",result[0]},{"losses",result[1]}});
    }
    const QJsonObject summary{{"game",QFileInfo(rom).fileName()},{"rom_sha256",QString::fromLatin1(QCryptographicHash::hash(read(rom),QCryptographicHash::Sha256).toHex())},
        {"real_transport",true},{"native_size","160x144"},{"host",hostResult},{"guest",clientResult},{"scores",scores},
        {"role_specific_snapshot_images_equal",true},{"distinct_screens",true},{"complete_best_of_three",true}};
    write(out+"/result.json",QJsonDocument(summary).toJson());std::puts(QJsonDocument(summary).toJson(QJsonDocument::Compact).constData());return 0;
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    try{const auto args=app.arguments();if(args.size()==5&&(args[1]=="--host"||args[1]=="--client"))return participant(app,args);return driver(args);}
    catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
