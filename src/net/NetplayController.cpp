#include "NetplayController.h"
#include "NetTransferWorker.h"
#include "NetplayTimeline.h"
#include "core/EmulationSession.h"
#include "ui/NetplayLobby.h"
#include "i18n/Localization.h"
#include <QSettings>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QJsonDocument>
#include <QDateTime>
#include <QHostInfo>
#include <QUrl>
#include <QUrlQuery>
#include <QMessageBox>
#include <QtEndian>
#include <algorithm>
using namespace kega::net;
namespace {
QString t(const char* s) { return Localization::text(QString::fromUtf8(s)); }
QByteArray json(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QString roleName(int n) { return n==1?QStringLiteral("player1"):n==2?QStringLiteral("player2"):QStringLiteral("spectator"); }
int roleValue(const QString& n) { return n=="player1"?1:n=="player2"?2:0; }
void append32(QByteArray& b,quint32 n) { n=qToBigEndian(n);b.append(reinterpret_cast<const char*>(&n),4); }
void append64(QByteArray& b,quint64 n){n=qToBigEndian(n);b.append(reinterpret_cast<const char*>(&n),8);}
QByteArray snapshotHeader(quint32 size,quint64 epoch) { QByteArray b(1,'\0');append32(b,size);append64(b,epoch);return b; }
QString manifestId(const QByteArray& bytes) { const auto m=NetFileTransfer::Manifest::fromBytes(bytes); return m?QString::fromLatin1(m->id.toHex()):QString(); }
}
NetplayController::NetplayController(EmulationSession& session,const QString& dataPath,QSettings& settings,QWidget* parentWindow)
    : QObject(parentWindow),session_(session),dataPath_(dataPath),settings_(settings),lobby_(new NetplayLobby(parentWindow)),transport_(this) {
    files_=new NetTransferWorker;
    files_->moveToThread(&fileThread_);
    connect(&fileThread_,&QThread::finished,files_,&QObject::deleteLater);
    fileThread_.start();
    inputTimer_.setInterval(16); inputTimer_.setTimerType(Qt::PreciseTimer);
    connect(&inputTimer_,&QTimer::timeout,this,&NetplayController::sendInput);
    reconnectTimer_.setInterval(5000);
    connect(&reconnectTimer_,&QTimer::timeout,this,[this] {
        if(!active_ || hosting_) { reconnectTimer_.stop();return; }
        if(transport_.isConnected()) { reconnectTimer_.stop();return; }
        if(++reconnectAttempts_>10) { reconnectTimer_.stop();status_=t("Связь потеряна. Выйдите из лобби и подключитесь снова.");refreshLobby();return; }
        JoinOptions o;QString error;
        if(!NetTransport::parseInvitation(joinInvite_,&o.host,&o.port,&o.inviteSecret,&error)) return;
        o.password=joinPassword_;o.nickname=nickname();o.identitySecret=identity_;
        transport_.join(o,&error);
    });
    connect(lobby_,&NetplayLobby::createRequested,this,&NetplayController::startHost);
    connect(lobby_,&NetplayLobby::joinRequested,this,&NetplayController::startJoin);
    connect(lobby_,&NetplayLobby::leaveRequested,this,&NetplayController::leaveSession);
    connect(lobby_,&NetplayLobby::selectRomRequested,this,&NetplayController::openRomRequested);
    connect(lobby_,&NetplayLobby::roleRequested,this,[this](const QString& text,const QString& role) {
        if(!active_ || !hosting_)return;
        bool valid=false;const auto id=text.toULongLong(&valid);if(!valid || !participants_.contains(id))return;
        const int value=roleValue(role);
        if(id==0) { localRole_=value;session_.setNetworkRole(value);participants_[id].insert("role",roleName(value)); }
        else if(!transport_.setRole(id,Role(value)))return;
        if(value==2)session_.setControllerConnected(1,true);
        inputs_.remove(id);sendInput();updateWaiting();refreshLobby();
    });
    connect(lobby_,&NetplayLobby::kickRequested,this,[this](const QString& id){if(hosting_)transport_.kick(id.toULongLong(),false,false);});
    connect(lobby_,&NetplayLobby::banRequested,this,[this](const QString& id,bool permanent){if(hosting_)transport_.kick(id.toULongLong(),permanent,true);});
    connect(lobby_,&NetplayLobby::allowCommandsRequested,this,[this](bool yes){if(hosting_){allowCommands_=yes;refreshLobby();}});
    connect(lobby_,&NetplayLobby::allowDownloadsRequested,this,[this](bool yes){
        if(!hosting_)return;
        allowDownloads_=yes;
        QMetaObject::invokeMethod(files_,[this,yes]{files_->setSharingEnabled(yes);});announceContent();refreshLobby();
    });
    connect(lobby_,&NetplayLobby::downloadRomRequested,this,[this]{
        if(!active_ || hosting_ || !allowDownloads_ || manifest_.isEmpty() || downloading_)return;
        downloading_=true;transferDone_=transferTotal_=0;
        const auto bytes=manifest_;const auto path=dataPath_;
        QMetaObject::invokeMethod(files_,[this,bytes,path]{files_->acceptManifest(bytes,path);});refreshLobby();
    });
    connect(lobby_,&NetplayLobby::cancelTransferRequested,this,[this]{
        files_->requestCancel();QMetaObject::invokeMethod(files_,&NetTransferWorker::cancel);downloading_=false;refreshLobby();
    });
    connect(files_,&NetTransferWorker::preparedSource,this,[this](const QString& source,int index,const QByteArray& bytes,const QString& error){
        if(!active_ || source!=gamePath_ || index!=sourceArchiveIndex_)return;
        if(bytes.isEmpty()){status_=error;emit notice(error);refreshLobby();return;}
        localManifest_=bytes;
        if(hosting_){manifest_=bytes;gameReady_=true;status_=t("Игра готова к подключению участников");announceContent();participants_[0].insert("readiness","ready");}
        else compareContent();
        refreshLobby();
    });
    connect(files_,&NetTransferWorker::requestReady,this,[this](const QByteArray& b){if(active_&&!hosting_&&downloading_){fileChunkExpected_=true;transport_.send(0,MessageKind::FileRequest,b);}});
    connect(files_,&NetTransferWorker::chunkReady,this,[this](quint64 id,const QByteArray& b){filePending_.remove(id);if(hosting_&&allowDownloads_)transport_.send(id,MessageKind::FileChunk,b);});
    connect(files_,&NetTransferWorker::progress,this,[this](qint64 done,qint64 total){transferDone_=done;transferTotal_=total;refreshLobby();});
    connect(files_,&NetTransferWorker::completed,this,[this](const QString& path){
        if(!active_||hosting_||!downloading_)return;
        downloading_=false;downloadedPath_=path;localManifest_=manifest_;emit openRomRequested(path);refreshLobby();
    });
    connect(files_,&NetTransferWorker::requestFailed,this,[this](quint64 id,const QString& error){
        filePending_.remove(id);if(hosting_)transport_.send(id,MessageKind::Control,json({{"op","file-error"},{"text",error.left(1024)}}));
    });
    connect(files_,&NetTransferWorker::failed,this,[this](const QString& e){downloading_=false;status_=e;emit notice(e);refreshLobby();});
    connect(&transport_,&NetTransport::peerLatency,this,[this](quint64 id,int ms){
        if(participants_.contains(id)){participants_[id].insert("pingMs",ms);refreshLobby();}
    });
    connect(&transport_,&NetTransport::messageReceived,this,&NetplayController::receive);
    connect(&transport_,&NetTransport::connected,this,[this](const PeerInfo& self){
        if(!active_||hosting_)return;
        localId_=self.id;localRole_=int(self.role);session_.setNetworkRole(localRole_);
        reconnectTimer_.stop();reconnectAttempts_=0;status_=t("Подключено к хосту");
        compareContent();
        refreshLobby();
    });
    connect(&transport_,&NetTransport::peerJoined,this,[this](const PeerInfo& peer){
        if(!active_)return;
        participants_.insert(peer.id,QJsonObject{{"id",QString::number(peer.id)},{"nickname",peer.nickname},
            {"role",roleName(int(peer.role))},{"readiness","rom-missing"},{"local",peer.id==localId_},{"host",false},{"pingMs",-1}});
        disconnectedAt_.remove(peer.id);
        if(hosting_) {announceContent();updateWaiting();}
        refreshLobby();
    });
    connect(&transport_,&NetTransport::peerRoleChanged,this,[this](quint64 id,Role role){
        if(participants_.contains(id))participants_[id].insert("role",roleName(int(role)));
        if(id==localId_){localRole_=int(role);session_.setNetworkRole(localRole_);}
        inputs_.remove(id);if(hosting_)updateWaiting();refreshLobby();
    });
    connect(&transport_,&NetTransport::peerLeft,this,[this](quint64 id,const QString& reason){
        if(!active_||stopping_)return;
        if(hosting_) {
            inputs_.remove(id);readyPeers_.remove(id);outgoingSnapshots_.remove(id);heldFrames_.remove(id);
            const bool removed=reason.contains("kick",Qt::CaseInsensitive)||reason.contains("ban",Qt::CaseInsensitive);
            if(removed||roleValue(participants_.value(id).value("role").toString())==0) {
                transport_.setRole(id,Role::Spectator);participants_.remove(id);disconnectedAt_.remove(id);
            } else {
                if(participants_.contains(id))participants_[id].insert("readiness","reconnecting");
                disconnectedAt_.insert(id,QDateTime::currentMSecsSinceEpoch());
            }
            updateWaiting();sendInput();
        } else if(id==0) {
            gameReady_=false;session_.setNetworkWaiting(true);status_=t("Связь потеряна. Переподключение…");
            // An explicit host removal is final for this attempt; don't hammer a ban.
            if(reason.contains("kick",Qt::CaseInsensitive)||reason.contains("ban",Qt::CaseInsensitive)||reason.contains("stopped",Qt::CaseInsensitive)) {
                status_=reason;reconnectTimer_.stop();
            } else {reconnectAttempts_=0;reconnectTimer_.start();}
        }
        refreshLobby();
    });
    connect(&transport_,&NetTransport::transportError,this,[this](const QString& error){status_=error;emit notice(error);refreshLobby();});
    connect(&session_,&EmulationSession::networkModeFailed,this,[this](const QString& error){leaveSession();emit notice(error);});
    connect(&session_,&EmulationSession::networkCommandRequested,this,[this](const QByteArray& b){
        if(!active_)return;
        if(hosting_)acceptCommand(0,b);
        else if(localRole_&&allowCommands_&&gameReady_)transport_.send(0,MessageKind::Command,b);
        else emit notice(t("Это действие доступно хосту и назначенным игрокам."));
    });
    connect(&session_,&EmulationSession::networkSnapshotReady,this,[this](const QByteArray& state){
        session_.acknowledgeNetworkSnapshot();
        if(!active_||!hosting_)return;
        snapshotRequested_=false;
        if(state.size()<8)return;
        const auto metadataLength=qFromBigEndian<quint32>(state.constData());
        hostEpoch_=QJsonDocument::fromJson(state.mid(4,metadataLength)).object().value("epoch").toString().toULongLong();
        readyPeers_.clear();outgoingSnapshots_.clear();outgoingOffsets_.clear();heldFrames_.clear();
        for(auto it=participants_.begin();it!=participants_.end();++it)if(it.key()&&it->value("matched").toBool()&&!disconnectedAt_.contains(it.key())) {
            it->insert("readiness","syncing");queueSnapshot(it.key(),state);
        }
        updateWaiting();refreshLobby();
    });
    connect(&session_,&EmulationSession::networkFrameReady,this,[this](const QByteArray& frame){
        session_.acknowledgeNetworkFrame();
        if(!active_||!hosting_)return;
        const auto ready=readyPeers_;
        for(auto id:ready)transport_.send(id,MessageKind::Frame,frame);
        for(auto it=outgoingSnapshots_.cbegin();it!=outgoingSnapshots_.cend();++it){
            auto& queue=heldFrames_[it.key()];
            if(queue.size()<240)queue.append(frame);
            else {transport_.kick(it.key(),false,false);break;}
        }
    });
    connect(&session_,&EmulationSession::networkSnapshotApplied,this,[this](bool ok,const QString& error){
        if(!active_||hosting_)return;
        gameReady_=ok;
        if(ok){status_=t("Игра синхронизирована");transport_.send(0,MessageKind::Control,json({{"op","snapshot-ready"},{"epoch",QString::number(applyingEpoch_)}}));}
        else {status_=error;emit notice(error);transport_.send(0,MessageKind::Control,json({{"op","sync-failed"}}));}
        applyingEpoch_=0;
        if(pendingIncomingEpoch_) {const auto size=pendingIncomingSize_;const auto epoch=pendingIncomingEpoch_;pendingIncomingEpoch_=0;pendingIncomingSize_=0;beginIncomingSnapshot(size,epoch);}
        refreshLobby();
    });
    connect(&session_,&EmulationSession::networkResyncRequested,this,[this]{
        if(active_&&!hosting_){gameReady_=false;transport_.send(0,MessageKind::Control,json({{"op","resync"}}));status_=t("Восстановление синхронизации…");refreshLobby();}
    });
    connect(&session_,&EmulationSession::actionCompleted,this,[this](const QString& text){
        if(active_&&hosting_)transport_.broadcast(MessageKind::Control,json({{"op","notice"},{"text",text.left(1024)}}));
    });
    connect(&session_,&EmulationSession::stateSlotsChanged,this,[this](int,unsigned mask){
        if(active_&&hosting_)transport_.broadcast(MessageKind::Control,json({{"op","slots"},{"mask",int(mask&0x3ff)}}));
    });
    connect(&session_,&EmulationSession::actionNotice,this,[this](const QString& text){
        if(active_&&hosting_)transport_.broadcast(MessageKind::Control,json({{"op","notice"},{"text",text.left(1024)}}));
    });
    connect(&session_,&EmulationSession::networkGameLoading,this,[this](const QString& path,int index){
        if(!active_)return;
        gameReady_=false;gamePath_=path;sourceArchiveIndex_=index;files_->requestCancel();
        // Preserve verification only for the explicitly completed download.
        if(path!=downloadedPath_)localManifest_.clear();
        incomingSnapshot_.clear();incomingSize_=0;pendingIncomingEpoch_=0;
        if(hosting_){manifest_.clear();readyPeers_.clear();for(auto& p:participants_)p.insert("matched",false);announceContent();}
        session_.setNetworkWaiting(!hosting_);
    });
    connect(&session_,&EmulationSession::networkGameSource,this,[this](const QString& path,int index){if(active_)prepareSource(path,index);});
    connect(&session_,&EmulationSession::gameLoaded,this,[this](const QString& path,double,const QString& name){gamePath_=path;gameName_=name;onGameLoaded(path);});
    connect(&session_,&EmulationSession::gameClosed,this,[this]{
        gamePath_.clear();gameName_.clear();localManifest_.clear();gameReady_=false;
        if(active_&&hosting_){manifest_.clear();readyPeers_.clear();announceContent();}
        refreshLobby();
    });
}
NetplayController::~NetplayController(){leaveSession();files_->requestCancel();fileThread_.quit();fileThread_.wait();}
QString NetplayController::nickname() const {return settings_.value("netplay/nickname",QHostInfo::localHostName()).toString().trimmed().left(32);}
void NetplayController::setNickname(const QString& name){const auto n=name.trimmed().left(32);if(n.isEmpty())return;settings_.setValue("netplay/nickname",n);if(hosting_){participants_[0].insert("nickname",n);refreshLobby();}}
QJsonArray NetplayController::bannedPeers() const {
    if(hosting_)return transport_.bans();
    QFile f(QFileInfo(settings_.fileName()).absolutePath()+"/netplay-bans.json");
    if(!f.open(QIODevice::ReadOnly)||f.size()>1024*1024)return {};
    const auto object=QJsonDocument::fromJson(f.readAll()).object();QJsonArray a;
    for(auto i=object.begin();i!=object.end();++i)a.append(QJsonObject{{"fingerprint",i.key()},{"nickname",i.value().toString()},{"persistent",true}});
    return a;
}
void NetplayController::unban(const QString& id){
    if(hosting_){transport_.unban(id);return;}
    const QString path=QFileInfo(settings_.fileName()).absolutePath()+"/netplay-bans.json";
    QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()>1024*1024)return;
    auto o=QJsonDocument::fromJson(f.readAll()).object();f.close();if(!o.contains(id))return;o.remove(id);
    QSaveFile out(path);if(out.open(QIODevice::WriteOnly)){out.write(json(o));out.commit();}
}
void NetplayController::createSession(){if(active_){showLobby();return;}lobby_->showCreateDialog();}
void NetplayController::joinSession(){if(active_){showLobby();return;}lobby_->showJoinDialog();}
void NetplayController::showLobby(){refreshLobby();lobby_->show();lobby_->raise();lobby_->activateWindow();}
void NetplayController::startHost(quint16 port,const QString& address,const QString& password){
    if(active_)return;
    HostOptions options;options.port=port;options.inviteSecret=NetTransport::generateSecret();options.password=password;
    options.banFilePath=QFileInfo(settings_.fileName()).absolutePath()+"/netplay-bans.json";
    QString error;if(!transport_.host(options,&error)){emit notice(error);QMessageBox::warning(lobby_,t("Онлайн"),error);return;}
    active_=hosting_=true;localId_=0;localRole_=1;allowCommands_=true;allowDownloads_=false;
    storageId_=QString::fromLatin1(NetTransport::generateSecret().left(16).toHex());
    invite_=NetTransport::makeInvitation(address,transport_.localPort(),options.inviteSecret);inviteWithPassword_=invite_;
    if(!password.isEmpty()){QUrl u(invite_);QUrlQuery q(u);q.addQueryItem("password",password);u.setQuery(q);inviteWithPassword_=u.toString(QUrl::FullyEncoded);}
    participants_.clear();participants_.insert(0,QJsonObject{{"id","0"},{"nickname",nickname()},{"role","player1"},{"host",true},{"local",true},{"readiness","waiting"},{"pingMs",0}});
    session_.setNetworkRole(1);session_.setNetworkMode(1,storageId_);session_.setNetworkWaiting(false);
    inputTimer_.start();status_=t("Лобби создано. Передайте приглашение другим игрокам.");
    emit activeChanged(true);showLobby();sendInput();
}
void NetplayController::startJoin(const QString& invitation,const QString& password){
    if(active_)return;
    JoinOptions options;QString error;
    if(!NetTransport::parseInvitation(invitation,&options.host,&options.port,&options.inviteSecret,&error)){QMessageBox::warning(lobby_,t("Онлайн"),error);return;}
    identity_=NetTransport::loadOrCreateIdentity(QFileInfo(settings_.fileName()).absolutePath()+"/netplay-identity.key",&error);
    if(identity_.isEmpty()){emit notice(error);return;}
    options.nickname=nickname();options.identitySecret=identity_;options.password=password;
    if(options.password.isEmpty())options.password=QUrlQuery(QUrl(invitation)).queryItemValue("password",QUrl::FullyDecoded);
    joinInvite_=invitation;joinPassword_=options.password;
    if(!transport_.join(options,&error)){QMessageBox::warning(lobby_,t("Онлайн"),error);return;}
    active_=true;hosting_=false;remoteGameOpen_=false;localRole_=0;localId_=0;allowDownloads_=false;gameReady_=false;
    storageId_=QString::fromLatin1(options.inviteSecret.left(16).toHex());
    session_.setNetworkMode(2,storageId_);session_.setNetworkRole(0);
    inputTimer_.start();status_=t("Подключение…");emit activeChanged(true);showLobby();
}
void NetplayController::leaveSession(){
    if(!active_)return;
    stopping_=true;active_=false;
    inputTimer_.stop();reconnectTimer_.stop();snapshotTimer_.stop();
    transport_.stop();session_.setNetworkMode(0);session_.setNetworkWaiting(false);
    files_->requestCancel();QMetaObject::invokeMethod(files_,&NetTransferWorker::cancel);
    QMetaObject::invokeMethod(files_,[this]{files_->setSharingEnabled(false);});
    participants_.clear();inputs_.clear();readyPeers_.clear();filePending_.clear();outgoingSnapshots_.clear();heldFrames_.clear();disconnectedAt_.clear();
    incomingSnapshot_.clear();manifest_.clear();localManifest_.clear();
    hosting_=false;gameReady_=false;downloading_=false;snapshotRequested_=false;applyingEpoch_=incomingEpoch_=pendingIncomingEpoch_=0;incomingSize_=pendingIncomingSize_=0;fileChunkExpected_=false;stopping_=false;
    status_=t("Онлайн-сессия завершена");emit activeChanged(false);refreshLobby();lobby_->hide();
}
void NetplayController::onGameLoaded(const QString&){if(active_){gameReady_=hosting_;refreshLobby();}}
void NetplayController::prepareSource(const QString& path,int archiveIndex){
    files_->requestCancel();gamePath_=path;sourceArchiveIndex_=archiveIndex;
    if(!hosting_&&path==downloadedPath_&&localManifest_==manifest_&&!manifest_.isEmpty()) {downloadedPath_.clear();compareContent();return;}
    localManifest_.clear();
    if(hosting_){manifest_.clear();readyPeers_.clear();announceContent();}
    const auto source=path;QMetaObject::invokeMethod(files_,[this,source,archiveIndex]{files_->prepare(source,archiveIndex);});
    status_=t("Проверка файлов игры…");refreshLobby();
}
void NetplayController::announceContent(){
    if(!hosting_)return;
    const QJsonObject o{{"name",gameName_},{"open",!gamePath_.isEmpty()},{"manifest",QString::fromLatin1(manifest_.toBase64())},{"downloads",allowDownloads_}};
    transport_.broadcast(MessageKind::Content,json(o));
}
void NetplayController::compareContent(){
    if(hosting_||!active_||manifest_.isEmpty()||localManifest_.isEmpty())return;
    const bool match=manifestId(manifest_)==manifestId(localManifest_);
    gameReady_=false;session_.setNetworkWaiting(true);
    transport_.send(0,MessageKind::Control,json({{"op","content-match"},{"id",manifestId(localManifest_)},{"match",match}}));
    status_=match?t("ROM совпадает. Синхронизация…"):t("ROM не совпадает с игрой хоста");refreshLobby();
}
void NetplayController::sendState(quint64){
    if(!hosting_||!session_.hasGame()||snapshotRequested_)return;
    snapshotRequested_=true;session_.requestNetworkSnapshot();
}
void NetplayController::queueSnapshot(quint64 peer,const QByteArray& state){
    if(state.isEmpty()||state.size()>128*1024*1024)return;
    outgoingSnapshots_.insert(peer,state);outgoingOffsets_.insert(peer,0);
    transport_.send(peer,MessageKind::Snapshot,snapshotHeader(quint32(state.size()),hostEpoch_));
}
void NetplayController::beginIncomingSnapshot(qsizetype size,quint64 epoch){
    if(size<=0||size>128*1024*1024||!epoch||manifest_.isEmpty()||manifestId(manifest_)!=manifestId(localManifest_))return;
    if(applyingEpoch_){pendingIncomingSize_=size;pendingIncomingEpoch_=epoch;return;}
    incomingSize_=size;incomingEpoch_=epoch;incomingSnapshot_.clear();incomingSnapshot_.reserve(size);
    gameReady_=false;session_.setNetworkWaiting(true);
    transport_.send(0,MessageKind::Control,json({{"op","snapshot-offset"},{"offset",0},{"epoch",QString::number(epoch)}}));
}
void NetplayController::updateWaiting(){
    if(!hosting_)return;
    bool wait=false;const auto now=QDateTime::currentMSecsSinceEpoch();QList<quint64> expired;
    for(auto it=participants_.begin();it!=participants_.end();++it){
        if(!it.key()||roleValue(it->value("role").toString())==0)continue;
        if(disconnectedAt_.contains(it.key())&&now-disconnectedAt_.value(it.key())>15000){
            it->insert("role","spectator");it->insert("readiness","disconnected");
            expired.append(it.key());continue;
        }
        if(!readyPeers_.contains(it.key()))wait=true;
    }
    for(auto id:expired){transport_.setRole(id,Role::Spectator);participants_.remove(id);disconnectedAt_.remove(id);}
    session_.setNetworkWaiting(wait);
    if(!expired.isEmpty())refreshLobby();
}
void NetplayController::sendInput(){
    if(!active_)return;
    const auto local=session_.networkLocalInput();
    if(!hosting_){if(transport_.isConnected()&&localRole_&&gameReady_)transport_.send(0,MessageKind::Input,local);return;}
    inputs_.insert(0,local);
    QList<QPair<int,MovieInput>> inputs;
    auto ids=participants_.keys();std::sort(ids.begin(),ids.end());
    for(auto id:ids){MovieInput input;if((id==0||readyPeers_.contains(id))&&NetplayTimeline::parseInput(inputs_.value(id),input))inputs.append({roleValue(participants_[id].value("role").toString()),input});}
    session_.setNetworkInput(NetplayTimeline::inputBytes(NetplayTimeline::merge(inputs)));updateWaiting();
}
void NetplayController::acceptCommand(quint64 peer,const QByteArray& b){
    if(!hosting_||b.size()>16*1024)return;
    if(peer&&(!allowCommands_||!readyPeers_.contains(peer)||roleValue(participants_.value(peer).value("role").toString())==0))return;
    const auto now=QDateTime::currentMSecsSinceEpoch();
    if(peer && now-commandTimes_.value(peer)<25 && QJsonDocument::fromJson(b).object().value("value").toInt()!=0)return;
    commandTimes_.insert(peer,now);session_.applyNetworkCommand(b);
}
void NetplayController::receive(quint64 peer,MessageKind kind,const QByteArray& b){
    if(!active_)return;
    if(kind==MessageKind::Input&&hosting_){
        MovieInput input;if(readyPeers_.contains(peer)&&roleValue(participants_.value(peer).value("role").toString())&&NetplayTimeline::parseInput(b,input)){inputs_.insert(peer,b);sendInput();}return;
    }
    if(kind==MessageKind::Command&&hosting_){acceptCommand(peer,b);return;}
    if(kind==MessageKind::FileRequest&&hosting_&&allowDownloads_){if(filePending_.contains(peer))return;filePending_.insert(peer);QMetaObject::invokeMethod(files_,[this,peer,b]{files_->requestChunk(peer,b);});return;}
    if(kind==MessageKind::FileChunk&&!hosting_&&downloading_){if(!fileChunkExpected_)return;fileChunkExpected_=false;QMetaObject::invokeMethod(files_,[this,b]{files_->receiveChunk(b);});return;}
    if(kind==MessageKind::Frame&&!hosting_){if(gameReady_)session_.receiveNetworkFrame(b);return;}
    if(kind==MessageKind::Content&&!hosting_){
        const auto o=QJsonDocument::fromJson(b).object();gameName_=o.value("name").toString().left(512);allowDownloads_=o.value("downloads").toBool();
        const auto next=QByteArray::fromBase64(o.value("manifest").toString().toLatin1());
        if(next!=manifest_){manifest_=next;gameReady_=false;incomingSnapshot_.clear();session_.setNetworkWaiting(true);compareContent();}
        const bool open=o.value("open").toBool();
        if(!open){gameReady_=false;session_.setNetworkWaiting(true);if(remoteGameOpen_)session_.closeNetworkGame();status_=t("Хост закрыл игру");}
        remoteGameOpen_=open;
        refreshLobby();return;
    }
    if(kind==MessageKind::Snapshot&&!hosting_){
        if(b.isEmpty())return;
        const auto tag=quint8(b[0]);
        if(tag==0&&b.size()==13){
            beginIncomingSnapshot(qFromBigEndian<quint32>(b.constData()+1),qFromBigEndian<quint64>(b.constData()+5));
        } else if(tag==1&&b.size()>13&&b.size()<=64*1024+13){
            const auto offset=qFromBigEndian<quint32>(b.constData()+1);
            const auto epoch=qFromBigEndian<quint64>(b.constData()+5);
            if(applyingEpoch_||epoch!=incomingEpoch_||offset!=quint32(incomingSnapshot_.size())||incomingSize_<=0||incomingSnapshot_.size()+b.size()-13>incomingSize_)return;
            incomingSnapshot_.append(b.constData()+13,b.size()-13);
            if(incomingSnapshot_.size()==incomingSize_){applyingEpoch_=incomingEpoch_;session_.receiveNetworkSnapshot(incomingSnapshot_);incomingSnapshot_.clear();incomingSize_=0;}
            else transport_.send(0,MessageKind::Control,json({{"op","snapshot-offset"},{"offset",incomingSnapshot_.size()},{"epoch",QString::number(epoch)}}));
        }
        return;
    }
    if(kind!=MessageKind::Control)return;
    const auto o=QJsonDocument::fromJson(b).object();const auto op=o.value("op").toString();
    if(hosting_){
        if(op=="content-match"){
            const bool match=!manifest_.isEmpty()&&o.value("id").toString()==manifestId(manifest_);
            participants_[peer].insert("matched",match);participants_[peer].insert("readiness",match?"syncing":"rom-mismatch");
            if(match)sendState(peer);else readyPeers_.remove(peer);updateWaiting();refreshLobby();
        } else if(op=="snapshot-offset"){
            if(!outgoingSnapshots_.contains(peer)||o.value("epoch").toString().toULongLong()!=hostEpoch_)return;
            const qsizetype offset=o.value("offset").toInteger(-1);const auto& state=outgoingSnapshots_[peer];
            if(offset<0||offset!=outgoingOffsets_.value(peer)||offset>=state.size())return;
            const auto size=std::min<qsizetype>(64*1024,state.size()-offset);QByteArray chunk(1,char(1));append32(chunk,quint32(offset));append64(chunk,hostEpoch_);chunk.append(state.constData()+offset,size);
            outgoingOffsets_[peer]=offset+size;transport_.send(peer,MessageKind::Snapshot,chunk);
        } else if(op=="snapshot-ready"){
            if(!outgoingSnapshots_.contains(peer)||o.value("epoch").toString().toULongLong()!=hostEpoch_||outgoingOffsets_.value(peer)!=outgoingSnapshots_[peer].size())return;
            outgoingSnapshots_.remove(peer);outgoingOffsets_.remove(peer);readyPeers_.insert(peer);participants_[peer].insert("readiness","ready");
            for(const auto& frame:heldFrames_.take(peer))transport_.send(peer,MessageKind::Frame,frame);
            updateWaiting();refreshLobby();
        } else if(op=="resync"){
            if(participants_.value(peer).value("matched").toBool())sendState(peer);
        } else if(op=="sync-failed"){
            outgoingSnapshots_.remove(peer);heldFrames_.remove(peer);readyPeers_.remove(peer);participants_[peer].insert("readiness","rom-mismatch");
            transport_.setRole(peer,Role::Spectator);updateWaiting();refreshLobby();
        }
    } else if(op=="slots") {session_.receiveNetworkSlots(unsigned(o.value("mask").toInt()));
    } else if(op=="file-error") {downloading_=false;files_->requestCancel();status_=o.value("text").toString().left(1024);emit notice(status_);refreshLobby();
    } else if(op=="notice") {emit notice(o.value("text").toString().left(1024));
    } else if(op=="lobby"){
        allowCommands_=o.value("allowCommands").toBool();
        const auto array=o.value("participants").toArray();if(array.size()>32)return;
        participants_.clear();for(const auto& value:array){auto participant=value.toObject();const auto id=participant.value("id").toString().toULongLong();participant.insert("local",id==localId_);participants_.insert(id,participant);}
        localRole_=roleValue(participants_.value(localId_).value("role").toString());session_.setNetworkRole(localRole_);refreshLobby();
    }
}
void NetplayController::refreshLobby(){
    QJsonArray list;auto ids=participants_.keys();std::sort(ids.begin(),ids.end());for(auto id:ids)list.append(participants_.value(id));
    if(hosting_)transport_.broadcast(MessageKind::Control,json({{"op","lobby"},{"participants",list},{"allowCommands",allowCommands_}}));
    QString romStatus=gameReady_?QStringLiteral("matched"):localManifest_.isEmpty()?QStringLiteral("missing"):QStringLiteral("verifying");
    if(!hosting_&&!localManifest_.isEmpty()&&!manifest_.isEmpty()&&manifestId(localManifest_)!=manifestId(manifest_))romStatus="mismatch";
    if(downloading_)romStatus="downloading";
    QJsonObject rom{{"filename",gameName_},{"status",romStatus},{"progress",transferTotal_?double(transferDone_)*100.0/double(transferTotal_):0},
        {"canSelect",active_},{"canDownload",active_&&!hosting_&&allowDownloads_&&!manifest_.isEmpty()&&!downloading_},{"canCancel",downloading_}};
    if(!hosting_) {
        int owners=0;for(const auto& participant:participants_)if(roleValue(participant.value("role").toString())==localRole_)++owners;
        session_.setNetworkRole(owners>1?0:localRole_);
    }
    lobby_->setLobbyState({{"active",active_},{"host",hosting_},{"status",status_},{"invite",invite_},{"inviteWithPassword",inviteWithPassword_},
        {"participants",list},{"rom",rom},{"allowCommands",allowCommands_},{"allowDownloads",allowDownloads_}});
}
