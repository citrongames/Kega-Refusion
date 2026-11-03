#include "i18n/Localization.h"
#include "EmulationSession.h"
#include "LibretroHost.h"
#include "net/NetplayTimeline.h"
#include "net/NetRollbackSession.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QBuffer>
#include <QImageReader>
#include "MovieSession.h"
#include "AchievementsClient.h"
#include <QtEndian>
#include "RewindBuffer.h"
#include "FramePacer.h"
#include "PerformanceMetrics.h"
#include "VideoFrameGeometry.h"
#include <QPainter>
#include <cstring>
#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace {
constexpr int LinkPreviewBytes = 48;
struct LinkPreviewGeometry {
    QSize size[2], native[2];
    double aspect[2]{};
};
bool linkPreviewGeometry(const QByteArray& bytes, LinkPreviewGeometry& geometry) {
    if(bytes.size()!=LinkPreviewBytes)return false;
    for(int i=0;i<2;++i) {
        const char* data=bytes.constData()+24*i;
        geometry.size[i]=QSize(qFromBigEndian<quint32>(data),qFromBigEndian<quint32>(data+4));
        geometry.native[i]=QSize(qFromBigEndian<quint32>(data+8),qFromBigEndian<quint32>(data+12));
        const quint64 bits=qFromBigEndian<quint64>(data+16);
        std::memcpy(&geometry.aspect[i],&bits,sizeof(bits));
        if(!VideoFrameGeometry::validSize(geometry.size[i]) || !VideoFrameGeometry::validSize(geometry.native[i]) ||
            !std::isfinite(geometry.aspect[i]) || geometry.aspect[i]<=0 || geometry.aspect[i]>8)return false;
    }
    return geometry.size[0].height()+geometry.size[1].height()<=4096;
}
QImage combineLinkPreview(const QImage& first,double firstAspect,const QImage& second,double secondAspect,QByteArray& geometry) {
    geometry.clear();
    if(first.isNull() || second.isNull())return {};
    QImage combined(std::max(first.width(),second.width()),first.height()+second.height(),QImage::Format_RGB32);
    if(combined.isNull() || combined.sizeInBytes()>64*1024*1024)return {};
    combined.fill(Qt::black);
    { QPainter painter(&combined);painter.drawImage(0,0,first);painter.drawImage(0,first.height(),second); }
    geometry.resize(LinkPreviewBytes);
    const QImage* frames[]={&first,&second};
    const double aspects[]={firstAspect,secondAspect};
    for(int i=0;i<2;++i) {
        char* data=geometry.data()+24*i;
        const auto native=VideoFrameGeometry::nativeSize(*frames[i]);
        qToBigEndian(quint32(frames[i]->width()),data);qToBigEndian(quint32(frames[i]->height()),data+4);
        qToBigEndian(quint32(native.width()),data+8);qToBigEndian(quint32(native.height()),data+12);
        quint64 bits;std::memcpy(&bits,&aspects[i],sizeof(bits));qToBigEndian(bits,data+16);
    }
    return combined;
}
QImage selectLinkPreview(const QImage& image,const QByteArray& bytes,int role,double& aspect) {
    if(bytes.isEmpty())return image;
    LinkPreviewGeometry geometry;
    if(!linkPreviewGeometry(bytes,geometry) || image.width()!=std::max(geometry.size[0].width(),geometry.size[1].width()) ||
        image.height()!=geometry.size[0].height()+geometry.size[1].height())return {};
    const int index=role==2?1:0;
    QImage selected=image.copy(QRect(QPoint(0,index?geometry.size[0].height():0),geometry.size[index]));
    VideoFrameGeometry::setNativeSize(selected,geometry.native[index]);aspect=geometry.aspect[index];return selected;
}
QByteArray encodeRewindPreview(const QImage& image,QByteArray& geometry) {
    QByteArray bytes;
    if(image.isNull())return bytes;
    { QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);image.save(&buffer,"PNG"); }
    if(bytes.size()<=60*1024)return bytes;
    QImage smaller;
    if(!geometry.isEmpty()) {
        double firstAspect=1,secondAspect=1;
        const auto first=selectLinkPreview(image,geometry,1,firstAspect),second=selectLinkPreview(image,geometry,2,secondAspect);
        auto firstSmall=first.scaled(160,120,Qt::KeepAspectRatio,Qt::FastTransformation);
        auto secondSmall=second.scaled(160,120,Qt::KeepAspectRatio,Qt::FastTransformation);
        VideoFrameGeometry::setNativeSize(firstSmall,VideoFrameGeometry::nativeSize(first));
        VideoFrameGeometry::setNativeSize(secondSmall,VideoFrameGeometry::nativeSize(second));
        smaller=combineLinkPreview(firstSmall,firstAspect,secondSmall,secondAspect,geometry);
    } else smaller=image.scaled(320,240,Qt::KeepAspectRatio,Qt::FastTransformation);
    bytes.clear();QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);smaller.save(&buffer,"JPEG",65);
    if(bytes.size()>60*1024)bytes.clear();
    return bytes;
}
}

EmulationSession::EmulationSession(QString corePath, QString dataPath, QObject* parent)
    : QThread(parent), corePath_(std::move(corePath)), dataPath_(std::move(dataPath)) {
    qRegisterMetaType<CheatList>(); qRegisterMetaType<AchievementsState>();
    qRegisterMetaType<AchievementNotice>(); qRegisterMetaType<AchievementHttpRequest>(); start();
}
EmulationSession::~EmulationSession() { shutdown(); }
void EmulationSession::enqueue(Command command) {
    if(command.type==Type::Load&&networkMode_.load())emit networkGameLoading(command.path,command.value);
    if (networkMode_.load() && !command.networkAuthorized && networkShared(command.type)) {
        emit networkCommandRequested(encodeNetworkCommand(command)); return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (commands_.size()<2048) commands_.push_back(std::move(command));
}
void EmulationSession::loadGame(const QString& path, int archiveIndex) { enqueue({Type::Load,path,archiveIndex}); }
void EmulationSession::loadGame(const QString& path, const RomArchive::Entry& archiveEntry) {
    enqueue({Type::Load,path,archiveEntry.index,archiveEntry});
}
void EmulationSession::closeGame() { enqueue({Type::Close,{}}); }
void EmulationSession::setPaused(bool paused) { enqueue({Type::Pause,{},paused ? 1 : 0}); }
void EmulationSession::resetGame() { enqueue({Type::Reset,{}}); }
void EmulationSession::hardResetGame() { enqueue({Type::HardReset,{}}); }
void EmulationSession::changeDisc(const QString& path) { enqueue({Type::Disc,path}); }
void EmulationSession::selectDisc(int index) { enqueue({Type::DiscIndex,{},index}); }
void EmulationSession::setDiscEjected(bool ejected) { enqueue({Type::Eject,{},ejected ? 1 : 0}); }
void EmulationSession::setRegion(int region) { enqueue({Type::Region,{},region}); }
void EmulationSession::setDebugMasks(quint32 video, quint64 audio) {
    Command command{Type::DebugMasks,{},int(video & 63u)};
    command.requestId = audio & ((quint64(1) << 38)-1); enqueue(std::move(command));
}
void EmulationSession::setDebugAudioGain(unsigned channel, unsigned percent) {
    if (channel >= 38) return;
    percent = std::min(percent,100u);
    std::lock_guard<std::mutex> lock(mutex_);
    // Collapse slider motion before the next emulation boundary; never queue
    // an unbounded stream, and never cross a load/close command boundary.
    for (auto it=commands_.rbegin();it!=commands_.rend();++it) {
        if (it->type==Type::Load || it->type==Type::Close) break;
        if (it->type==Type::DebugGain && it->value==int(channel)) { it->requestId=percent;return; }
    }
    if (commands_.size()<2048) {
        Command command{Type::DebugGain,{},int(channel)};command.requestId=percent;
        commands_.push_back(std::move(command));
    }
}
void EmulationSession::setDebugBackdrop(quint32 rgb) { enqueue({Type::DebugBackdrop,{},int(rgb & 0xffffff)}); }
void EmulationSession::setBlarggFilter(int filter) { enqueue({Type::Filter,{},filter}); }
void EmulationSession::setFastCdLoading(bool enabled) { enqueue({Type::CdLoading,{},enabled ? 1 : 0}); }
void EmulationSession::setRemoveSpriteLimit(bool enabled) { enqueue({Type::SpriteLimit,{},enabled ? 1 : 0}); }
void EmulationSession::setAutoFixChecksum(bool enabled) { enqueue({Type::Checksum,{},enabled ? 1 : 0}); }
void EmulationSession::setControllerAdapter(ControllerAdapter adapter) {
    // Connected participants follow host hardware; local bindings remain editable.
    if (networkMode_.load() != 2 && validControllerAdapter(int(adapter))) enqueue({Type::Adapter,{},int(adapter)});
}
void EmulationSession::setControllerConnected(unsigned port, bool connected) {
    if (networkMode_.load() == 2) return; // Host snapshots own hardware; local bindings do not send it back.
    if (port < MaxControllerPlayers) enqueue({Type::Port,{},int(port) | (connected ? 8 : 0)});
}
void EmulationSession::setControllerSixButton(unsigned port, bool sixButton) {
    if (networkMode_.load() == 2) return; // Host snapshots own hardware; local bindings do not send it back.
    if (port < MaxControllerPlayers) enqueue({Type::ControllerType,{},int(port) | (sixButton ? 8 : 0)});
}
void EmulationSession::setControllerKind(unsigned player, ControllerType type) {
    if (networkMode_.load() == 2) return; // Host snapshots own hardware; local bindings do not send it back.
    if (player < MaxControllerPlayers && int(type) >= 0 && int(type) <= int(ControllerType::Paddle))
        enqueue({Type::ControllerKind,{},int(player) | (int(type) << 3)});
}
void EmulationSession::setGunAim(unsigned player, int x, int y, bool offscreen) {
    if (player >= 3) return;
    const quint64 packed = quint16(std::clamp(x,-32767,32767)) |
        (quint64(quint16(std::clamp(y,-32767,32767))) << 16) | (quint64(offscreen) << 32);
    gunPositions_[player].store(packed);
}
void EmulationSession::advanceFrame() { enqueue({Type::FrameAdvance,{}}); }
void EmulationSession::setFrameAdvanceHeld(bool held) {
    if (networkMode_.load()) {
        if (networkStepRequested_.exchange(held) != held) enqueue({Type::NetStepHold,{},held?1:0});
    } else frameAdvanceHeld_.store(held);
}
void EmulationSession::startMovieRecording(const QString& path) { enqueue({Type::MovieRecord,path}); }
void EmulationSession::playMovie(const QString& path) { enqueue({Type::MoviePlay,path}); }
void EmulationSession::resumeMovieRecording(const QString& path) { enqueue({Type::MovieResume,path}); }
void EmulationSession::rerecordMovie(const QString& path) { enqueue({Type::MovieFork,path}); }
void EmulationSession::takeOverMovie() { enqueue({Type::MovieTakeOver,{}}); }
void EmulationSession::stopMovie() { enqueue({Type::MovieStop,{}}); }
void EmulationSession::setCheats(const CheatList& cheats) {
    Command command{Type::Cheats,{}}; command.cheats = cheats; enqueue(std::move(command));
}
void EmulationSession::updateAudioClock(qint64 queuedUsecs, qint64 targetUsecs, bool available) {
    audioQueueUs_.store(queuedUsecs); audioTargetUs_.store(targetUsecs);
    audioClockAvailable_.store(available);
    audioFeedbackNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void EmulationSession::publishVideo(const QImage& frame, double aspectRatio) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        latestFrame_ = frame; latestAspect_ = aspectRatio;
        if (!videoPending_) { videoPending_ = true; notify = true; }
    }
    if (notify) emit videoFrameReady();
    // Kept for instrumentation; the application uses the coalesced ready signal.
    emit videoFrame(frame,aspectRatio);
}
bool EmulationSession::takeVideoFrame(QImage& frame, double& aspectRatio) {
    std::lock_guard<std::mutex> lock(videoMutex_);
    if (!videoPending_) return false;
    frame = latestFrame_; aspectRatio = latestAspect_; videoPending_ = false;
    return !frame.isNull();
}
void EmulationSession::saveState(int slot) { enqueue({Type::Save,{},slot}); }
void EmulationSession::loadState(int slot) { enqueue({Type::Restore,{},slot}); }
void EmulationSession::setButton(unsigned port, unsigned id, bool pressed) {
    if (port >= MaxControllerPlayers || id >= 16) return;
    if (pressed) buttons_[port].fetch_or(1u << id); else buttons_[port].fetch_and(~(1u << id));
}
void EmulationSession::setButtons(unsigned port, unsigned mask) { if (port < MaxControllerPlayers) buttons_[port].store(mask & 0xffffu); }
void EmulationSession::releaseButtons() {
    for (auto& mask : buttons_) mask.store(0);
    clearMouseMotion();
}
void EmulationSession::clearMouseMotion() {
    for (auto& motion : mouseMotion_) {
        auto previous=motion.load();
        while(!motion.compare_exchange_weak(previous,((previous>>32)+1)<<32)) {}
    }
}
void EmulationSession::addMouseMotion(unsigned player,int dx,int dy) {
    if(player>=3)return;
    auto& motion=mouseMotion_[player];
    auto previous=motion.load();
    for (;;) {
        const int x=std::clamp(int(qint16(previous))+std::clamp(dx,-32767,32767),-32767,32767);
        const int y=std::clamp(int(qint16(previous>>16))+std::clamp(dy,-32767,32767),-32767,32767);
        const quint64 next=(previous&0xffffffff00000000ULL)|quint16(qint16(x))|(quint64(quint16(qint16(y)))<<16);
        if(motion.compare_exchange_weak(previous,next))break;
    }
}
void EmulationSession::setPaddlePosition(unsigned player,int position) {
    if(player<3)gunPositions_[player].store(quint16(qint16(std::clamp(position,-32768,32767))));
}
void EmulationSession::setFastForward(bool enabled) {
    if (networkMode_.load()) {
        if (networkFastRequested_.exchange(enabled) != enabled) enqueue({Type::NetFast,{},enabled?1:0});
    } else fastForward_.store(enabled);
}
void EmulationSession::setFastForwardFactor(int factor) {
    factor=factor==0?0:std::clamp(factor,2,32);
    if(networkMode_.load()) enqueue({Type::NetFactor,{},factor}); else fastForwardFactor_.store(factor);
}
void EmulationSession::setRewind(bool enabled) {
    if (networkMode_.load()) {
        if (networkRewindRequested_.exchange(enabled) != enabled) enqueue({Type::NetReverse,{},enabled?1:0});
    } else rewind_.store(enabled);
}
void EmulationSession::setRewindEnabled(bool enabled) { rewindEnabled_.store(enabled); }
void EmulationSession::setRewindMemoryMiB(int mebibytes) { rewindMemoryMiB_.store(std::clamp(mebibytes,16,1024)); }
void EmulationSession::setSmsFmEnabled(bool enabled) { enqueue({Type::SmsFm,{},enabled?1:0}); }
void EmulationSession::setBackgroundPaused(bool paused) { backgroundPaused_.store(paused); }
void EmulationSession::setPerformanceMetricsEnabled(bool enabled) { enqueue({Type::PerformanceMetrics,{},enabled ? 1 : 0}); }
void EmulationSession::setGameFpsEnabled(bool enabled) { enqueue({Type::GameFps,{},enabled ? 1 : 0}); }
void EmulationSession::setExpansionDevices(bool cd, bool x32) { enqueue({Type::Expansions,{},(cd ? 1 : 0) | (x32 ? 2 : 0)}); }
void EmulationSession::requestSlotStatus(int selected) { enqueue({Type::Slots,{},selected}); }
void EmulationSession::configureAchievements(bool enabled, bool hardcore) {
    enqueue({Type::AchievementsConfigure,{},(enabled ? 1 : 0) | (hardcore ? 2 : 0)});
}
void EmulationSession::loginAchievements(QString user, QString password) {
    Command command{Type::AchievementsLogin,std::move(user)};
    command.bytes = password.toUtf8(); enqueue(std::move(command));
}
void EmulationSession::logoutAchievements() { enqueue({Type::AchievementsLogout,{}}); }
void EmulationSession::requestAchievements() { enqueue({Type::AchievementsRefresh,{}}); }
void EmulationSession::achievementResponse(quint64 id, int status, QByteArray body) {
    if (body.size() > 8*1024*1024) { body.clear(); status = 0; }
    Command command{Type::AchievementsResponse,{},status};
    command.requestId = id; command.bytes = std::move(body); enqueue(std::move(command));
}
bool EmulationSession::networkShared(Type t) {
    switch(t) {
    case Type::Close: case Type::Pause: case Type::Reset: case Type::HardReset:
    case Type::Region: case Type::CdLoading: case Type::SpriteLimit:
    case Type::Checksum: case Type::Port: case Type::ControllerType: case Type::ControllerKind:
    case Type::FrameAdvance: case Type::Cheats: case Type::Save: case Type::Restore:
    case Type::DiscIndex: case Type::Eject: case Type::Expansions: case Type::NetFast: case Type::NetReverse: case Type::NetFactor: case Type::NetStepHold: return true;
    default:return false;
    }
}
QByteArray EmulationSession::encodeNetworkCommand(const Command& c) {
    QJsonObject o{{"type",int(c.type)},{"value",c.value}};
    if (c.type==Type::Cheats) {
        QJsonArray a; for (const auto& e:c.cheats) a.append(QJsonObject{{"code",e.code},{"description",e.description},{"enabled",e.enabled}});
        o.insert("cheats",a);
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
void EmulationSession::applyNetworkCommand(const QByteArray& bytes) {
    if (networkMode_.load()!=1 || bytes.size()>64*1024) return;
    const auto o=QJsonDocument::fromJson(bytes).object();
    const int t=o.value("type").toInt(-1), v=o.value("value").toInt();
    if (t<0 || t>int(Type::NetStepHold) || !networkShared(Type(t))) return;
    Command c{Type(t),{},v}; c.networkAuthorized=true;
    if ((c.type==Type::Save || c.type==Type::Restore) && (v<0 || v>9)) return;
    if (c.type==Type::Region && (v<0 || v>3)) return;
    if (c.type==Type::Filter && (v<0 || v>4)) return;
    if ((c.type==Type::ControllerType || c.type==Type::Port) && (v<0 || v>15)) return;
    if (c.type==Type::ControllerKind && (v<0 || v>47 || ((v&7)>=3 && (v>>3)!=0) || ((v&7)==2 && (v>>3)!=0 && (v>>3)!=3))) return;
    if (c.type==Type::Cheats) {
        const auto a=o.value("cheats").toArray(); if(a.size()>256) return;
        for (const auto& v:a) { const auto e=v.toObject();
            const auto code=e.value("code").toString(), description=e.value("description").toString();
            if(code.size()>256 || description.size()>512) return;
            c.cheats.push_back({code,description,e.value("enabled").toBool()});
        }
    }
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(c.type==Type::Save){if(now-lastNetworkSaveMs_<1000)return;lastNetworkSaveMs_=now;}
    if(c.type==Type::Restore||c.type==Type::Reset||c.type==Type::HardReset){if(now-lastNetworkHeavyMs_<200)return;lastNetworkHeavyMs_=now;}
    enqueue(std::move(c));
}
bool EmulationSession::isNetworkMomentaryCommand(const QByteArray& bytes) {
    if(bytes.size()>64*1024)return false;
    const auto document=QJsonDocument::fromJson(bytes);
    if(!document.isObject())return false;
    const auto object=document.object();const auto value=object.value("value");
    if(!value.isDouble() || (value.toDouble()!=0.0 && value.toDouble()!=1.0))return false;
    const int type=object.value("type").toInt(-1);
    return type==int(Type::NetFast)||type==int(Type::NetReverse)||type==int(Type::NetStepHold);
}
bool EmulationSession::isNetworkMomentaryRelease(const QByteArray& bytes) {
    return isNetworkMomentaryCommand(bytes) && QJsonDocument::fromJson(bytes).object().value("value").toInt()==0;
}
void EmulationSession::setNetworkMode(int mode,const QString& scope) {
    if(mode==0 && networkLinkCableDesired_.exchange(false))networkLinkCableRevision_.fetch_add(1);
    if (networkMode_.exchange(mode) != mode) {
        networkFastRequested_.store(false);
        networkRewindRequested_.store(false);
        networkStepRequested_.store(false);
    }
    networkWaiting_.store(mode==2);
    enqueue({Type::NetMode,scope,mode});
}
void EmulationSession::setNetworkRole(int role) {
    role=std::clamp(role,0,int(MaxControllerPlayers));
    if(networkRole_.exchange(role)==role)return;
    enqueue({Type::NetRole,{},role});
    emit networkRoleChanged(role);
}
void EmulationSession::setNetworkLinkCableEnabled(bool enabled) {
    if(networkLinkCableDesired_.exchange(enabled)!=enabled)networkLinkCableRevision_.fetch_add(1);
    std::lock_guard<std::mutex> lock(mutex_);
    for(auto command=commands_.rbegin();command!=commands_.rend();++command) {
        if(command->type==Type::NetMode || command->type==Type::Load || command->type==Type::Close)break;
        if(command->type==Type::NetLinkCable){command->value=enabled?1:0;return;}
    }
    if(commands_.size()<2048)commands_.push_back({Type::NetLinkCable,{},enabled?1:0});
}
void EmulationSession::receiveNetworkSlots(unsigned mask) {
    if(networkMode_.load()==2)enqueue({Type::NetSlots,{},int(mask&0x3ff)});
}
void EmulationSession::closeNetworkGame() {
    if(networkMode_.load()!=2)return;
    Command c{Type::Close,{}};c.networkAuthorized=true;enqueue(std::move(c));
}
void EmulationSession::requestNetworkSnapshot() { enqueue({Type::NetSnapshot,{}}); }
void EmulationSession::receiveNetworkSnapshot(const QByteArray& bytes) {
    if(bytes.size()>128*1024*1024) return;
    Command c{Type::NetReceiveState,{}};c.bytes=bytes;c.requestId=networkLinkCableRevision_.load();enqueue(std::move(c));
}
void EmulationSession::receiveNetworkRewindPreview(const QByteArray& bytes) {
    if(networkMode_.load()!=2 || bytes.size()<4 || bytes.size()>64*1024)return;
    std::lock_guard<std::mutex> lock(mutex_);
    // A delayed GUI/worker needs only the latest preview, not a growing video queue.
    for(auto& command:commands_)if(command.type==Type::NetRewindPreview){command.bytes=bytes;return;}
    Command command{Type::NetRewindPreview,{}};command.bytes=bytes;commands_.push_back(std::move(command));
}
void EmulationSession::setNetworkRoster(const QByteArray& roster) {
    if(roster.size()>16*1024)return;
    Command c{Type::NetRoster,{}};c.bytes=roster;enqueue(std::move(c));
}
void EmulationSession::receiveNetworkDatagram(quint64 peer,const QByteArray& packet) {
    if(!networkMode_.load() || packet.size()<9 || packet.size()>8200)return;
    Command c{Type::NetDatagram,{}};c.bytes=packet;c.requestId=peer;
    std::lock_guard<std::mutex> lock(mutex_);
    if(commands_.size()<256)commands_.push_back(std::move(c));
}
void EmulationSession::advanceNetworkFrame() {
    if(networkMode_.load()==2)enqueue({Type::NetAdvance,{}});
}
void EmulationSession::shutdown() { quitting_.store(true); if (isRunning()) wait(); }
void EmulationSession::run() {
    using Clock = std::chrono::steady_clock;
    LibretroHost host(corePath_,dataPath_);
    host.message = [this](const QString& message) { emit notification(message); };
    MovieSession movie(host, dataPath_);
    AchievementsState achievementState;
    bool achievementEnabled = false, achievementHardcoreRequested = false;
    bool achievementHardcore = false, achievementResetRequested = false;
    bool achievementPolicyRejected = false;
    bool achievementSuppressed = false;
    QString achievementStatusOverride;
    AchievementsClient achievements(dataPath_, {
        [&](uint32_t address, uint8_t* buffer, uint32_t count) {
            return host.readAchievementMemory(address,buffer,count);
        },
        [this](const AchievementHttpRequest& request) { emit achievementHttpRequest(request); },
        [&](const AchievementsState& state) {
            achievementState = state;
            if (!achievementStatusOverride.isEmpty()) achievementState.status = achievementStatusOverride;
            if (achievementSuppressed) achievementState.status = Localization::text(QStringLiteral("Учёт достижений отключён для записи управления или онлайн-сессии."));
            emit achievementsChanged(achievementState);
            const bool hardcore = state.enabled && state.loggedIn && state.hardcore && host.loaded() && !achievementSuppressed;
            if (hardcore && !host.achievementHardcoreBlockReason().isEmpty()) {
                achievementPolicyRejected = true;
                achievementStatusOverride = Localization::text(QStringLiteral("Hardcore недоступен: %1")).arg(host.achievementHardcoreBlockReason());
            }
            if (hardcore != achievementHardcore) {
                if (hardcore && !achievementPolicyRejected) achievementResetRequested = true;
                achievementHardcore = hardcore;
                if (hardcore) host.setDebugMasks(0,host.debugAudioMask());
                emit achievementsHardcoreChanged(hardcore);
            }
        },
        [this](const AchievementNotice& notice) { emit achievementNotice(notice); },
        [&] { achievementResetRequested = true; }
    });
    host.captureAchievementProgress = [&] {
        return achievementSuppressed ? QByteArray() : achievements.saveProgress();
    };
    host.restoreAchievementProgress = [&](const QByteArray& progress) { achievements.restoreProgress(progress); };
    const auto loadAchievements = [&](bool changedMedia = false) {
        if (achievementSuppressed || !achievementEnabled || !host.loaded()) { achievements.unloadGame(); achievements.refresh(); return; }
        if (!achievements.wantsGame()) return;
        QString hashError;
        const auto hash = host.achievementHash(hashError);
        if (hash.isEmpty()) {
            achievements.unloadGame(); achievementStatusOverride = hashError; achievements.refresh(); return;
        }
        if (changedMedia) achievements.changeMedia(hash,host.achievementConsoleId());
        else achievements.loadGame(hash,host.achievementConsoleId());
        achievements.refresh();
    };
    const auto configureAchievements = [&] {
        achievementStatusOverride.clear();
        const auto blocked = achievementHardcoreRequested && host.loaded()
            ? host.achievementHardcoreBlockReason() : QString();
        if (!blocked.isEmpty()) achievementStatusOverride = Localization::text(QStringLiteral("Hardcore недоступен: %1")).arg(blocked);
        if (achievementSuppressed && achievementHardcoreRequested)
            achievementStatusOverride = Localization::text(QStringLiteral("Завершите запись управления перед включением Hardcore."));
        if (achievementHardcoreRequested && !AchievementsHardcoreApproved)
            achievementStatusOverride = Localization::text(QStringLiteral("Hardcore требует одобрения Kega Refusion сервисом RetroAchievements. Учёт работает в Softcore."));
        achievements.configure(achievementEnabled,achievementHardcoreRequested && AchievementsHardcoreApproved &&
            blocked.isEmpty() && !achievementSuppressed);
        achievements.refresh();
    };
    auto nextAchievementIdle = Clock::now();
    bool lastAchievementBackground = false;
    std::array<quint64,3> mouseSnapshots{};
    auto liveInput = [&] {
        MovieInput input;
        for (unsigned player = 0; player < MaxControllerPlayers; ++player)
            input.buttons[player] = quint16(buttons_[player].load());
        for (unsigned player = 0; player < MaxGunPlayers; ++player) {
            if(host.controllerKind(player)==ControllerType::Mouse) {
                const auto pending=mouseSnapshots[player]=mouseMotion_[player].load();
                input.aim[player] = quint16(qint16(std::clamp(int(qint16(pending)),-255,255))) |
                    (quint64(quint16(qint16(std::clamp(int(qint16(pending>>16)),-255,255))))<<16);
            }
            else input.aim[player] = gunPositions_[player].load();
        }
        return input;
    };
    auto consumeMouseInput = [&](const MovieInput& input) {
        for(unsigned player=0;player<MaxGunPlayers;++player) if(host.controllerKind(player)==ControllerType::Mouse) {
            auto& motion=mouseMotion_[player];
            auto previous=motion.load();
            // A focus/topology reset invalidates this sample; never subtract it from new input.
            while((previous>>32)==(mouseSnapshots[player]>>32)) {
                const int x=std::clamp(int(qint16(previous))-int(qint16(input.aim[player])),-32767,32767);
                const int y=std::clamp(int(qint16(previous>>16))-int(qint16(input.aim[player]>>16)),-32767,32767);
                const quint64 next=(previous&0xffffffff00000000ULL)|quint16(qint16(x))|(quint64(quint16(qint16(y)))<<16);
                if(motion.compare_exchange_weak(previous,next))break;
            }
        }
    };
    auto cheatStatus = [&] {
        emit cheatsChanged(host.cheats(),int(host.cheatPlatform()),host.cheatsSupported(),host.cheatSupportText());
    };
    bool lastDiscEjected = false;
    auto discStatus = [&] {
        lastDiscEjected = host.discEjected();
        emit discStateChanged(host.discPaths(),host.discIndex(),lastDiscEjected);
    };
    RewindBuffer history;
    bool historyEnabled = rewindEnabled_.load();
    int historyMemoryMiB = rewindMemoryMiB_.load();
    bool historyHealthy = true;
    unsigned captureFrame = 0;
    auto clearHistory = [&] {
        history.setByteLimit(qsizetype(historyMemoryMiB) * 1024 * 1024);
        history.configure(historyEnabled);
        historyHealthy = true;
        captureFrame = 0;
    };
    clearHistory();
    bool paused = false, effectivePaused = false, rewindLatched = false, rewinding = false;
    bool masterSystem = false;
    LibretroHost::DebugCapabilities debugAvailable;
    const auto updateDebugControls = [&] {
        const auto available = host.debugCapabilities();
        if (available.video != debugAvailable.video || available.audio != debugAvailable.audio) {
            debugAvailable = available;
            emit debugControlsChanged(available.video,available.audio);
        }
    };
    auto rewindStatus = [&](bool enabled) {
        if (rewinding != enabled) { rewinding = enabled; emit rewindingChanged(enabled); }
    };
    auto nowNs = [] { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); };
    qint64 rewindTotalMs = 0, rewindLastProgressMs = 0, nextRewindProgressNs = 0;
    double rewindStepMs = 0;
    auto rewindAvailableMs = [&] {
        const size_t checkpoints = history.size();
        return qint64(std::llround(double(checkpoints ? checkpoints-1 : 0) * rewindStepMs));
    };
    PerformanceMetrics metrics;
    metrics.reset(nowNs());
    bool performanceMetricsEnabled = false, gameFpsEnabled = false;
    bool metricsWindowRunning = false;
    auto resetMetrics = [&] {
        metrics.reset(nowNs());
        if (performanceMetricsEnabled || gameFpsEnabled)
            emit performanceMetricsChanged(0.0, 0.0, 0.0, false);
    };
    FramePacer pacer;
    FrameWaiter waiter;
    pacer.reset(nowNs());
    auto clearAudio = [&] { audioClockAvailable_.store(false); emit audioReset(); };
    NetRollbackSession networkTimeline;
    bool networkQualityVisible=false;
    QJsonArray networkRoster;
    bool networkStepAnnounced=false, frameRequested=false;
    int networkMode=0, loadedArchiveIndex=-1;
    bool networkLinkCableRequested=false;
    unsigned networkSlots=0;
    quint64 networkEpoch=0, networkPosition=0;
    bool networkDirty=false, networkReady=false, networkRemoteRewinding=false;
    QJsonObject lastNetworkConfiguration;
    qint64 nextNetworkRewind=0,lastNetworkRewindRemaining=-1;
    QImage networkRewindImage;
    QByteArray networkRewindLinkPreview;
    double networkRewindAspect=4.0/3.0;
    auto networkRewindPreview=[&](qint64 remaining,qint64 total) {
        if(networkMode!=1 || networkRoster.size()<2 || networkPreviewPending_.exchange(true))return false;
        QByteArray geometry=networkRewindLinkPreview;
        const auto preview=encodeRewindPreview(networkRewindImage,geometry);
        QJsonObject metadata{{"epoch",QString::number(networkEpoch)},
            {"remaining",remaining},{"total",total},{"aspect",networkRewindAspect}};
        if(!geometry.isEmpty())metadata.insert("linkPreview",QString::fromLatin1(geometry.toBase64()));
        const auto json=QJsonDocument(metadata).toJson(QJsonDocument::Compact);
        QByteArray packet;packet.reserve(4+json.size()+preview.size());
        const auto size=qToBigEndian(quint32(json.size()));packet.append(reinterpret_cast<const char*>(&size),4);
        packet+=json;packet+=preview;emit networkRewindPreviewReady(packet);return true;
    };
    auto networkSnapshot=[&] {
        if(networkMode!=1 || !host.loaded()) return;
        if(networkSnapshotPending_.exchange(true)) {networkDirty=true;return;}
        const auto state=host.captureNetworkState();
        if(state.isEmpty()) {
            networkSnapshotPending_.store(false);networkDirty=false;networkWaiting_.store(true);
            emit networkModeFailed(QStringLiteral("Не удалось синхронизировать состояние игры."));return;
        }
        ++networkEpoch;
        auto configuration=host.movieConfiguration();
        configuration.insert("networkCompatibility",QString::fromLatin1(host.networkCompatibility().toHex()));
        QString startError;
        if(!networkTimeline.start(host,networkEpoch,networkRoster,0,
            [this](quint64 peer,const QByteArray& packet){if(networkQueuedDatagrams_.fetch_add(1)<512)emit networkDatagramReady(peer,packet);else networkQueuedDatagrams_.fetch_sub(1);},startError)) {
            networkSnapshotPending_.store(false);networkDirty=false;
            emit networkModeFailed(startError);return;
        }
        networkStepAnnounced=false;networkReady=true;
        QJsonObject metadata{{"epoch",QString::number(networkEpoch)},{"configuration",configuration},{"roster",networkRoster},
            {"paused",paused},{"fast",fastForward_.load()},{"factor",fastForwardFactor_.load()},
            {"rewinding",rewind_.load()},{"rewindRemaining",rewindLastProgressMs},{"rewindTotal",rewindTotalMs},{"slots",int(host.occupiedStateSlots())}};
        QImage snapshotImage=!networkRewindImage.isNull()?networkRewindImage:host.frame();
        QByteArray snapshotGeometry=networkRewindLinkPreview;
        if(host.linkCableEnabled() && networkRewindImage.isNull())
            snapshotImage=combineLinkPreview(host.frameForLinkRole(1),host.aspectRatioForLinkRole(1),
                host.frameForLinkRole(2),host.aspectRatioForLinkRole(2),snapshotGeometry);
        if(!snapshotImage.isNull()) {
            QByteArray preview;QBuffer buffer(&preview);buffer.open(QIODevice::WriteOnly);
            if(snapshotImage.save(&buffer,"PNG") && preview.size()<=2*1024*1024) {
                metadata.insert("image",QString::fromLatin1(preview.toBase64()));
                metadata.insert("aspect",!networkRewindImage.isNull()?networkRewindAspect:host.aspectRatio());
                if(!snapshotGeometry.isEmpty())metadata.insert("linkPreview",QString::fromLatin1(snapshotGeometry.toBase64()));
            }
        }
        const auto json=QJsonDocument(metadata).toJson(QJsonDocument::Compact);
        QByteArray packet; const auto n=qToBigEndian(quint32(json.size()));
        packet.append(reinterpret_cast<const char*>(&n),4); packet+=json; packet+=qCompress(state,1);
        networkPosition=0; networkDirty=false; emit networkSnapshotReady(packet);
    };
    auto lastBatteryCheck = Clock::now();
    bool optionsPending = false, fastLatched = false;
    qint64 nextPresentationNs = 0;
    qint64 nextMovieStatusNs = 0, nextHeldFrameNs = 0;
    bool stepHeldLatched = false;
    auto slotStatus = [&](int selected = -1) {
        emit stateSlotsChanged(selected, networkMode==2?networkSlots:movie.active() ? movie.occupiedSlots() : host.occupiedStateSlots());
    };
    auto movieStatus = [&](bool force = false) {
        if (force || (movie.active() && nowNs() >= nextMovieStatusNs)) {
            emit movieStatusChanged(int(movie.mode()), movie.path(), movie.position(), movie.length(), movie.frameRate());
            nextMovieStatusNs = nowNs() + 100000000;
        }
    };
    auto movieBoundary = [&] {
        emit movieHardwareChanged(movie.controllers());
        movieStatus(true); slotStatus();
        clearHistory(); clearAudio(); rewind_.store(false); rewindStatus(false);
        frameAdvanceHeld_.store(false); stepHeldLatched = false;
        pacer.reset(nowNs()); nextPresentationNs = 0;
        resetMetrics();
    };
    auto beforeFrame = [&] {
        QString error;
        if(networkMode) {
            MovieInput input;
            if(!networkReady)return false;
            const auto local=liveInput();
            const auto result=networkTimeline.beforeFrame(local,input,error);
            if(networkTimeline.localInputConsumed())consumeMouseInput(local);
            if(result==NetRollbackSession::Wait) {std::this_thread::sleep_for(std::chrono::milliseconds(1));return false;}
            if(result==NetRollbackSession::Failed) {
                networkReady=false;
#ifdef KEGA_NETPLAY_TEST_INPUTS
                qWarning("GGPO before-frame failure: %s",qPrintable(error));
#endif
                emit networkResyncRequested();return false;
            }
#ifdef KEGA_NETPLAY_TEST_INPUTS
            emit networkFrameReady(NetplayTimeline::frameBytes(networkEpoch,networkPosition,input));
#endif
            NetplayTimeline::apply(host,input); return true;
        }
        const auto local=liveInput();
        if (movie.beforeFrame(local, error)) {consumeMouseInput(local);return true;}
        paused = true; frameAdvanceHeld_.store(false);
        clearAudio(); emit pausedChanged(true);
        if (movie.recording() && !error.isEmpty()) {
            QString saveError;
            if (movie.finish(false,saveError)) {
                achievementSuppressed = false; achievements.reset(); achievements.refresh();
                movieBoundary();
                error += Localization::text(QStringLiteral("\nЗаписанная часть записи сохранена; игра оставлена на паузе."));
            } else error += QStringLiteral("\n") + saveError;
        }
        if (!error.isEmpty()) emit errorOccurred(error);
        return false;
    };
    auto afterFrame = [&] {
        if(networkMode) {
            QString error;
            if(!networkTimeline.afterFrame(error)){
                networkReady=false;
#ifdef KEGA_NETPLAY_TEST_INPUTS
                qWarning("GGPO after-frame failure: %s",qPrintable(error));
#endif
                emit networkResyncRequested();
            }
            ++networkPosition;return;
        }
        movie.afterFrame();
        if (movie.mode() == MovieSession::Finished) {
            paused = true; frameAdvanceHeld_.store(false);
            clearAudio(); emit pausedChanged(true); movieStatus(true);
            emit actionNotice(Localization::text(QStringLiteral("Воспроизведение завершено")));
        } else movieStatus();
    };
    auto captureHistory = [&] {
        const auto state = movie.active() ? movie.captureRewind() : host.captureRewindState();
        const auto progress = achievementSuppressed ? QByteArray() : achievements.saveProgress(false);
        if (progress.isEmpty() || state.isEmpty()) return state;
        QByteArray wrapped("KRACHRW1",8);
        for (quint32 length : {quint32(state.size()),quint32(progress.size())}) {
            const auto encoded = qToBigEndian(length);
            wrapped.append(reinterpret_cast<const char*>(&encoded),4);
        }
        wrapped += state; wrapped += progress;
        return wrapped;
    };
    auto restoreHistory = [&](const QByteArray& packed, QString& error) {
        if(networkMode && packed.startsWith("KRGGORW1")) {
            networkRewindLinkPreview.clear();return host.restoreMovieState(packed.mid(8),error,true);
        }
        if(networkMode && packed.startsWith("KRGGORW2")) {
            LinkPreviewGeometry geometry;
            const auto bytes=packed.mid(8,LinkPreviewBytes);
            if(packed.size()<=8+LinkPreviewBytes || !linkPreviewGeometry(bytes,geometry))return false;
            if(!host.restoreMovieState(packed.mid(8+LinkPreviewBytes),error,true))return false;
            networkRewindLinkPreview=bytes;return true;
        }
        QByteArray state = packed, progress;
        if (packed.startsWith("KRACHRW1")) {
            if (packed.size() < 16) return false;
            const auto stateSize = qFromBigEndian<quint32>(packed.constData()+8);
            const auto progressSize = qFromBigEndian<quint32>(packed.constData()+12);
            if (progressSize > 2*1024*1024 || quint64(stateSize)+progressSize+16 != quint64(packed.size())) return false;
            state = packed.mid(16,stateSize); progress = packed.mid(16+stateSize,progressSize);
        }
        const bool ok = movie.active() ? movie.restoreRewind(state,error) : host.restoreRewindState(state);
        if (ok) achievements.restoreProgress(progress,true);
        return ok;
    };
    auto recordHistory = [&] {
        if(networkMode==1) {
            NetRollbackSession::RewindFrame frame;
            while(networkTimeline.takeConfirmedHistory(frame)) {
                QImage image=frame.image;QByteArray geometry;
                if(host.linkCableEnabled()) {
                    // Only confirmed history checkpoints retain both console previews.
                    if(frame.secondImage.isNull())continue;
                    image=combineLinkPreview(frame.image,frame.aspect,frame.secondImage,frame.secondAspect,geometry);
                    if(image.isNull())return false;
                }
                QByteArray packed(geometry.isEmpty()?"KRGGORW1":"KRGGORW2",8);
                packed.reserve(8+geometry.size()+frame.state.size());packed+=geometry;packed+=frame.state;
                if(!history.capture(packed,image,frame.aspect))return false;
            }
            return true;
        }
        if(++captureFrame%2)return true;
        return history.capture(captureHistory(),host.frame(),host.aspectRatio());
    };
    while (!quitting_.load()) {
        std::deque<Command> pending;
        { std::lock_guard<std::mutex> lock(mutex_); pending.swap(commands_); }
        if(!networkMode)frameRequested=false;
        for (const auto& command : pending) {
            QString error;
            if(networkMode && (command.type==Type::Load || command.type==Type::Close))networkTimeline.stop();
            if(networkMode && (command.type==Type::MovieRecord || command.type==Type::MoviePlay ||
                command.type==Type::MovieResume || command.type==Type::MovieFork || command.type==Type::Disc)) {
                emit actionNotice(QStringLiteral("Завершите онлайн-сессию перед этим действием.")); continue;
            }
            if(networkMode==1 && command.networkAuthorized && command.type!=Type::Save && command.type!=Type::NetStepHold && command.type!=Type::FrameAdvance && command.type!=Type::NetReverse &&
               command.type!=Type::Port && command.type!=Type::ControllerType && command.type!=Type::ControllerKind)
                networkDirty=true;
            if (achievementHardcore) {
                const bool prohibited = command.type == Type::Restore || command.type == Type::FrameAdvance ||
                    command.type == Type::MoviePlay || command.type == Type::MovieResume || command.type == Type::MovieFork ||
                    command.type == Type::Cheats || command.type == Type::Region ||
                    (command.type == Type::Checksum && command.value) ||
                    (command.type == Type::CdLoading && command.value) ||
                    (command.type == Type::SpriteLimit && command.value);
                if (prohibited) {
                    achievementStatusOverride = Localization::text(QStringLiteral("Это действие недоступно в Hardcore. Переключите режим в настройках достижений."));
                    achievements.refresh(); continue;
                }
            }
            if (movie.active()) {
                // Recorded hardware/logic is fixed; pause, speed, input bindings and rewind remain live.
                bool blocked = false;
                switch (command.type) {
                case Type::Port:
                    blocked = host.controllerConnected(unsigned(command.value & 7)) != bool(command.value & 8);
                    if (!blocked) continue;
                    break;
                case Type::ControllerType:
                    blocked = host.controllerSixButton(unsigned(command.value & 7)) != bool(command.value & 8);
                    if (!blocked) continue;
                    break;
                case Type::ControllerKind:
                    blocked = host.controllerKind(unsigned(command.value & 7)) != ControllerType(command.value >> 3);
                    if (!blocked) continue;
                    break;
                case Type::Adapter:
                    blocked = host.controllerAdapter() != ControllerAdapter(command.value);
                    if (!blocked) continue;
                    break;
                case Type::Reset: case Type::HardReset: case Type::Region:
                case Type::CdLoading: case Type::SpriteLimit: case Type::Checksum: case Type::Cheats:
                case Type::Disc: case Type::DiscIndex: case Type::Eject: blocked = true; break;
                default: break;
                }
                if (blocked) {
                    emit actionNotice(Localization::text(QStringLiteral("Сначала завершите запись: параметры консоли зафиксированы в записи.")));
                    continue;
                }
            }
            switch (command.type) {
            case Type::NetMode: {
                networkTimeline.stop();
                if(movie.active() && !movie.finish(false,error)) {networkMode_.store(networkMode);emit networkModeFailed(error);break;}
                if(command.value==0) {
                    // Do not allow a later offline unload to write the online battery into solo saves.
                    if(networkMode && host.loaded()) { host.unload(); loaded_.store(false); emit gameClosed(); }
                    if(!host.setLinkCableEnabled(false,error))emit actionNotice(error);
                    networkLinkCableRequested=false;
                    {
                        std::lock_guard<std::mutex> lock(videoMutex_);
                        latestFrame_={};videoPending_=false;latestAspect_=4.0/3.0;
                    }
                    host.restoreMovieConfiguration(error);
                    if(!host.setNetplayStorage({},error))emit errorOccurred(error);
                } else if(!host.setNetplayStorage(command.path,error)) {
                    networkMode_.store(networkMode);loaded_.store(host.loaded());
                    if(!host.loaded())emit gameClosed();
                    emit networkModeFailed(error);break;
                }
                if(command.value==1 && !host.setLinkCableEnabled(networkLinkCableRequested,error)) {
                    networkLinkCableRequested=host.linkCableEnabled();emit actionNotice(error);
                }
                host.setLinkPresentationRole(networkRole_.load());
                if(networkLinkCableDesired_.load()==networkLinkCableRequested)emit networkLinkCableChanged(host.linkCableEnabled());
                emit movieHardwareChanged({});
                networkMode=command.value; networkReady=networkMode==1; networkDirty=networkMode==1;
                networkRoster=networkMode==1?QJsonArray{QJsonObject{{"id","0"},{"role",networkRole_.load()}}}:QJsonArray{};
                networkEpoch=networkPosition=0; networkRemoteRewinding=false;rewindStatus(false);networkRewindImage={};networkRewindLinkPreview.clear(); networkTimeline.stop(); networkStepAnnounced=false; frameRequested=false; lastNetworkConfiguration={};
                achievementSuppressed=networkMode!=0;
                achievements.unloadGame(); configureAchievements();
                if(!networkMode && host.loaded()) loadAchievements();
                clearHistory(); clearAudio(); fastForward_.store(false); rewind_.store(false);
                pacer.reset(nowNs());
                if(networkMode && host.loaded()) emit networkGameSource(host.gamePath(),loadedArchiveIndex);
                break;
            }
            case Type::NetRole:
                host.setLinkPresentationRole(command.value);
                clearAudio();
                if(host.loaded() && !host.frame().isNull() && networkRewindImage.isNull())publishVideo(host.frame(),host.aspectRatio());
                break;
            case Type::NetLinkCable: {
                const bool requested=command.value!=0;
                networkLinkCableRequested=requested;
                if(networkMode!=1)break; // Clients apply hardware atomically with the authenticated snapshot.
                if(host.linkCableEnabled()!=networkLinkCableRequested) {
                    networkTimeline.stop();networkReady=false;
                    if(!host.setLinkCableEnabled(networkLinkCableRequested,error)) {
                        networkLinkCableRequested=host.linkCableEnabled();emit actionNotice(error);
                    }
                    host.setLinkPresentationRole(networkRole_.load());
                    networkRewindImage={};networkRewindLinkPreview.clear();networkRemoteRewinding=false;
                    rewind_.store(false);rewindStatus(false);frameRequested=false;networkStepAnnounced=false;
                    clearHistory();clearAudio();resetMetrics();pacer.reset(nowNs());
                    networkDirty=true;lastNetworkConfiguration={};
                    loaded_.store(host.loaded());
                    if(!host.loaded())emit gameClosed();
                    if(host.loaded()) {
                        emit stateSlotsChanged(-1,host.occupiedStateSlots());
                        if(!host.frame().isNull())publishVideo(host.frame(),host.aspectRatio());
                    }
                }
                networkDirty=true; // Readiness was cleared by the lobby even when requests coalesced.
                if(networkLinkCableDesired_.load()==requested)emit networkLinkCableChanged(host.linkCableEnabled());
                break;
            }
            case Type::NetSnapshot: if(networkMode==1) networkDirty=true; break;
            case Type::NetRoster: {
                const auto roster=QJsonDocument::fromJson(command.bytes).array();
                if(networkMode==1 && !roster.isEmpty() && roster.size()<=32 && roster!=networkRoster) {
                    networkRoster=roster;networkDirty=true;
                }
                break;
            }
            case Type::NetDatagram: networkTimeline.receive(command.requestId,command.bytes);break;
            case Type::NetAdvance: if(networkMode==2 && paused)frameRequested=true;break;
            case Type::NetReceiveFrame: break; // Obsolete TCP frame packets are never executed.
            case Type::NetReceiveState: {
                if(networkMode!=2 || !host.loaded()) { emit networkSnapshotApplied(false,QStringLiteral("Сначала откройте игру хоста.")); break; }
                const auto& b=command.bytes;
                bool ok=false;
                if(b.size()>=8) {
                    const quint32 n=qFromBigEndian<quint32>(b.constData());
                    if(n<=4*1024*1024 && quint64(n)+8<=quint64(b.size()) && qFromBigEndian<quint32>(b.constData()+4+n)<=128*1024*1024) {
                        const auto meta=QJsonDocument::fromJson(b.mid(4,n)).object();
                        const auto config=meta.value("configuration").toObject();
                        bool epochValid=false; const auto epoch=meta.value("epoch").toString().toULongLong(&epochValid);
                        const auto state=LibretroHost::unpackNetworkState(b.mid(4+n));
                        if(epochValid && epoch && !state.isEmpty()) {
                            const auto current=host.movieConfiguration();
                            if(command.requestId!=networkLinkCableRevision_.load() || config.value("linkCable").toBool()!=networkLinkCableDesired_.load()) {
                                emit networkSnapshotDiscarded();break;
                            }
                            if(config.value("coreVersion")!=current.value("coreVersion"))
                                error=QStringLiteral("Для онлайн-игры нужны одинаковые версии Kega Refusion и совместимые ядра.");
                            else {
                                networkTimeline.stop();networkReady=false;
                                const bool prepared=config==lastNetworkConfiguration;
                                lastNetworkConfiguration={};
                                ok=prepared || host.prepareNetwork(config,error);
                                if(ok && (host.networkCompatibility().isEmpty() || config.value("networkCompatibility").toString()!=QString::fromLatin1(host.networkCompatibility().toHex()))) {
                                    error=QStringLiteral("Для онлайн-игры нужны одинаковые версии Kega Refusion и совместимые ядра.");ok=false;
                                }
                                if(ok) ok=host.restoreNetworkState(state,error);
                                if(ok && (command.requestId!=networkLinkCableRevision_.load() || config.value("linkCable").toBool()!=networkLinkCableDesired_.load())) {
                                    emit networkSnapshotDiscarded();break;
                                }
                                if(ok) {
                                    networkRoster=meta.value("roster").toArray();
                                    ok=networkTimeline.start(host,epoch,networkRoster,networkPeerId_.load(),
                                        [this](quint64 peer,const QByteArray& packet){if(networkQueuedDatagrams_.fetch_add(1)<512)emit networkDatagramReady(peer,packet);else networkQueuedDatagrams_.fetch_sub(1);},error);
                                }
                            }
                            if(ok) {
                                lastNetworkConfiguration=config; networkEpoch=epoch; networkPosition=0;
                                host.setLinkPresentationRole(networkRole_.load());
                                if(config.value("linkCable").toBool()==networkLinkCableDesired_.load())emit networkLinkCableChanged(host.linkCableEnabled());
                                networkReady=true; networkWaiting_.store(false); frameRequested=false; networkStepAnnounced=false;
                                paused=meta.value("paused").toBool();
                                networkRemoteRewinding=meta.value("rewinding").toBool();
                                rewindStatus(networkRemoteRewinding);
                                if(networkRemoteRewinding)emit rewindProgress(meta.value("rewindRemaining").toInteger(),meta.value("rewindTotal").toInteger());
                                fastForward_.store(meta.value("fast").toBool());
                                fastForwardFactor_.store(std::clamp(meta.value("factor").toInt(4),0,32));
                                networkSlots=unsigned(meta.value("slots").toInt())&0x3ff;
                                emit pausedChanged(paused);emit stateSlotsChanged(-1,networkSlots);
                                emit movieHardwareChanged(QJsonDocument(QJsonObject{{"adapter",config.value("adapter")},{"controllers",config.value("controllers")},{"jcart",config.value("jcart")},{"jcartBase",config.value("jcartBase")}}).toJson(QJsonDocument::Compact));
                                clearHistory(); clearAudio(); pacer.reset(nowNs()); cheatStatus(); discStatus();
                                QByteArray preview=QByteArray::fromBase64(meta.value("image").toString().toLatin1());
                                if(!preview.isEmpty()) {
                                QBuffer imageBuffer(&preview);imageBuffer.open(QIODevice::ReadOnly);
                                QImageReader reader(&imageBuffer,"PNG");const auto size=reader.size();
                                if(size.isValid()&&size.width()<=2048&&size.height()<=2048) {
                                    const auto image=reader.read();double aspect=meta.value("aspect").toDouble(4.0/3.0);
                                    const auto selected=selectLinkPreview(image,QByteArray::fromBase64(meta.value("linkPreview").toString().toLatin1()),networkRole_.load(),aspect);
                                    if(!selected.isNull())publishVideo(selected,aspect);
                                }
                                }
                            }
                        }
                    }
                }
                if(!ok) { networkReady=false; if(error.isEmpty()) error=QStringLiteral("Повреждено состояние онлайн-игры."); }
                emit networkSnapshotApplied(ok,error); break;
            }
            case Type::NetRewindPreview: {
                if(networkMode!=2 || !host.loaded())break;
                const auto& packet=command.bytes;
                const quint32 size=qFromBigEndian<quint32>(packet.constData());
                if(size>1024 || quint64(size)+4>quint64(packet.size()))break;
                const auto meta=QJsonDocument::fromJson(packet.mid(4,size)).object();
                const auto remaining=meta.value("remaining").toInteger(-1),total=meta.value("total").toInteger(-1);
                if(meta.value("epoch").toString().toULongLong()!=networkEpoch || remaining<0 || total<remaining)break;
                if(!networkRemoteRewinding) {networkRemoteRewinding=true;networkTimeline.stop();clearAudio();}
                rewindStatus(true);emit rewindProgress(remaining,total);
                QByteArray preview=packet.mid(4+size);
                if(!preview.isEmpty()) {
                    QBuffer buffer(&preview);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer);
                    const auto dimensions=reader.size();
                    if(dimensions.isValid() && dimensions.width()<=2048 && dimensions.height()<=2048) {
                        const auto image=reader.read();double aspect=meta.value("aspect").toDouble(4.0/3.0);
                        const auto selected=selectLinkPreview(image,QByteArray::fromBase64(meta.value("linkPreview").toString().toLatin1()),networkRole_.load(),aspect);
                        if(!selected.isNull() && std::isfinite(aspect) && aspect>0 && aspect<=8)publishVideo(selected,aspect);
                    }
                }
                break;
            }
            case Type::NetSlots: networkSlots=unsigned(command.value)&0x3ff;slotStatus();break;
            case Type::SmsFm: host.setSmsFmEnabled(command.value!=0); break;
            case Type::NetFactor: fastForwardFactor_.store(command.value==0?0:std::clamp(command.value,2,32)); break;
            case Type::NetStepHold: frameAdvanceHeld_.store(command.value!=0); break;
            case Type::NetFast: fastForward_.store(command.value!=0); break;
            case Type::NetReverse:
                if(rewind_.exchange(command.value!=0) && !command.value && networkMode==1)networkDirty=true;
                break;
            case Type::PerformanceMetrics:
                if (performanceMetricsEnabled != (command.value != 0)) {
                    performanceMetricsEnabled = command.value != 0;
                    resetMetrics();
                }
                break;
            case Type::GameFps:
                if (gameFpsEnabled != (command.value != 0)) {
                    gameFpsEnabled = command.value != 0;
                    resetMetrics();
                }
                break;
            case Type::Expansions:
                host.setExpansionDevices(command.value & 1, command.value & 2);
                break;
            case Type::AchievementsConfigure:
                achievementEnabled = bool(command.value & 1);
                achievementHardcoreRequested = bool(command.value & 2);
                configureAchievements();
                if (host.loaded()) loadAchievements();
                clearHistory();
                break;
            case Type::AchievementsLogin:
                achievementStatusOverride.clear();
                achievements.login(command.path,QString::fromUtf8(command.bytes));
                if (host.loaded()) loadAchievements();
                break;
            case Type::AchievementsLogout:
                achievementStatusOverride.clear(); achievements.logout(); clearHistory();
                break;
            case Type::AchievementsRefresh: achievements.refresh(); break;
            case Type::AchievementsResponse:
                achievements.respond(command.requestId,command.value,command.bytes);
                break;
            case Type::MovieRecord:
            case Type::MoviePlay:
            case Type::MovieResume:
            case Type::MovieFork: {
                const bool ok = command.type == Type::MovieRecord ? movie.startRecording(command.path,error)
                    : command.type == Type::MovieFork ? movie.rerecord(command.path,error)
                    : movie.play(command.path,command.type == Type::MovieResume,error);
                loaded_.store(host.loaded());
                if (!ok) {
                    if (!host.loaded()) emit gameClosed();
                    emit errorOccurred(error); break;
                }
                achievementSuppressed = command.type != Type::MovieRecord;
                achievements.reset(); achievements.refresh();
                releaseButtons(); movieBoundary(); optionsPending = false;
                if ((command.type == Type::MoviePlay || command.type == Type::MovieResume) &&
                    !host.movieCompatibilityWarning().isEmpty()) {
                    emit movieCompatibilityWarning(host.movieCompatibilityWarning());
                    emit notification(host.movieCompatibilityWarning());
                }
                paused = movie.mode() == MovieSession::Finished;
                emit pausedChanged(paused);
                break;
            }
            case Type::MovieStop:
            case Type::MovieTakeOver:
                if (movie.active()) {
                    if (!movie.finish(false,error)) { emit errorOccurred(error); break; }
                    achievementSuppressed = false; achievements.reset(); achievements.refresh();
                    movieBoundary(); cheatStatus(); discStatus();
                    if (command.type == Type::MovieTakeOver) { paused = false; emit pausedChanged(false); }
                } else movieStatus(true); // Acknowledge stop after a failed pending start (window-close handshake).
                break;
            case Type::Load:
                if (movie.active()) {
                    if (!movie.finish(true,error)) { emit errorOccurred(error); break; }
                    movieBoundary();
                }
                if (host.load(command.path,error,command.value,
                              command.archiveEntry ? &*command.archiveEntry : nullptr)) {
                    debugAvailable = {}; emit debugControlsChanged(0,0);
                    host.setLinkPresentationRole(networkRole_.load());
                    loaded_.store(true); releaseButtons(); paused = false;
                    achievementSuppressed = networkMode!=0; configureAchievements();
                    if(!networkMode) loadAchievements();
                    networkReady=networkMode==1; networkDirty=networkMode==1;
                    lastNetworkConfiguration={};
                    loadedArchiveIndex=command.value;
                    if(networkMode) emit networkGameSource(command.path,command.value);
                    rewind_.store(false); rewindStatus(false); clearHistory();
                    clearAudio();
                    resetMetrics();
                    emit gameLoaded(host.gamePath(),host.fps(),host.contentName());
                    emit stateSlotsChanged(-1,host.occupiedStateSlots());
                    emit pausedChanged(false);
                    pacer.reset(nowNs());
                } else {
                    if (!host.loaded()) { clearHistory(); rewindStatus(false); emit gameClosed(); }
                    else if(networkMode)emit networkGameSource(host.gamePath(),loadedArchiveIndex);
                    emit errorOccurred(error);
                }
                loaded_.store(host.loaded());
                if (!host.loaded()) { achievements.unloadGame(); achievements.refresh(); }
                discStatus(); cheatStatus();
                break;
            case Type::Close:
                if (movie.active()) {
                    if (!movie.finish(true,error)) { emit errorOccurred(error); break; }
                    movieBoundary();
                }
                // Keep the running game intact if a changed battery save cannot be written.
                if (host.loaded() && !host.flushSaveRam(error)) {
                    emit errorOccurred(error);
                    break;
                }
                achievements.unloadGame(); achievementSuppressed = networkMode!=0;
                host.unload(); achievements.refresh();
                loaded_.store(false);
                releaseButtons();
                for (auto& aim : gunPositions_) aim.store(quint64(1) << 32);
                paused = false;
                fastForward_.store(false);
                rewind_.store(false);
                rewindLatched = false;networkRemoteRewinding=false;networkRewindImage={};networkRewindLinkPreview.clear();
                fastLatched = false;
                clearHistory();
                rewindStatus(false);
                rewindTotalMs = rewindLastProgressMs = nextRewindProgressNs = 0;
                rewindStepMs = 0;
                emit rewindProgress(0,0);
                optionsPending = false;
                frameRequested = false;
                nextPresentationNs = 0;
                {
                    std::lock_guard<std::mutex> lock(videoMutex_);
                    latestFrame_ = QImage();
                    latestAspect_ = 4.0 / 3.0;
                    videoPending_ = false;
                }
                clearAudio();
                emit pausedChanged(false);
                effectivePaused = false;
                emit effectivePauseChanged(false);
                emit stateSlotsChanged(-1,0);
                discStatus();
                cheatStatus();
                lastBatteryCheck = Clock::now();
                pacer.reset(nowNs()); resetMetrics();
                emit gameClosed();
                break;
            case Type::FrameAdvance:
                if (movie.mode() == MovieSession::Finished) {
                    if (!movie.finish(false,error)) { emit errorOccurred(error); break; }
                    achievementSuppressed = false; achievements.reset(); achievements.refresh();
                    movieBoundary(); cheatStatus();
                }
                if (host.loaded() && paused && !backgroundPaused_.load() && !rewind_.load()) frameRequested = true;
                break;
            case Type::Cheats: {
                bool changed = false;
                if (!host.setCheats(command.cheats,error,&changed)) emit errorOccurred(error);
                else if (changed) { clearHistory(); clearAudio(); rewind_.store(false); rewindStatus(false); pacer.reset(nowNs()); }
                cheatStatus();
                break;
            }
            case Type::Adapter:
                if (networkMode == 2) break;
                if (host.requestedControllerAdapter() == ControllerAdapter(command.value)) break;
                if (host.setControllerAdapter(ControllerAdapter(command.value),error)) {
                    clearHistory(); clearAudio(); releaseButtons(); rewind_.store(false); rewindStatus(false);
                    pacer.reset(nowNs()); if (networkMode == 1) networkDirty = true;
                } else emit actionNotice(error);
                break;
            case Type::ControllerKind:
                if (host.setControllerKind(unsigned(command.value & 7),ControllerType(command.value >> 3))) {
                    if (networkMode == 1) networkDirty = true;
                    clearHistory(); clearAudio(); rewind_.store(false); rewindStatus(false); pacer.reset(nowNs());
                }
                break;
            case Type::Pause:
                if (command.value && achievementHardcore && !achievements.canPause()) {
                    achievementStatusOverride = Localization::text(QStringLiteral("Частые паузы: Hardcore отключён для этой сессии."));
                    achievements.configure(achievementEnabled,false); achievements.refresh();
                }
                if (!command.value && movie.mode() == MovieSession::Finished) {
                    if (!movie.finish(false,error)) { emit errorOccurred(error); break; }
                    achievementSuppressed = false; achievements.reset(); achievements.refresh();
                    movieBoundary(); cheatStatus();
                }
                paused = command.value != 0;
                clearAudio();
                emit pausedChanged(paused);
                pacer.reset(nowNs());
                break;
            case Type::Reset:
                if (host.loaded()) {
                    host.reset(); achievements.reset(); clearHistory(); clearAudio(); resetMetrics();
                    emit actionCompleted(Localization::text(QStringLiteral("Сброс")));
                    pacer.reset(nowNs());
                }
                break;
            case Type::HardReset:
                if (host.loaded()) {
                    if (host.hardReset(error)) {
                        achievements.reset();
                        clearHistory(); clearAudio(); rewindStatus(false); resetMetrics();
                        emit actionCompleted(Localization::text(QStringLiteral("Перезапуск питания")));
                        emit stateSlotsChanged(-1,host.occupiedStateSlots());
                        pacer.reset(nowNs());
                    } else emit errorOccurred(error);
                    loaded_.store(host.loaded()); discStatus();
                }
                break;
            case Type::Disc:
            case Type::DiscIndex:
            case Type::Eject: {
                const bool ok = command.type == Type::Disc ? host.changeDisc(command.path,error) :
                    command.type == Type::DiscIndex ? host.selectDisc(command.value,error) :
                    host.setDiscEjected(command.value != 0,error);
                // Failed mounts can also disturb the drive; do not retain an
                // older timeline across any attempted media operation.
                clearHistory(); clearAudio(); rewindStatus(false); pacer.reset(nowNs());
                discStatus(); cheatStatus(); emit stateSlotsChanged(-1,host.occupiedStateSlots());
                if (ok && command.type != Type::Eject) loadAchievements(true);
                if (ok) emit actionCompleted(command.type == Type::Eject ?
                    (command.value ? Localization::text(QStringLiteral("Привод открыт")) : Localization::text(QStringLiteral("Привод закрыт"))) :
                    Localization::text(QStringLiteral("Диск: %1")).arg(QFileInfo(host.discPaths().value(host.discIndex())).completeBaseName()));
                else emit errorOccurred(error);
                break;
            }
            case Type::Port:
                if (host.setControllerConnected(unsigned(command.value & 7), (command.value & 8) != 0)) {
                    if (networkMode == 1) networkDirty = true;
                    clearHistory(); clearAudio(); rewind_.store(false); rewindStatus(false);
                    pacer.reset(nowNs());
                }
                break;
            case Type::ControllerType:
                if (host.setControllerSixButton(unsigned(command.value & 7), (command.value & 8) != 0)) {
                    if (networkMode == 1) networkDirty = true;
                    clearHistory(); clearAudio(); rewind_.store(false); rewindStatus(false);
                    pacer.reset(nowNs());
                }
                break;
            case Type::SpriteLimit:
                if (host.setRemoveSpriteLimit(command.value != 0,error)) {
                    optionsPending = true; clearHistory();
                } else emit errorOccurred(error);
                break;
            case Type::CdLoading:
                if (host.setFastCdLoading(command.value != 0,error)) {
                    if (host.isCd()) { optionsPending = true; clearHistory(); }
                } else emit errorOccurred(error);
                break;
            case Type::Checksum:
                // Update the policy only. load() and an explicit hard reset
                // rebuild the in-memory ROM with this setting.
                if (!host.setAutoFixChecksum(command.value != 0,error))
                    emit errorOccurred(error);
                break;
            case Type::Region:
            case Type::Filter:
                if (command.type == Type::Region ? host.setRegion(command.value,error)
                                                 : host.setBlarggFilter(command.value,error)) {
                    if (command.type == Type::Region && host.isCd()) {
                        if (!host.hardReset(error)) emit errorOccurred(error);
                        achievements.reset();
                        loaded_.store(host.loaded()); discStatus();
                    }
                    optionsPending = true; clearHistory(); clearAudio();
                } else emit errorOccurred(error);
                break;
            case Type::Save:
                if (movie.active() ? movie.saveSlot(command.value,error) : host.saveState(command.value,error)) {
                    slotStatus();
                    emit actionCompleted(Localization::text(QStringLiteral("Сохранено: слот %1")).arg(command.value));
                } else emit errorOccurred(error);
                break;
            case Type::Restore: {
                if (movie.active()) {
                    bool missing = false; QImage frame; double aspect = host.aspectRatio();
                    if (movie.loadSlot(command.value,frame,aspect,missing,error)) {
                        achievements.reset();
                        clearHistory(); clearAudio(); resetMetrics();
                        if (!frame.isNull()) publishVideo(frame,aspect);
                        movieStatus(true);
                        emit actionCompleted(Localization::text(QStringLiteral("Загружено: слот %1")).arg(command.value));
                        pacer.reset(nowNs());
                    } else emit actionNotice(error);
                } else {
                    LibretroHost::StateLoadError failure;
                    if (host.loadState(command.value,error,&failure)) {
                        clearHistory(); clearAudio(); resetMetrics();
                        emit actionCompleted(Localization::text(QStringLiteral("Загружено: слот %1")).arg(command.value));
                        pacer.reset(nowNs());
                    } else if (failure == LibretroHost::StateLoadError::Missing) emit actionNotice(error);
                    else emit errorOccurred(error);
                }
                slotStatus();
                break;
            }
            case Type::DebugMasks: {
                const auto previousAudio = host.debugAudioMask();
                host.setDebugMasks(achievementHardcore ? 0u : quint32(command.value),command.requestId);
                if (host.debugAudioMask() != previousAudio) clearAudio();
                break;
            }
            case Type::DebugBackdrop: host.setDebugBackdrop(quint32(command.value)); break;
            case Type::DebugGain: host.setDebugAudioGain(unsigned(command.value),unsigned(command.requestId)); break;
            case Type::Slots: slotStatus(command.value); break;
            }
            switch(command.type) {
            case Type::Load: case Type::Close: case Type::HardReset: case Type::NetMode:
            case Type::Region: case Type::NetLinkCable:
            case Type::NetReceiveState: case Type::MovieRecord: case Type::MoviePlay:
            case Type::MovieResume: case Type::MovieFork: case Type::MovieStop: case Type::MovieTakeOver: {
                const bool sms=host.isMasterSystem();
                if(sms!=masterSystem){masterSystem=sms;emit masterSystemChanged(sms);}
                updateDebugControls();
                break;
            }
            default:break;
            }
        }
        if (!pending.empty()) {
            const bool jcart=host.hasJCart();
            if(jcart_.exchange(jcart)!=jcart)emit jcartChanged(jcart);
            const auto effectiveAdapter = int(host.controllerAdapter());
            const auto playerCount = host.controllerPlayerCount();
            if (controllerAdapter_.exchange(effectiveAdapter) != effectiveAdapter || controllerPlayerCount_.load() != playerCount) {
                controllerPlayerCount_.store(playerCount);
                emit controllerTopologyChanged(effectiveAdapter,playerCount);
            }
        }
        if (networkMode == 1 && networkDirty) for (int i=0;i<networkRoster.size();++i) {
            auto peer=networkRoster[i].toObject();
            if (peer.value("role").toInt() > int(host.controllerPlayerCount())) {
                peer.insert("role",0);networkRoster[i]=peer;networkDirty=true;
            }
        }
        if(networkDirty && networkMode==1 && host.loaded()) networkSnapshot();
        if(networkMode && networkTimeline.active()) {
            QString networkError;
            if(networkReady && !rewind_.load() && !networkRemoteRewinding && !networkTimeline.idle(networkError)) {
                networkReady=false;
#ifdef KEGA_NETPLAY_TEST_INPUTS
                qWarning("GGPO idle failure: %s",qPrintable(networkError));
#endif
                emit networkResyncRequested();
            }
        }
        if(networkMode && networkTimeline.active()) {
            QJsonObject quality;
            const bool realtime=networkReady && !paused && !networkWaiting_.load() &&
                !rewind_.load() && !networkRemoteRewinding && !fastForward_.load();
            if(networkTimeline.takeQuality(quality,realtime) && !networkQualityPending_.exchange(true)) {
                networkQualityVisible=true;emit networkQualityChanged(quality);
            }
        } else if(networkQualityVisible && !networkQualityPending_.exchange(true)) {
            networkQualityVisible=false;emit networkQualityChanged(QJsonObject{{"active",false}});
        }
        if(networkMode && (networkWaiting_.load() || (networkMode==2 && !networkReady))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue;
        }
        if (Clock::now() >= nextAchievementIdle) {
            achievements.idle(); nextAchievementIdle = Clock::now() + std::chrono::milliseconds(100);
        }
        if (achievementPolicyRejected) {
            achievementPolicyRejected = false; achievementResetRequested = false;
            achievements.configure(achievementEnabled,false); achievements.refresh();
        }
        if (achievementResetRequested) {
            achievementResetRequested = false;
            QString resetError;
            const bool hadGame = host.loaded();
            if (hadGame && host.hardReset(resetError)) {
                achievements.reset(); clearHistory(); clearAudio(); pacer.reset(nowNs()); resetMetrics();
            } else if (hadGame) {
                achievementStatusOverride = resetError;
                achievements.configure(achievementEnabled,false); achievements.refresh();
                loaded_.store(host.loaded());
                if (!host.loaded()) { achievements.unloadGame(); emit gameClosed(); }
            }
        }
        const bool requestedHistoryEnabled = rewindEnabled_.load() && !achievementHardcore && networkMode!=2;
        const int requestedHistoryMemoryMiB = rewindMemoryMiB_.load();
        if (historyEnabled != requestedHistoryEnabled || historyMemoryMiB != requestedHistoryMemoryMiB) {
            historyEnabled = requestedHistoryEnabled; historyMemoryMiB = requestedHistoryMemoryMiB;
            clearHistory();
        }
        const bool background = !networkMode && backgroundPaused_.load();
        if (background && !lastAchievementBackground && achievementHardcore && !achievements.canPause()) {
            achievementStatusOverride = Localization::text(QStringLiteral("Частые паузы: Hardcore отключён для этой сессии."));
            achievements.configure(achievementEnabled,false); achievements.refresh();
        }
        lastAchievementBackground = background;
        // Achievement HTTP is asynchronous and must never gate gameplay.
        // Its completion callback publishes GameReady only when data is usable.
        const bool nowPaused = host.loaded() && (paused || background);
        if (nowPaused != effectivePaused) {
            effectivePaused = nowPaused; emit effectivePauseChanged(nowPaused); clearAudio(); pacer.reset(nowNs());
        }
        const bool reverse = rewind_.load() && historyEnabled && host.loaded() && !background && !achievementHardcore && !optionsPending;
        const bool metricsRunning = (performanceMetricsEnabled || gameFpsEnabled) &&
            host.loaded() && !nowPaused && !reverse && !networkRemoteRewinding;
        if (metricsRunning != metricsWindowRunning) {
            metricsWindowRunning = metricsRunning;
            resetMetrics();
        }
        if (metricsRunning) {
            PerformanceMetrics::Snapshot sample;
            if (metrics.takeSample(nowNs(), host.fps(), gameFpsEnabled && host.supportsInputPollMetrics(), sample))
                emit performanceMetricsChanged(sample.emulatedFps, sample.speedPercent,
                                               sample.gameFps, sample.gameFpsAvailable);
        }
        if (reverse != rewindLatched) {
            rewindLatched = reverse; clearAudio(); pacer.reset(nowNs());
            if (reverse) {
                // Checkpoints are recorded every two emulated frames. Freeze
                // this hold's origin while the remaining step count decreases.
                rewindStepMs = 2000.0 / host.fps();
                rewindTotalMs = rewindAvailableMs();
                rewindLastProgressMs = rewindTotalMs;
                nextRewindProgressNs = nowNs() + 100000000;
                networkRewindImage=host.frame();networkRewindAspect=host.aspectRatio();networkRewindLinkPreview.clear();
                if(host.linkCableEnabled())networkRewindImage=combineLinkPreview(host.frameForLinkRole(1),host.aspectRatioForLinkRole(1),
                    host.frameForLinkRole(2),host.aspectRatioForLinkRole(2),networkRewindLinkPreview);
                nextNetworkRewind=0;lastNetworkRewindRemaining=-1;
                rewindStatus(true);
                emit rewindProgress(rewindTotalMs,rewindTotalMs);
            } else {rewindStatus(networkRemoteRewinding);networkRewindImage={};networkRewindLinkPreview.clear();}
        }
        if (reverse) {
            rewindStatus(true);
            QByteArray state;
            QImage frame;
            double aspect = 4.0 / 3.0;
            if (history.stepBack(state,frame,aspect)) {
                QString restoreError;
                if (restoreHistory(state,restoreError)) {
                    captureFrame = 0;
                    const auto selected=selectLinkPreview(frame,networkRewindLinkPreview,networkRole_.load(),aspect);
                    if(!selected.isNull())publishVideo(selected,aspect);
                    movieStatus();networkRewindImage=frame;networkRewindAspect=aspect;
                } else {
                    clearHistory(); historyHealthy = false;
                    emit errorOccurred(Localization::text(QStringLiteral("Ядро отклонило состояние перемотки. История очищена.")));
                }
            }
            const qint64 remainingMs = std::min(rewindTotalMs,rewindAvailableMs());
            const qint64 progressNow = nowNs();
            if (remainingMs != rewindLastProgressMs && progressNow >= nextRewindProgressNs) {
                rewindLastProgressMs = remainingMs;
                nextRewindProgressNs = progressNow + 100000000;
                emit rewindProgress(remainingMs,rewindTotalMs);
            }
            if(networkMode==1 && progressNow>=nextNetworkRewind) {
                if(remainingMs!=lastNetworkRewindRemaining && networkRewindPreview(remainingMs,rewindTotalMs))lastNetworkRewindRemaining=remainingMs;
                nextNetworkRewind=progressNow+100000000;
            }
            // Hold at the oldest checkpoint rather than inadvertently running forward.
            const auto deadline = pacer.next(nowNs(),qint64(2000000000.0 / host.fps()));
            waiter.waitUntil(deadline);
            continue;
        }
        const auto batteryNow = Clock::now();
        if (host.loaded() && !reverse &&
            batteryNow - lastBatteryCheck >= std::chrono::seconds(1)) {
            QString error;
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                batteryNow.time_since_epoch()).count();
            if (!host.pollSaveRam(milliseconds, error)) emit notification(error);
            lastBatteryCheck = batteryNow;
        }
        const bool stepHeld = networkMode!=2 && frameAdvanceHeld_.load() && host.loaded() && paused && !background && !achievementHardcore;
        if (stepHeld && !stepHeldLatched) nextHeldFrameNs = nowNs() + 350000000;
        if (stepHeld && stepHeldLatched && nowNs() >= nextHeldFrameNs) {
            frameRequested = true; nextHeldFrameNs = nowNs() + 33333333;
        }
        stepHeldLatched = stepHeld;
        if (frameRequested && host.loaded() && paused && !background && !achievementHardcore && !networkRemoteRewinding) {
            if(networkMode==1 && !networkStepAnnounced) {
                networkStepAnnounced=true;emit networkStepRequested(networkEpoch);
            }
            networkTimeline.setHistoryEnabled(historyEnabled&&networkMode==1);
            if (!beforeFrame()) continue;
            frameRequested=false;networkStepAnnounced=false;
            host.runFrame(true,false); // Exactly one video frame; no burst of queued sound.
            if (!achievementSuppressed) achievements.frame();
            afterFrame();
            if (host.isCd() && host.discEjected() != lastDiscEjected) { clearHistory(); discStatus(); cheatStatus(); }
            if (optionsPending) { optionsPending = false; clearHistory(); }
            if (historyEnabled && historyHealthy && !recordHistory()) {
                history.clear(); historyHealthy = false;
            }
            publishVideo(host.frame(),host.aspectRatio());
            clearAudio(); pacer.reset(nowNs());
            continue;
        }
        if (!host.loaded() || nowPaused || networkRemoteRewinding) { std::this_thread::sleep_for(std::chrono::milliseconds(4)); continue; }
        const bool fast = fastForward_.load();
        if (fast != fastLatched) {
            fastLatched = fast; clearAudio(); pacer.reset(nowNs()); nextPresentationNs = 0;
            resetMetrics();
            // Fast-forward creates a gap: discard the old timeline so rewind
            // cannot jump across it when normal recording resumes.
            if (fast) clearHistory();
        }
        const int factor = fast ? fastForwardFactor_.load() : 1;
        const bool finalMovieFrame = movie.readOnly() && movie.position() + 1 >= movie.length();
        const bool present = !fast || finalMovieFrame || nowNs() >= nextPresentationNs;
        networkTimeline.setHistoryEnabled(historyEnabled&&networkMode==1&&!fast);
        if (!beforeFrame()) continue;
        // Fast-forward omits unseen rendering, PCM copies and rewind recording.
        // Emulated CPU/audio state still advances normally.
        host.runFrame(!fast || present || optionsPending, !fast);
        if (metricsWindowRunning) metrics.recordFrame(gameFpsEnabled && host.frameInputPolled());
        if (!achievementSuppressed) achievements.frame();
        afterFrame();
        if (host.isCd() && host.discEjected() != lastDiscEjected) {
            clearHistory(); discStatus();
        }
        if (optionsPending) {
            optionsPending = false; clearHistory(); clearAudio(); pacer.reset(nowNs());
        }
        if (!fast && historyEnabled && historyHealthy) {
            if (!recordHistory()) {
                history.clear(); historyHealthy = false;
                emit notification(Localization::text(QStringLiteral("История перемотки недоступна для текущей игры.")));
            }
        }
        if (present) {
            publishVideo(host.frame(),host.aspectRatio());
            nextPresentationNs = nowNs() + 16666667; // Fast-forward display stays near60Hz, independent of emulation speed.
        }
        if (!fast && !paused && !host.audio().isEmpty()) emit audioSamples(host.audio(),host.sampleRate());
        const double clockScale = fast ? 1.0 : pacer.audioPeriodScale(nowNs(),audioFeedbackNs_.load(),
            audioQueueUs_.load(),audioTargetUs_.load(),audioClockAvailable_.load());
        if (factor != 0) {
            const auto deadline = pacer.next(nowNs(),qint64(1000000000.0 * clockScale / (host.fps() * factor)));
            waiter.waitUntil(deadline);
        } else pacer.reset(nowNs());
    }
    QString movieError;
    if (movie.active()) {
        if (!movie.finish(true,movieError)) emit errorOccurred(movieError);
    }
    achievements.unloadGame();
    host.captureAchievementProgress = {}; host.restoreAchievementProgress = {};
    networkTimeline.stop();
    if(networkQualityVisible)emit networkQualityChanged(QJsonObject{{"active",false}});
    host.unload();
    loaded_.store(false);
}
