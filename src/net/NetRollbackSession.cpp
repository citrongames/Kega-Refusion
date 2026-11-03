#include "NetRollbackSession.h"
#include "NetplayTimeline.h"
#include "NetDelayPolicy.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include "core/LibretroHost.h"
#include <ggponet.h>
#include <kega_ggpo_transport.h>
#include <QHostAddress>
#include <QJsonObject>
#include <QVector>
#include <QtEndian>
#include <array>
#include <deque>
#include <stdexcept>
#include <cstring>

namespace {
constexpr int InputSize = 7;
constexpr qsizetype StateLimit = 64 * 1024 * 1024;
constexpr qsizetype PacketLimit = 2 * 1024 * 1024;
std::array<char,InputSize> encodeInput(const MovieInput& input) {
    std::array<char,InputSize> bytes{};
    qToBigEndian(input.buttons[0], bytes.data());
    qToBigEndian(quint32(input.aim[0]), bytes.data() + 2);
    bytes[6] = char((input.aim[0] >> 32) & 1);
    return bytes;
}
MovieInput decodeInput(const char* data) {
    MovieInput input;
    input.aim.fill(quint64(1) << 32);
    input.buttons[0] = qFromBigEndian<quint16>(data);
    input.aim[0] = qFromBigEndian<quint32>(data + 2) | (quint64(quint8(data[6]) & 1) << 32);
    return input;
}
}
struct NetRollbackSession::Impl {
    struct Peer { quint64 id; int role; quint32 address; };
    struct Packet { quint32 address; QByteArray data; };
    struct RemoteQuality {
        quint64 id; GGPOPlayerHandle handle; unsigned serial=0; NetRttEstimate estimate;
    };
    LibretroHost* host = nullptr;
    GGPOSession* session = nullptr;
    GGPOPlayerHandle localHandle = -1;
    int localRole=0;
    quint64 epoch = 0, localId = 0, frame = 0;
    QVector<Peer> peers, players;
    QList<QPair<int,MovieInput>> contributions;
    std::deque<Packet> incoming;
    qsizetype queuedBytes = 0, stateBytes = 0;
    std::array<QByteArray, GGPO_MAX_PREDICTION_FRAMES + 2> states;
    std::array<int, GGPO_MAX_PREDICTION_FRAMES + 2> stateFrames{};
    std::array<QImage, GGPO_MAX_PREDICTION_FRAMES + 2> images, secondImages;
    std::array<double, GGPO_MAX_PREDICTION_FRAMES + 2> aspects{}, secondAspects{};
    bool historyEnabled=false;
    int rollbackPosition=0,lastHistoryFrame=0;
    Send send;
    bool running = false, localQueued = false;
    QVector<RemoteQuality> remoteQuality;
    QElapsedTimer qualityClock;
    qint64 lastQualityMs=0;
    quint64 qualityFrame=0;
    int rollbackFrames=0,rollbackEvents=0,maxRollbackFrames=0,rollbackGroupFrames=0;
    bool realtime=false,havePreviousInput=false;
    std::array<char,InputSize> previousInput{};
    NetDelayPolicy delay;
    QString failure;
    static thread_local Impl* current;
    struct Scope { Impl* previous; Scope(Impl* value): previous(current) { current=value; } ~Scope(){ current=previous; } };

    static bool begin(const char*) { return true; }
    static bool save(unsigned char** buffer,int* length,int* checksum,int frame) {
        auto& self=*current;
        auto state=self.host->captureMovieState(false);
        if(state.isEmpty() || self.stateBytes+state.size()>StateLimit)
            throw std::runtime_error("GGPO rollback state exceeds memory budget");
        for(size_t i=0;i<self.states.size();++i) if(self.states[i].isEmpty()) {
            auto& slot=self.states[i];slot=std::move(state);self.stateBytes+=slot.size();
            self.stateFrames[i]=frame;
            if(self.historyEnabled && self.localId==0 && frame%2==0) {
                self.images[i]=self.host->frameForLinkRole(1);self.aspects[i]=self.host->aspectRatioForLinkRole(1);
                if(self.host->linkCableEnabled()) {
                    self.secondImages[i]=self.host->frameForLinkRole(2);
                    self.secondAspects[i]=self.host->aspectRatioForLinkRole(2);
                }
                self.stateBytes+=self.images[i].sizeInBytes()+self.secondImages[i].sizeInBytes();
                if(self.stateBytes>StateLimit)throw std::runtime_error("GGPO rewind previews exceed memory budget");
            }
            *buffer=reinterpret_cast<unsigned char*>(slot.data());*length=int(slot.size());*checksum=0;
            return true;
        }
        throw std::runtime_error("GGPO state pool exhausted");
    }
    static bool load(unsigned char* buffer,int length) {
        auto& self=*current;QString error;
        for(size_t i=0;i<self.states.size();++i)if(self.states[i].constData()==reinterpret_cast<char*>(buffer))self.rollbackPosition=self.stateFrames[i];
        if(!self.host->restoreMovieState(QByteArray::fromRawData(reinterpret_cast<char*>(buffer),length),error,true))
            throw std::runtime_error(error.toStdString());
        ++self.rollbackEvents;
        self.rollbackGroupFrames=0;
        return true;
    }
    static bool log(char*,unsigned char*,int) { return true; }
    static void free(void* buffer) {
        for(size_t i=0;i<current->states.size();++i)if(current->states[i].constData()==buffer) {
            current->stateBytes-=current->states[i].size()+current->images[i].sizeInBytes()+current->secondImages[i].sizeInBytes();
            current->states[i].clear();current->images[i]={};current->secondImages[i]={};return;
        }
    }
    MovieInput synchronize() {
        std::array<char, GGPO_MAX_PLAYERS*InputSize> values{};int disconnected=0;
        const auto result=ggpo_synchronize_input(session,values.data(),int(players.size())*InputSize,&disconnected);
        if(result!=GGPO_OK) throw std::runtime_error("GGPO input synchronization failed during rollback");
        if(disconnected) throw std::runtime_error("GGPO participant disconnected");
        for(int i=0;i<players.size();++i)
            contributions[i]={players[i].role,decodeInput(values.data()+i*InputSize)};
        return NetplayTimeline::merge(contributions);
    }
    static bool advance(int) {
        auto& self=*current;
        NetplayTimeline::apply(*self.host,self.synchronize());
        ++self.rollbackPosition;
        ++self.rollbackFrames;
        self.maxRollbackFrames=std::max(self.maxRollbackFrames,++self.rollbackGroupFrames);
        const bool preview=self.historyEnabled&&self.localId==0&&self.rollbackPosition%2==0;
        self.host->runFrame(preview,false); // Only rewind previews; no presentation, audio queue, achievements or disk writes.
        if(ggpo_advance_frame(self.session)!=GGPO_OK) throw std::runtime_error("GGPO rollback advance failed");
        return true;
    }
    static bool event(GGPOEvent* event) {
        auto& self=*current;
        if(event->code==GGPO_EVENTCODE_RUNNING) self.running=true;
        if(event->code==GGPO_EVENTCODE_DISCONNECTED_FROM_PEER)
            self.failure=QStringLiteral("GGPO participant disconnected");
        return true;
    }
    static void transmit(void* opaque,uint32_t address,uint16_t,const void* data,int length) {
        auto& self=*static_cast<Impl*>(opaque);
        if(length<=0 || length>8192) throw std::runtime_error("Invalid GGPO datagram size");
        for(const auto& peer:self.peers) if(peer.address==address) {
            QByteArray packet(8+length,Qt::Uninitialized);qToBigEndian(self.epoch,packet.data());
            std::memcpy(packet.data()+8,data,size_t(length));self.send(peer.id,packet);return;
        }
        throw std::runtime_error("Unknown GGPO endpoint");
    }
    static int receive(void* opaque,uint32_t* address,uint16_t* port,void* data,int capacity) {
        auto& self=*static_cast<Impl*>(opaque);
        if(self.incoming.empty()) return 0;
        auto packet=std::move(self.incoming.front());self.incoming.pop_front();self.queuedBytes-=packet.data.size();
        const auto length=packet.data.size()-8;
        if(length>capacity) return -1;
        *address=packet.address;*port=1;std::memcpy(data,packet.data.constData()+8,size_t(length));
        return int(length);
    }
    bool check(GGPOErrorCode result,QString& error) {
        if(!failure.isEmpty()) {error=failure;return false;}
        if(result==GGPO_OK) return true;
        error=QStringLiteral("GGPO error %1").arg(int(result));failure=error;return false;
    }
};
thread_local NetRollbackSession::Impl* NetRollbackSession::Impl::current=nullptr;
NetRollbackSession::NetRollbackSession(): d(std::make_unique<Impl>()) {}
NetRollbackSession::~NetRollbackSession(){stop();}
void NetRollbackSession::stop() {
    Impl::Scope scope(d.get());
    if(d->session) ggpo_close_session(d->session);
    d=std::make_unique<Impl>();
}
void NetRollbackSession::setHistoryEnabled(bool enabled) {
    if(d->historyEnabled==enabled)return;
    d->historyEnabled=enabled;
    if(!enabled)for(size_t i=0;i<d->images.size();++i) {
        d->stateBytes-=d->images[i].sizeInBytes()+d->secondImages[i].sizeInBytes();
        d->images[i]={};d->secondImages[i]={};
    }
}
bool NetRollbackSession::takeConfirmedHistory(RewindFrame& output) {
    if(!d->session || !d->historyEnabled || d->localId!=0)return false;
    int confirmed=-1;Impl::Scope scope(d.get());
    if(ggpo_get_confirmed_frame(d->session,&confirmed)!=GGPO_OK)return false;
    int index=-1,earliest=2147483647;
    for(size_t i=0;i<d->states.size();++i)
        if(!d->states[i].isEmpty() && !d->images[i].isNull() && d->stateFrames[i]>d->lastHistoryFrame && d->stateFrames[i]<earliest && d->stateFrames[i]<=confirmed+1) {
            index=int(i);earliest=d->stateFrames[i];
        }
    if(index<0)return false;
    output={d->states[index],d->images[index],d->aspects[index],d->secondImages[index],d->secondAspects[index]};d->lastHistoryFrame=earliest;return true;
}
bool NetRollbackSession::active() const {return d->session!=nullptr;}
quint64 NetRollbackSession::position() const {return d->frame;}
bool NetRollbackSession::start(LibretroHost& host,quint64 epoch,const QJsonArray& roster,quint64 localId,Send send,QString& error) {
    stop();error.clear();d->qualityClock.start();d->host=&host;d->epoch=epoch;d->localId=localId;d->send=std::move(send);
    if(!epoch || roster.isEmpty() || roster.size()>32) {error=QStringLiteral("Invalid GGPO roster");return false;}
    bool localPresent=false,hostPresent=false;
    for(int i=0;i<roster.size();++i) {
        const auto entry=roster[i].toObject();bool ok=false;const auto id=entry.value("id").toString().toULongLong(&ok);
        const int role=entry.value("role").toInt(-1);
        if(!ok || role<0 || role>2) {error=QStringLiteral("Invalid GGPO participant");return false;}
        for(const auto& peer:d->peers) if(peer.id==id) {error=QStringLiteral("Duplicate GGPO participant");return false;}
        Impl::Peer peer{id,role,quint32(0x0a010001u+i)};d->peers.append(peer);
        if(role || id==0) d->players.append(peer);
        localPresent|=id==localId;hostPresent|=id==0;
    }
    if(!localPresent || !hostPresent || d->players.isEmpty() || d->players.size()>GGPO_MAX_PLAYERS) {
        error=QStringLiteral("Unsupported GGPO roster");return false;
    }
    d->contributions.resize(d->players.size());
    Impl::Scope scope(d.get());
    KegaGgpoTransport transport{d.get(),&Impl::transmit,&Impl::receive};
    kega_ggpo_set_transport(&transport);
    GGPOSessionCallbacks cb{};cb.begin_game=&Impl::begin;cb.save_game_state=&Impl::save;cb.load_game_state=&Impl::load;
    cb.log_game_state=&Impl::log;cb.free_buffer=&Impl::free;cb.advance_frame=&Impl::advance;cb.on_event=&Impl::event;
    bool spectator=true;for(const auto& player:d->players) if(player.id==localId)spectator=false;
    // Epoch and game/core compatibility were authenticated by the application.
    // The fork does not propagate optional verification data to spectator endpoints.
    const QByteArray verification;
    try {
        GGPOErrorCode result;
        if(spectator) {
            QByteArray address;for(const auto& peer:d->peers) if(peer.id==0)address=QHostAddress(peer.address).toString().toLatin1();
            result=ggpo_start_spectating(&d->session,&cb,"KegaRefusion",int(d->players.size()),InputSize,0,address.data(),1,verification.constData(),int(verification.size()));
        } else result=ggpo_start_session(&d->session,&cb,"KegaRefusion",int(d->players.size()),InputSize,0,verification.constData(),int(verification.size()));
        kega_ggpo_set_transport(nullptr);
        if(!d->check(result,error)){stop();return false;}
        if(!spectator) {
            for(int i=0;i<d->players.size();++i) {
                const auto& peer=d->players[i];GGPOPlayer player{};player.size=sizeof(player);player.player_num=i+1;
                player.type=peer.id==localId?GGPO_PLAYERTYPE_LOCAL:GGPO_PLAYERTYPE_REMOTE;
                const auto address=QHostAddress(peer.address).toString().toLatin1();
                if(player.type==GGPO_PLAYERTYPE_REMOTE) {std::strncpy(player.u.remote.ip_address,address.constData(),sizeof(player.u.remote.ip_address)-1);player.u.remote.port=1;}
                GGPOPlayerHandle handle=-1;if(!d->check(ggpo_add_player(d->session,&player,&handle),error)){stop();return false;}
                if(peer.id==localId){d->localHandle=handle;d->localRole=peer.role;}
                else d->remoteQuality.append({peer.id,handle,0,{}});
            }
            if(localId==0) for(const auto& peer:d->peers) if(peer.id && !peer.role) {
                GGPOPlayer player{};player.size=sizeof(player);player.type=GGPO_PLAYERTYPE_SPECTATOR;
                const auto address=QHostAddress(peer.address).toString().toLatin1();
                std::strncpy(player.u.remote.ip_address,address.constData(),sizeof(player.u.remote.ip_address)-1);player.u.remote.port=1;
                GGPOPlayerHandle handle=-1;if(!d->check(ggpo_add_player(d->session,&player,&handle),error)){stop();return false;}
            }
        }
        ggpo_set_disconnect_timeout(d->session,15000);ggpo_set_disconnect_notify_start(d->session,3000);
        return true;
    } catch(const std::exception& e) {kega_ggpo_set_transport(nullptr);error=QString::fromUtf8(e.what());stop();return false;}
}
void NetRollbackSession::receive(quint64 peer,const QByteArray& packet) {
    if(!d->session || packet.size()<9 || packet.size()>8200 || qFromBigEndian<quint64>(packet.constData())!=d->epoch)return;
    for(const auto& entry:d->peers) if(entry.id==peer && peer!=d->localId) {
        if(d->incoming.size()>=256 || d->queuedBytes+packet.size()>PacketLimit)return;
        d->queuedBytes+=packet.size();d->incoming.push_back({entry.address,packet});return;
    }
}
bool NetRollbackSession::idle(QString& error) {
    if(!d->session)return true;
    Impl::Scope scope(d.get());
    try {return d->check(ggpo_idle(d->session,0),error);}
    catch(const std::exception& e){error=QString::fromUtf8(e.what());d->failure=error;return false;}
}
NetRollbackSession::Result NetRollbackSession::beforeFrame(const MovieInput& local,MovieInput& merged,QString& error) {
    if(!d->session)return Wait;
    if(!d->failure.isEmpty()){error=d->failure;return Failed;}
    if(!d->running)return Wait;
    Impl::Scope scope(d.get());
    try {
        if(d->localHandle>=0 && !d->localQueued) {
            auto bytes=encodeInput(local);
            const int previousDelay=d->delay.frames();
            int nextDelay=d->realtime?d->delay.pending():previousDelay;
            // GGPO drops one captured input when reducing delay by one. Only
            // drop a duplicate, including pointer aim, never a fresh button edge.
            if(nextDelay<previousDelay && (!d->havePreviousInput || bytes!=d->previousInput))
                nextDelay=previousDelay;
            if(nextDelay!=previousDelay && !d->check(ggpo_set_frame_delay(d->session,d->localHandle,nextDelay),error))return Failed;
            const auto result=ggpo_add_local_input(d->session,d->localHandle,bytes.data(),bytes.size());
            if(result==GGPO_ERRORCODE_PREDICTION_THRESHOLD || result==GGPO_ERRORCODE_NOT_SYNCHRONIZED) {
                // No input entered GGPO. Restore the delay, so a later retry
                // cannot discard a newly pressed button after the wait.
                if(nextDelay!=previousDelay && !d->check(ggpo_set_frame_delay(d->session,d->localHandle,previousDelay),error))return Failed;
                return Wait;
            }
            if(!d->check(result,error))return Failed;
            if(nextDelay!=previousDelay)d->delay.applied(nextDelay,d->qualityClock.elapsed());
            d->previousInput=bytes;d->havePreviousInput=true;
            d->localQueued=true;
        }
        std::array<char,GGPO_MAX_PLAYERS*InputSize> values{};int disconnected=0;
        const auto result=ggpo_synchronize_input(d->session,values.data(),int(d->players.size())*InputSize,&disconnected);
        if(result==GGPO_ERRORCODE_NOT_SYNCHRONIZED || result==GGPO_ERRORCODE_PREDICTION_THRESHOLD)return Wait;
        if(!d->check(result,error))return Failed;
        if(disconnected){error=QStringLiteral("GGPO participant disconnected");return Failed;}
        for(int i=0;i<d->players.size();++i)d->contributions[i]={d->players[i].role,decodeInput(values.data()+i*InputSize)};
        merged=NetplayTimeline::merge(d->contributions);return Run;
    }catch(const std::exception& e){error=QString::fromUtf8(e.what());d->failure=error;return Failed;}
}
bool NetRollbackSession::afterFrame(QString& error) {
    if(!d->session)return false;
    Impl::Scope scope(d.get());
    try {
        if(!d->check(ggpo_advance_frame(d->session),error))return false;
        ++d->frame;d->localQueued=false;return true;
    }catch(const std::exception& e){error=QString::fromUtf8(e.what());d->failure=error;return false;}
}

bool NetRollbackSession::takeQuality(QJsonObject& output,bool realtime) {
    if(!d->session)return false;
    d->realtime=realtime;
    if(!realtime)d->delay.suspend();
    const qint64 now=d->qualityClock.elapsed(),interval=now-d->lastQualityMs;
    if(interval<1000)return false;
    Impl::Scope scope(d.get());
    int ping=-1,jitter=0;bool allFresh=!d->remoteQuality.isEmpty(),newSample=false;
    QJsonArray peers;
    for(auto& peer:d->remoteQuality) {
        GGPONetworkStats stats{};
        const bool valid=ggpo_get_network_stats(d->session,peer.handle,&stats)==GGPO_OK &&
            stats.network.quality_samples && stats.network.quality_age_ms>=0 &&
            stats.network.quality_age_ms<=2500 && stats.network.ping>=0;
        allFresh&=valid;
        if(valid) {
            if(peer.serial!=stats.network.quality_samples) {
                peer.serial=stats.network.quality_samples;peer.estimate.sample(stats.network.ping);newSample=true;
            }
            ping=std::max(ping,peer.estimate.pingMs());jitter=std::max(jitter,peer.estimate.jitterMs());
        }
        peers.append(QJsonObject{{"id",QString::number(peer.id)},
            {"pingMs",valid?peer.estimate.pingMs():-1},{"jitterMs",valid?peer.estimate.jitterMs():0}});
    }
    // Do not interpret a paused or CPU-starved emulator as a reason to add
    // input latency. Severe links beyond GGPO's prediction window remain bounded.
    const auto minimumProgress=quint64(std::ceil(d->host->fps()*double(interval)/2000.0));
    const bool eligible=realtime && d->running && d->localHandle>=0 && d->localRole>0 && allFresh &&
        d->frame-d->qualityFrame>=std::max<quint64>(1,minimumProgress);
    if(!eligible)d->delay.suspend();
    else if(newSample)d->delay.observe(ping,jitter,d->host->fps(),now,true,d->maxRollbackFrames<=2);
    const int replayRate=int(std::lround(double(d->rollbackFrames)*1000.0/double(interval)));
    output=QJsonObject{{"active",true},{"pingMs",ping},{"jitterMs",jitter},
        {"delayFrames",d->localRole>0?d->delay.frames():-1},
        {"rollbackFrames",replayRate},{"rollbackEvents",d->rollbackEvents},
        {"maxRollbackFrames",d->maxRollbackFrames},{"peers",peers}};
    d->lastQualityMs=now;d->qualityFrame=d->frame;
    d->rollbackFrames=d->rollbackEvents=d->maxRollbackFrames=0;
    return true;
}
