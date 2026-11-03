#include "NetplayController.h"
#include "NetTransferWorker.h"
#include "NetAddressDiscovery.h"
#include "NetInterfaces.h"
#include "core/EmulationSession.h"
#include "input/ControllerTopology.h"
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
QString roleName(int n) { return n>=1 && n<=int(MaxControllerPlayers)?QStringLiteral("player%1").arg(n):QStringLiteral("spectator"); }
int roleValue(const QString& n) {
    if (!n.startsWith(QLatin1String("player"))) return 0;
    bool ok=false; const int value=n.mid(6).toInt(&ok);
    return ok && value>=1 && value<=int(MaxControllerPlayers) && n==roleName(value) ? value : 0;
}
void append32(QByteArray& b,quint32 n) { n=qToBigEndian(n);b.append(reinterpret_cast<const char*>(&n),4); }
void append64(QByteArray& b,quint64 n){n=qToBigEndian(n);b.append(reinterpret_cast<const char*>(&n),8);}
QByteArray snapshotHeader(quint32 size,quint64 epoch) { QByteArray b(1,'\0');append32(b,size);append64(b,epoch);return b; }
QJsonObject snapshotMetadata(const QByteArray& bytes) {
    if(bytes.size()<8)return {};
    const auto length=qFromBigEndian<quint32>(bytes.constData());
    if(length>4*1024*1024 || quint64(length)+8>quint64(bytes.size()))return {};
    return QJsonDocument::fromJson(bytes.mid(4,length)).object();
}
QString manifestId(const QByteArray& bytes) { const auto m=NetFileTransfer::Manifest::fromBytes(bytes); return m?QString::fromLatin1(m->id.toHex()):QString(); }
bool validRoleKinds(const QJsonArray& kinds) {
    if(kinds.size()>int(MaxControllerPlayers))return false;
    for(const auto& value:kinds)if(!value.isDouble() || value.toInt(-1)<0 || value.toInt(-1)>3)return false;
    return true;
}
QJsonArray roleKindsFromConfiguration(const QJsonObject& configuration) {
    QJsonArray kinds;
    const auto controllers=configuration.value("controllers").toArray();
    for(int i=0;i<controllers.size() && i<int(MaxControllerPlayers);++i)
        kinds.append(controllers[i].toObject().value("kind").toInt(0));
    return validRoleKinds(kinds)?kinds:QJsonArray{};
}

bool validSnapshotRoster(const QJsonObject& metadata,quint64 localId) {
    const auto config=metadata.value("configuration").toObject();
    const int count=config.value("linkCable").toBool()?2:config.value("playerCount").toInt(-1);
    const auto roster=metadata.value("roster").toArray();
    if(count<2 || count>int(MaxControllerPlayers) || roster.isEmpty() || roster.size()>32)return false;
    bool host=false,local=false;
    QSet<quint64> ids;
    for(const auto& item:roster) {
        const auto entry=item.toObject();bool ok=false;
        const quint64 id=entry.value("id").toString().toULongLong(&ok);
        const int role=entry.value("role").toInt(-1);
        if(!ok || role<0 || role>count || ids.contains(id))return false;
        ids.insert(id);host|=id==0;local|=id==localId;
    }
    return host && local;
}

}
NetplayController::NetplayController(EmulationSession& session,const QString& dataPath,QSettings& settings,QWidget* parentWindow)
    : QObject(parentWindow),session_(session),dataPath_(dataPath),settings_(settings),lobby_(new NetplayLobby(parentWindow)),transport_(this),datagrams_(this) {
    connect(&datagrams_,&NetDatagramChannel::packet,this,[this](quint64 peer,const QByteArray& packet){
        if(active_)session_.receiveNetworkDatagram(peer,packet);
    });
    connect(&datagrams_,&NetDatagramChannel::ready,this,[this](quint64 peer){
        if(active_&&hosting_){udpReadyPeers_.insert(peer);updateRoster();updateWaiting();}
    });
    connect(&datagrams_,&NetDatagramChannel::error,this,[this](const QString& error){
        if(active_){status_=error;emit notice(error);refreshLobby();}
    });
    connect(&session_,&EmulationSession::networkQualityChanged,this,[this](const QJsonObject& quality){
        session_.acknowledgeNetworkQuality();
        if(!active_)return;
        networkQuality_=quality;lobby_->setNetworkQuality(quality);
    });
    connect(&session_,&EmulationSession::networkDatagramReady,this,[this](quint64 peer,const QByteArray& packet){
        session_.acknowledgeNetworkDatagram();
        if(active_)datagrams_.send(peer,packet);
    });
    connect(&session_,&EmulationSession::networkStepRequested,this,[this](quint64 epoch){
        if(active_&&hosting_)transport_.broadcast(MessageKind::Control,json({{"op","step"},{"epoch",QString::number(epoch)}}));
    });
    connect(&session_,&EmulationSession::networkRewindPreviewReady,this,[this](const QByteArray& preview){
        session_.acknowledgeNetworkRewindPreview();
        if(active_&&hosting_)transport_.broadcast(MessageKind::Frame,preview);
    });
    addressDiscovery_=new NetAddressDiscovery(this);
    connect(addressDiscovery_,&NetAddressDiscovery::finished,this,[this](const QString& address,const QString& detail){
        if(!active_||!hosting_||!automaticAddress_||!addressPending_)return;
        addressPending_=false;addressRetry_=address.isEmpty();
        if(address.isEmpty())addressStatus_=t("Не удалось определить внешний адрес. Можно повторить или указать его в дополнительных параметрах при создании лобби.");
        else {setHostAddress(address);addressStatus_=t("Адрес определён через %1. Для подключения нужен доступ к TCP-порту %2.").arg(addressDiscovery_->connectionName()).arg(transport_.localPort());}
        Q_UNUSED(detail);
        refreshLobby();
    });
    connect(lobby_,&NetplayLobby::retryAddressRequested,this,&NetplayController::resolveHostAddress);
    files_=new NetTransferWorker;
    files_->moveToThread(&fileThread_);
    connect(&fileThread_,&QThread::finished,files_,&QObject::deleteLater);
    fileThread_.start();
    inputTimer_.setInterval(1000); inputTimer_.setTimerType(Qt::PreciseTimer);
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
    connect(lobby_,&NetplayLobby::stopWaitingRequested,this,&NetplayController::stopWaiting);
    connect(lobby_,&NetplayLobby::selectRomRequested,this,&NetplayController::openRomRequested);
    connect(lobby_,&NetplayLobby::roleRequested,this,[this](const QString& text,const QString& role) {
        if(!active_ || !hosting_)return;
        bool valid=false;const auto id=text.toULongLong(&valid);if(!valid || !participants_.contains(id))return;
        const int value=roleValue(role);
        if(role!=roleName(value) || value>activePlayerCount()) { refreshLobby(); return; }
        if(value==0)releasePeerCommands(id);
        // Queue the hardware port before the roster change that can start a new GGPO snapshot.
        if(value>=2)session_.setControllerConnected(unsigned(value-1),true);
        if(id==0) { localRole_=value;session_.setNetworkRole(value);participants_[id].insert("role",roleName(value)); }
        else if(!transport_.setRole(id,Role(value)))return;
        inputs_.remove(id);sendInput();updateWaiting();refreshLobby();
    });
    connect(lobby_,&NetplayLobby::kickRequested,this,[this](const QString& id){if(hosting_)transport_.kick(id.toULongLong(),false,false);});
    connect(lobby_,&NetplayLobby::banRequested,this,[this](const QString& id,bool permanent){if(hosting_)transport_.kick(id.toULongLong(),permanent,true);});
    connect(lobby_,&NetplayLobby::allowCommandsRequested,this,[this](bool yes){if(hosting_){
        allowCommands_=yes;
        if(!yes)for(auto id:participants_.keys())if(id)releasePeerCommands(id);
        refreshLobby();
    }});
    connect(lobby_,&NetplayLobby::allowDownloadsRequested,this,[this](bool yes){
        if(!hosting_)return;
        allowDownloads_=yes;
        QMetaObject::invokeMethod(files_,[this,yes]{files_->setSharingEnabled(yes);});announceContent();refreshLobby();
    });
    connect(lobby_,&NetplayLobby::linkCableRequested,this,[this](bool enabled){
        if(active_&&hosting_)applyLinkCableConfiguration(enabled);
    });
    connect(&session_,&EmulationSession::controllerTopologyChanged,this,[this](int adapter,unsigned count){
        if(!active_ || hosting_)setControllerTopology(adapter,count);
    });
    connect(&session_,&EmulationSession::networkLinkCableChanged,this,[this](bool enabled){
        if(!active_||!hosting_||linkCable_==enabled)return;
        // A failed hardware change reports the mode that remains available.
        // Reflect it once; this is not another request to the emulation thread.
        linkCable_=enabled;++linkCableRevision_;normalizeRoles();announceContent();refreshLobby();
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
    connect(files_,&NetTransferWorker::progress,this,[this](qint64 done,qint64 total){if(!active_)return;transferDone_=done;transferTotal_=total;refreshLobby();});
    connect(files_,&NetTransferWorker::completed,this,[this](const QString& path){
        if(!active_||hosting_||!downloading_)return;
        downloading_=false;downloadedPath_=path;localManifest_=manifest_;emit openRomRequested(path);refreshLobby();
    });
    connect(files_,&NetTransferWorker::requestFailed,this,[this](quint64 id,const QString& error){
        filePending_.remove(id);if(hosting_)transport_.send(id,MessageKind::Control,json({{"op","file-error"},{"text",error.left(1024)}}));
    });
    connect(files_,&NetTransferWorker::failed,this,[this](const QString& e){if(!active_)return;downloading_=false;status_=e;emit notice(e);refreshLobby();});
    connect(&transport_,&NetTransport::peerLatency,this,[this](quint64 id,int ms){
        if(participants_.contains(id)){participants_[id].insert("pingMs",ms);refreshLobby();}
    });
    connect(&transport_,&NetTransport::messageReceived,this,&NetplayController::receive);
    connect(&transport_,&NetTransport::connected,this,[this](const PeerInfo& self){
        if(!active_||hosting_)return;
        localId_=self.id;localRole_=int(self.role);session_.setNetworkRole(localRole_);session_.setNetworkPeerId(localId_);
        reconnectTimer_.stop();reconnectAttempts_=0;connectionLost_=false;status_=t("Подключено к хосту");
        compareContent();
        refreshLobby();
    });
    connect(&transport_,&NetTransport::peerJoined,this,[this](const PeerInfo& peer){
        if(!active_)return;
        participants_.insert(peer.id,QJsonObject{{"id",QString::number(peer.id)},{"nickname",peer.nickname},
            {"role",roleName(int(peer.role))},{"readiness","rom-missing"},{"local",peer.id==localId_},{"host",false},{"pingMs",-1}});
        disconnectedAt_.remove(peer.id);
        if(hosting_ && int(peer.role)>activePlayerCount())transport_.setRole(peer.id,Role::Spectator);
        if(hosting_) {
            const auto key=NetTransport::generateSecret();
            if(!datagrams_.addPeer(peer.id,key)){transport_.kick(peer.id,false,false);return;}
            transport_.send(peer.id,MessageKind::Control,json({{"op","udp"},{"port",int(datagrams_.localPort())},{"key",QString::fromLatin1(key.toBase64())}}));
            announceContent();updateRoster();updateWaiting();
        }
        refreshLobby();
    });
    connect(&transport_,&NetTransport::peerRoleChanged,this,[this](quint64 id,Role role){
        if(!hosting_ && int(role)>activePlayerCount())return;
        if(participants_.contains(id))participants_[id].insert("role",roleName(int(role)));
        if(hosting_&&role==Role::Spectator){
            releasePeerCommands(id);
            if(disconnectedAt_.remove(id))participants_.remove(id);
        }
        if(id==localId_){localRole_=int(role);session_.setNetworkRole(localRole_);}
        inputs_.remove(id);if(hosting_ && !adjustingRoles_){updateRoster();updateWaiting();}if(!adjustingRoles_)refreshLobby();
    });
    connect(&transport_,&NetTransport::sessionEnded,this,[this](const QString& cause,const QString& detail){
        if(!active_ || hosting_ || stopping_)return;
        QString message;
        if(cause==QLatin1String("session-ban"))message=t("Хост запретил вам вход до конца этого сеанса.");
        else if(cause==QLatin1String("permanent-ban"))message=t("Хост заблокировал ваши дальнейшие подключения.");
        else if(cause==QLatin1String("host-stopped"))message=t("Хост закрыл сеанс.");
        else if(cause==QLatin1String("rejected"))message=t("Подключение отклонено: %1").arg(Localization::text(detail));
        else message=t("Хост отключил вас от сеанса.");
        // Stop the worker timeline, transfers and retry timers before notifying
        // the UI. A later socket/rollback callback must not revive this session.
        leaveSession();
        emit sessionEnded(message);
    });
    connect(&transport_,&NetTransport::peerLeft,this,[this](quint64 id,const QString& reason){
        if(!active_||stopping_)return;
        if(hosting_) {
            releasePeerCommands(id);datagrams_.removePeer(id);udpReadyPeers_.remove(id);
            inputs_.remove(id);readyPeers_.remove(id);outgoingSnapshots_.remove(id);outgoingOffsets_.remove(id);heldFrames_.remove(id);
            const bool removed=reason==QStringLiteral("Left session")||reason.contains("kick",Qt::CaseInsensitive)||reason.contains("ban",Qt::CaseInsensitive);
            if(removed||roleValue(participants_.value(id).value("role").toString())==0) {
                transport_.setRole(id,Role::Spectator);participants_.remove(id);disconnectedAt_.remove(id);
            } else {
                if(participants_.contains(id))participants_[id].insert("readiness","reconnecting");
                disconnectedAt_.insert(id,QDateTime::currentMSecsSinceEpoch());
            }
            updateWaiting();sendInput();
        } else if(id==0) {
            // Explicit kick/ban/stop arrives through sessionEnded above. Only
            // an unexpected transport loss reaches this reconnect path.
            gameReady_=false;session_.setNetworkWaiting(true);status_=t("Связь потеряна. Переподключение…");
            connectionLost_=true;reconnectAttempts_=0;reconnectTimer_.start();
        }
        refreshLobby();
    });
    connect(&transport_,&NetTransport::transportError,this,[this](const QString& error){if(!active_||stopping_)return;status_=error;if(!connectionLost_)emit notice(error);refreshLobby();});
    connect(&session_,&EmulationSession::networkModeFailed,this,[this](const QString& error){if(!active_||stopping_)return;leaveSession();emit notice(error);});
    connect(&session_,&EmulationSession::networkCommandRequested,this,[this](const QByteArray& b){
        if(!active_)return;
        if(hosting_)acceptCommand(0,b);
        else if((localRole_&&allowCommands_&&gameReady_)||EmulationSession::isNetworkMomentaryRelease(b))transport_.send(0,MessageKind::Command,b);
        else if(!EmulationSession::isNetworkMomentaryRelease(b))
            emit notice(t("Это действие доступно хосту и назначенным игрокам."));
    });
    connect(&session_,&EmulationSession::networkSnapshotReady,this,[this](const QByteArray& state){
        session_.acknowledgeNetworkSnapshot();
        if(!active_||!hosting_)return;
        snapshotRequested_=false;
        const auto metadata=snapshotMetadata(state);
        if(metadata.isEmpty()||metadata.value("configuration").toObject().value("linkCable").toBool()!=linkCable_)return;
        const auto kinds=roleKindsFromConfiguration(metadata.value("configuration").toObject());
        if(kinds!=roleKinds_){roleKinds_=kinds;announceContent();}
        hostEpoch_=metadata.value("epoch").toString().toULongLong();
        readyPeers_.clear();outgoingSnapshots_.clear();outgoingOffsets_.clear();heldFrames_.clear();
        for(auto it=participants_.begin();it!=participants_.end();++it)if(it.key()&&it->value("matched").toBool()&&udpReadyPeers_.contains(it.key())&&!disconnectedAt_.contains(it.key())) {
            it->insert("readiness","syncing");queueSnapshot(it.key(),state);
        }
        updateWaiting();refreshLobby();
    });
    connect(&session_,&EmulationSession::networkSnapshotApplied,this,[this](bool ok,const QString& error){
        if(!active_||hosting_||!applyingEpoch_)return;
        const bool obsolete=applyingLinkCableRevision_!=linkCableRevision_;
        gameReady_=ok&&!obsolete;
        if(obsolete)session_.setNetworkWaiting(true);
        else if(ok){status_=t("Игра синхронизирована");transport_.send(0,MessageKind::Control,json({{"op","snapshot-ready"},{"epoch",QString::number(applyingEpoch_)}}));}
        else {status_=error;emit notice(error);transport_.send(0,MessageKind::Control,json({{"op","sync-failed"}}));}
        applyingEpoch_=0;
        if(pendingIncomingEpoch_) {const auto size=pendingIncomingSize_;const auto epoch=pendingIncomingEpoch_;pendingIncomingEpoch_=0;pendingIncomingSize_=0;beginIncomingSnapshot(size,epoch);}
        else if(obsolete)transport_.send(0,MessageKind::Control,json({{"op","resync"}}));
        refreshLobby();
    });
    connect(&session_,&EmulationSession::networkSnapshotDiscarded,this,[this]{
        if(!active_||hosting_||!applyingEpoch_)return;
        applyingEpoch_=0;gameReady_=false;session_.setNetworkWaiting(true);
        if(pendingIncomingEpoch_) {const auto size=pendingIncomingSize_;const auto epoch=pendingIncomingEpoch_;pendingIncomingEpoch_=0;pendingIncomingSize_=0;beginIncomingSnapshot(size,epoch);}
        else transport_.send(0,MessageKind::Control,json({{"op","resync"}}));
        refreshLobby();
    });
    connect(&session_,&EmulationSession::networkResyncRequested,this,[this]{
        if(active_&&hosting_){session_.requestNetworkSnapshot();return;}
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
        networkQuality_={};lobby_->setNetworkQuality({});
        gameReady_=false;gamePath_=path;sourceArchiveIndex_=index;files_->requestCancel();
        if(hosting_)roleKinds_ = {};
        // Preserve verification only for the explicitly completed download.
        if(path!=downloadedPath_)localManifest_.clear();
        incomingSnapshot_.clear();incomingSize_=0;pendingIncomingEpoch_=0;
        if(hosting_){manifest_.clear();readyPeers_.clear();for(auto& p:participants_)p.insert("matched",false);announceContent();}
        session_.setNetworkWaiting(!hosting_);
    });
    connect(&session_,&EmulationSession::networkGameSource,this,[this](const QString& path,int index){if(active_)prepareSource(path,index);});
    connect(&session_,&EmulationSession::gameLoaded,this,[this](const QString& path,double,const QString& name){gamePath_=path;gameName_=name;onGameLoaded(path);});
    connect(&session_,&EmulationSession::gameClosed,this,[this]{
        networkQuality_={};lobby_->setNetworkQuality({});
        gamePath_.clear();gameName_.clear();localManifest_.clear();gameReady_=false;roleKinds_ = {};
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
void NetplayController::createSession(){
    if(active_){showLobby();return;}
    connections_={};
    for(const auto& c:NetInterfaces::available())connections_.append(QJsonObject{{"id",c.id},{"name",c.name},{"ipv4",c.ipv4},{"physical",c.physical}});
    refreshLobby();lobby_->showCreateDialog();
}
void NetplayController::joinSession(){if(active_){showLobby();return;}lobby_->showJoinDialog();}
void NetplayController::showLobby(){refreshLobby();lobby_->setNetworkQuality(networkQuality_);lobby_->show();lobby_->raise();lobby_->activateWindow();}
void NetplayController::setHostAddress(const QString& address){
    invite_=NetTransport::makeInvitation(address,transport_.localPort(),invitationKey_);
    inviteWithPassword_=invite_;
    if(!invite_.isEmpty()&&!invitationPassword_.isEmpty()) {
        QUrl u(invite_);QUrlQuery q(u);q.addQueryItem("password",invitationPassword_);u.setQuery(q);inviteWithPassword_=u.toString(QUrl::FullyEncoded);
    }
}
void NetplayController::resolveHostAddress(){
    if(!active_||!hosting_||!automaticAddress_||addressPending_)return;
    addressPending_=true;addressRetry_=false;addressStatus_=t("Определение внешнего адреса…");
    invite_.clear();inviteWithPassword_.clear();refreshLobby();addressDiscovery_->cancel();addressDiscovery_->start(connectionId_);
}
void NetplayController::startHost(quint16 port,const QString& address,const QString& password,const QString& connectionId){
    if(active_)return;
    HostOptions options;options.port=port;options.inviteSecret=NetTransport::generateSecret();options.password=password;
    options.banFilePath=QFileInfo(settings_.fileName()).absolutePath()+"/netplay-bans.json";
    QString error;if(!transport_.host(options,&error)){emit notice(error);QMessageBox::warning(lobby_,t("Онлайн"),error);return;}
    if(!datagrams_.host(transport_.localPort(),&error)) {
        transport_.stop();emit notice(error);QMessageBox::warning(lobby_,t("Онлайн"),error);return;
    }
    rosterBytes_.clear();lastLobbyBytes_.clear();udpReadyPeers_.clear();
    active_=hosting_=true;localId_=0;localRole_=1;allowCommands_=true;allowDownloads_=true;linkCable_=false;roleKinds_ = {};
    controllerAdapter_=session_.controllerAdapter();
    playerCount_=int(session_.controllerPlayerCount());
    if(!validControllerAdapter(controllerAdapter_) || playerCount_<2 || playerCount_>int(MaxControllerPlayers)) {controllerAdapter_=0;playerCount_=2;}
    QMetaObject::invokeMethod(files_,[this]{files_->setSharingEnabled(true);});
    storageId_=QString::fromLatin1(NetTransport::generateSecret().left(16).toHex());
    invitationKey_=options.inviteSecret;invitationPassword_=password;connectionId_=connectionId;
    lanInvite_.clear();
    const auto connection=NetInterfaces::choose(connectionId_);
    if(!connection.ipv4.isEmpty()) {setHostAddress(connection.ipv4);lanInvite_=inviteWithPassword_;}
    automaticAddress_=address.trimmed().isEmpty();addressPending_=false;addressRetry_=false;
    addressStatus_.clear();invite_.clear();inviteWithPassword_.clear();
    if(!automaticAddress_)setHostAddress(address);
    participants_.clear();participants_.insert(0,QJsonObject{{"id","0"},{"nickname",nickname()},{"role","player1"},{"host",true},{"local",true},{"readiness","waiting"},{"pingMs",0}});
    session_.setNetworkPeerId(0);session_.setNetworkRole(1);session_.setNetworkMode(1,storageId_);session_.setNetworkWaiting(false);
    inputTimer_.start();status_=t("Лобби создано.");
    emit activeChanged(true);showLobby();sendInput();
    if(automaticAddress_)resolveHostAddress();
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
    active_=true;hosting_=false;remoteGameOpen_=false;localRole_=0;localId_=0;allowDownloads_=false;gameReady_=false;linkCable_=false;
    playerCount_=2;controllerAdapter_=0;roleKinds_ = {};
    storageId_=QString::fromLatin1(options.inviteSecret.left(16).toHex());
    session_.setNetworkMode(2,storageId_);session_.setNetworkRole(0);
    inputTimer_.start();status_=t("Подключение…");emit activeChanged(true);showLobby();
}
void NetplayController::leaveSession(){
    if(!active_)return;
    networkQuality_={};lobby_->setNetworkQuality({});
    roleKinds_ = {};
    stopping_=true;active_=false;connectionLost_=remoteConnectionWaiting_=remoteReadinessWaiting_=false;
    addressPending_=false;automaticAddress_=false;addressRetry_=false;addressDiscovery_->cancel();
    invitationKey_.clear();invitationPassword_.clear();lanInvite_.clear();addressStatus_.clear();invite_.clear();inviteWithPassword_.clear();
    inputTimer_.stop();reconnectTimer_.stop();snapshotTimer_.stop();
    datagrams_.stop();udpReadyPeers_.clear();rosterBytes_.clear();lastLobbyBytes_.clear();heldCommandOwners_.clear();
    transport_.stop();session_.setNetworkMode(0);session_.setNetworkWaiting(false);
    files_->requestCancel();QMetaObject::invokeMethod(files_,&NetTransferWorker::cancel);
    QMetaObject::invokeMethod(files_,[this]{files_->setSharingEnabled(false);});
    participants_.clear();inputs_.clear();readyPeers_.clear();filePending_.clear();outgoingSnapshots_.clear();outgoingOffsets_.clear();heldFrames_.clear();disconnectedAt_.clear();
    incomingSnapshot_.clear();manifest_.clear();localManifest_.clear();transferDone_=transferTotal_=0;
    hosting_=false;linkCable_=false;gameReady_=false;downloading_=false;snapshotRequested_=false;applyingEpoch_=incomingEpoch_=pendingIncomingEpoch_=0;linkCableRevision_=applyingLinkCableRevision_=0;incomingSize_=pendingIncomingSize_=0;fileChunkExpected_=false;stopping_=false;
    status_=t("Онлайн-сессия завершена");emit activeChanged(false);refreshLobby();lobby_->hide();
}
void NetplayController::onGameLoaded(const QString&){if(active_){gameReady_=hosting_;refreshLobby();}}
void NetplayController::prepareSource(const QString& path,int archiveIndex){
    files_->requestCancel();gamePath_=path;sourceArchiveIndex_=archiveIndex;
    if(!hosting_&&path==downloadedPath_&&localManifest_==manifest_&&!manifest_.isEmpty()) {downloadedPath_.clear();compareContent();return;}
    localManifest_.clear();
    if(hosting_){
        manifest_.clear();readyPeers_.clear();outgoingSnapshots_.clear();outgoingOffsets_.clear();
        for(auto it=participants_.begin();it!=participants_.end();++it)if(it.key())it->insert("matched",false);
        updateRoster();announceContent();
    }
    const auto source=path;QMetaObject::invokeMethod(files_,[this,source,archiveIndex]{files_->prepare(source,archiveIndex);});
    status_=t("Проверка файлов игры…");refreshLobby();
}
void NetplayController::applyLinkCableConfiguration(bool enabled){
    if(!active_||linkCable_==enabled)return;
    if(hosting_ && enabled && controllerAdapter_!=0) {
        emit notice(t("Отключите многопортовый адаптер перед включением Link Cable."));refreshLobby();return;
    }
    linkCable_=enabled;++linkCableRevision_;
    if(hosting_)normalizeRoles();
    // A hardware transition starts a new GGPO timeline. Drop transfers for the
    // previous configuration rather than retaining another pair's full state.
    readyPeers_.clear();outgoingSnapshots_.clear();outgoingOffsets_.clear();heldFrames_.clear();
    incomingSnapshot_.clear();incomingSize_=0;pendingIncomingSize_=0;pendingIncomingEpoch_=0;
    snapshotRequested_=false;
    if(hosting_){
        for(auto it=participants_.begin();it!=participants_.end();++it)
            if(it.key()&&it->value("matched").toBool())it->insert("readiness","syncing");
        // Send the expected configuration before the worker publishes its state.
        announceContent();refreshLobby();
    } else {gameReady_=false;session_.setNetworkWaiting(true);}
    session_.setNetworkLinkCableEnabled(enabled);
}
void NetplayController::announceContent(){
    if(!hosting_)return;
    const QJsonObject o{{"name",gameName_},{"open",!gamePath_.isEmpty()},{"manifest",QString::fromLatin1(manifest_.toBase64())},{"downloads",allowDownloads_},{"linkCable",linkCable_},{"playerCount",activePlayerCount()},{"roleKinds",roleKinds_}};
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
    bool wait=!outgoingSnapshots_.isEmpty();const auto now=QDateTime::currentMSecsSinceEpoch();QList<quint64> expired;
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
    if(!expired.isEmpty() || participantReadinessWaiting()!=readinessWaiting_)refreshLobby();
}
bool NetplayController::participantReadinessWaiting() const {
    if(!active_)return false;
    if(!hosting_)return remoteReadinessWaiting_;
    if(gamePath_.isEmpty())return false;
    for(auto it=outgoingSnapshots_.cbegin();it!=outgoingSnapshots_.cend();++it)
        if(!disconnectedAt_.contains(it.key()))return true;
    for(auto it=participants_.cbegin();it!=participants_.cend();++it)
        if(it.key() && roleValue(it->value("role").toString()) &&
            !readyPeers_.contains(it.key()) && !disconnectedAt_.contains(it.key()))return true;
    return false;
}
void NetplayController::stopWaiting(){
    if(!active_ || !hosting_ || disconnectedAt_.isEmpty())return;
    const auto peers=disconnectedAt_.keys();
    disconnectedAt_.clear();
    for(auto id:peers)participants_.remove(id);
    for(auto id:peers)transport_.setRole(id,Role::Spectator);
    sendInput();refreshLobby();
}
void NetplayController::updateRoster(){
    if(!active_||!hosting_)return;
    QJsonArray roster;
    auto ids=participants_.keys();std::sort(ids.begin(),ids.end());
    for(auto id:ids) {
        const auto participant=participants_.value(id);
        if(id && (!participant.value("matched").toBool() || !udpReadyPeers_.contains(id) || disconnectedAt_.contains(id)))continue;
        roster.append(QJsonObject{{"id",QString::number(id)},{"role",roleValue(participant.value("role").toString())}});
    }
    const auto bytes=QJsonDocument(roster).toJson(QJsonDocument::Compact);
    if(!roster.isEmpty() && bytes!=rosterBytes_) {rosterBytes_=bytes;session_.setNetworkRoster(bytes);}
}
void NetplayController::sendInput(){
    if(!active_||!hosting_)return;
    updateRoster();updateWaiting();
}
void NetplayController::releasePeerCommands(quint64 peer){
    if(!hosting_)return;
    for(auto it=heldCommandOwners_.begin();it!=heldCommandOwners_.end();++it)
        if(it.value().remove(peer)&&it.value().isEmpty())session_.applyNetworkCommand(json({{"type",it.key()},{"value",0}}));
}
void NetplayController::acceptCommand(quint64 peer,const QByteArray& b){
    if(!hosting_||b.size()>16*1024)return;
    const auto command=QJsonDocument::fromJson(b).object();
    const bool momentary=EmulationSession::isNetworkMomentaryCommand(b);
    const int type=command.value("type").toInt(-1);
    if(momentary&&command.value("value").toInt()==0) {
        auto it=heldCommandOwners_.find(type);
        if(it!=heldCommandOwners_.end()&&it.value().remove(peer)&&it.value().isEmpty())session_.applyNetworkCommand(b);
        return;
    }
    if(peer&&(!allowCommands_||!readyPeers_.contains(peer)||roleValue(participants_.value(peer).value("role").toString())==0))return;
    const auto now=QDateTime::currentMSecsSinceEpoch();
    if(peer && !momentary && now-commandTimes_.value(peer)<25 && command.value("value").toInt()!=0)return;
    commandTimes_.insert(peer,now);
    if(momentary) {
        auto& owners=heldCommandOwners_[type];
        if(owners.contains(peer))return;
        const bool wasEmpty=owners.isEmpty();owners.insert(peer);
        if(!wasEmpty)return;
    }
    session_.applyNetworkCommand(b);
}
void NetplayController::receive(quint64 peer,MessageKind kind,const QByteArray& b){
    if(!active_)return;
    if(kind==MessageKind::Input)return; // Gameplay input uses GGPO datagrams.
    if(kind==MessageKind::Command&&hosting_){acceptCommand(peer,b);return;}
    if(kind==MessageKind::FileRequest&&hosting_&&allowDownloads_){if(filePending_.contains(peer))return;filePending_.insert(peer);QMetaObject::invokeMethod(files_,[this,peer,b]{files_->requestChunk(peer,b);});return;}
    if(kind==MessageKind::FileChunk&&!hosting_&&downloading_){if(!fileChunkExpected_)return;fileChunkExpected_=false;QMetaObject::invokeMethod(files_,[this,b]{files_->receiveChunk(b);});return;}
    if(kind==MessageKind::Frame){if(!hosting_)session_.receiveNetworkRewindPreview(b);return;}
    if(kind==MessageKind::Content&&!hosting_){
        const auto o=QJsonDocument::fromJson(b).object();
        const int count=o.value("playerCount").toInt(-1);if(count<2 || count>int(MaxControllerPlayers))return;
        const auto kinds=o.value("roleKinds").toArray();if(!validRoleKinds(kinds))return;roleKinds_=kinds;
        gameName_=o.value("name").toString().left(512);allowDownloads_=o.value("downloads").toBool();
        applyLinkCableConfiguration(o.value("linkCable").toBool());playerCount_=count;
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
            if(incomingSnapshot_.size()==incomingSize_){
                const auto metadata=snapshotMetadata(incomingSnapshot_);
                if(!metadata.isEmpty()&&metadata.value("configuration").toObject().value("linkCable").toBool()==linkCable_ &&
                   validSnapshotRoster(metadata,localId_)){
                    applyingEpoch_=incomingEpoch_;applyingLinkCableRevision_=linkCableRevision_;session_.receiveNetworkSnapshot(incomingSnapshot_);
                } else transport_.send(0,MessageKind::Control,json({{"op","resync"}}));
                incomingSnapshot_.clear();incomingSize_=0;
            } else transport_.send(0,MessageKind::Control,json({{"op","snapshot-offset"},{"offset",incomingSnapshot_.size()},{"epoch",QString::number(epoch)}}));
        }
        return;
    }
    if(kind!=MessageKind::Control)return;
    const auto o=QJsonDocument::fromJson(b).object();const auto op=o.value("op").toString();
    if(hosting_){
        if(op=="content-match"){
            const bool match=!manifest_.isEmpty()&&o.value("id").toString()==manifestId(manifest_);
            participants_[peer].insert("matched",match);participants_[peer].insert("readiness",match?"syncing":"rom-mismatch");
            updateRoster();if(match&&udpReadyPeers_.contains(peer))sendState(peer);else readyPeers_.remove(peer);updateWaiting();refreshLobby();
        } else if(op=="snapshot-offset"){
            if(!outgoingSnapshots_.contains(peer)||o.value("epoch").toString().toULongLong()!=hostEpoch_)return;
            const qsizetype offset=o.value("offset").toInteger(-1);const auto& state=outgoingSnapshots_[peer];
            if(offset<0||offset!=outgoingOffsets_.value(peer)||offset>=state.size())return;
            const auto size=std::min<qsizetype>(64*1024,state.size()-offset);QByteArray chunk(1,char(1));append32(chunk,quint32(offset));append64(chunk,hostEpoch_);chunk.append(state.constData()+offset,size);
            outgoingOffsets_[peer]=offset+size;transport_.send(peer,MessageKind::Snapshot,chunk);
        } else if(op=="snapshot-ready"){
            if(!outgoingSnapshots_.contains(peer)||o.value("epoch").toString().toULongLong()!=hostEpoch_||outgoingOffsets_.value(peer)!=outgoingSnapshots_[peer].size())return;
            outgoingSnapshots_.remove(peer);outgoingOffsets_.remove(peer);readyPeers_.insert(peer);participants_[peer].insert("readiness","ready");
            heldFrames_.remove(peer);
            updateWaiting();refreshLobby();
        } else if(op=="resync"){
            if(participants_.value(peer).value("matched").toBool())sendState(peer);
        } else if(op=="sync-failed"){
            outgoingSnapshots_.remove(peer);heldFrames_.remove(peer);readyPeers_.remove(peer);participants_[peer].insert("readiness","rom-mismatch");
            transport_.setRole(peer,Role::Spectator);updateWaiting();refreshLobby();
        }
    } else if(op=="udp") {
        const auto key=QByteArray::fromBase64(o.value("key").toString().toLatin1());
        const int port=o.value("port").toInt();QString error;
        if(port<1||port>65535||key.size()!=32||!localId_)return;
        if(!datagrams_.join(transport_.peerAddress(0),quint16(port),localId_,key,&error)) {
            status_=error;emit notice(error);refreshLobby();
        }
    } else if(op=="step") {
        if(gameReady_&&o.value("epoch").toString().toULongLong()==incomingEpoch_)session_.advanceNetworkFrame();
    } else if(op=="slots") {session_.receiveNetworkSlots(unsigned(o.value("mask").toInt()));
    } else if(op=="file-error") {downloading_=false;files_->requestCancel();status_=o.value("text").toString().left(1024);emit notice(status_);refreshLobby();
    } else if(op=="notice") {emit notice(o.value("text").toString().left(1024));
    } else if(op=="lobby"){
        const int count=o.value("playerCount").toInt(-1);
        if(count<2 || count>int(MaxControllerPlayers))return;
        const auto kinds=o.value("roleKinds").toArray();if(!validRoleKinds(kinds))return;
        const int available=o.value("linkCable").toBool()?2:count;
        const auto array=o.value("participants").toArray();if(array.size()>32)return;
        QSet<quint64> ids;
        for(const auto& value:array) {
            const auto participant=value.toObject();
            bool ok=false;const quint64 id=participant.value("id").toString().toULongLong(&ok);
            const auto role=participant.value("role").toString();
            const int roleNumber=roleValue(role);
            if(!ok || ids.contains(id) || role!=roleName(roleNumber) || roleNumber>available)return;
            ids.insert(id);
        }
        remoteConnectionWaiting_=o.value("connectionWaiting").toBool();
        remoteReadinessWaiting_=o.value("readinessWaiting").toBool();
        allowCommands_=o.value("allowCommands").toBool();
        applyLinkCableConfiguration(o.value("linkCable").toBool());
        playerCount_=count;roleKinds_=kinds;
        participants_.clear();for(const auto& value:array){auto participant=value.toObject();const auto id=participant.value("id").toString().toULongLong();participant.insert("local",id==localId_);participants_.insert(id,participant);}
        localRole_=roleValue(participants_.value(localId_).value("role").toString());session_.setNetworkRole(localRole_);refreshLobby();
    }
}
void NetplayController::refreshLobby(){
    const bool waiting=active_ && (hosting_?!disconnectedAt_.isEmpty():connectionLost_||remoteConnectionWaiting_);
    if(waiting!=connectionWaiting_){connectionWaiting_=waiting;emit connectionWaitingChanged(waiting);}
    const bool readinessWaiting=participantReadinessWaiting();
    if(readinessWaiting!=readinessWaiting_){readinessWaiting_=readinessWaiting;emit readinessWaitingChanged(readinessWaiting);}
    QJsonArray list;auto ids=participants_.keys();std::sort(ids.begin(),ids.end());for(auto id:ids)list.append(participants_.value(id));
    if(hosting_) {
        const auto bytes=json({{"op","lobby"},{"participants",list},{"allowCommands",allowCommands_},{"linkCable",linkCable_},{"playerCount",activePlayerCount()},{"roleKinds",roleKinds_},{"connectionWaiting",waiting},{"readinessWaiting",readinessWaiting}});
        if(bytes!=lastLobbyBytes_){lastLobbyBytes_=bytes;transport_.broadcast(MessageKind::Control,bytes);}
    }
    QString romStatus=gameReady_?QStringLiteral("matched"):localManifest_.isEmpty()?QStringLiteral("missing"):QStringLiteral("verifying");
    if(!hosting_&&!localManifest_.isEmpty()&&!manifest_.isEmpty()&&manifestId(localManifest_)!=manifestId(manifest_))romStatus="mismatch";
    if(downloading_)romStatus="downloading";
    QJsonObject rom{{"filename",gameName_},{"status",romStatus},{"progress",transferTotal_?double(transferDone_)*100.0/double(transferTotal_):0},
        {"canSelect",active_},{"canDownload",active_&&!hosting_&&allowDownloads_&&!manifest_.isEmpty()&&!downloading_},{"canCancel",downloading_}};
    lobby_->setLobbyState({{"active",active_},{"host",hosting_},{"connectionWaiting",waiting},{"status",status_},{"invite",invite_},{"inviteWithPassword",inviteWithPassword_},{"lanInvite",lanInvite_},{"addressStatus",addressStatus_},{"addressRetry",addressRetry_},{"connections",connections_},{"selectedConnectionId",connectionId_},
        {"participants",list},{"rom",rom},{"allowCommands",allowCommands_},{"allowDownloads",allowDownloads_},{"linkCable",linkCable_},{"playerCount",activePlayerCount()},{"roleKinds",roleKinds_}});
}

int NetplayController::activePlayerCount() const {
    return linkCable_ ? 2 : playerCount_;
}
void NetplayController::normalizeRoles() {
    if(!active_ || !hosting_)return;
    QList<quint64> invalid;
    for(auto it=participants_.cbegin();it!=participants_.cend();++it)
        if(roleValue(it->value("role").toString())>activePlayerCount())invalid.append(it.key());
    if(invalid.isEmpty()) {refreshLobby();return;}
    adjustingRoles_=true;
    for(const quint64 id:invalid) {
        releasePeerCommands(id);
        participants_[id].insert("role","spectator");
        if(id==0) {localRole_=0;session_.setNetworkRole(0);}
        else transport_.setRole(id,Role::Spectator);
        inputs_.remove(id);
    }
    adjustingRoles_=false;
    updateRoster();updateWaiting();refreshLobby();
}
void NetplayController::setControllerTopology(int adapter,unsigned count) {
    if(active_ && !hosting_)return;
    if(!validControllerAdapter(adapter) || count<2 || count>MaxControllerPlayers)return;
    if(controllerAdapter_==adapter && playerCount_==int(count))return;
    if(controllerAdapter_!=adapter)roleKinds_ = {};
    controllerAdapter_=adapter;playerCount_=int(count);
    if(active_ && hosting_) {normalizeRoles();announceContent();}
}
