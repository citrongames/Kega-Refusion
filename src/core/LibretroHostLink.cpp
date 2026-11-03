#include "LibretroHost.h"
#include "LibretroHostApi.h"
#include "i18n/Localization.h"
#include <libco.h>
#include <QFileInfo>
#include <QDir>
#include <QtEndian>
#include <array>
#include <limits>
#include <cstring>
#include <cmath>
#include <exception>
#include <stdexcept>

namespace {
constexpr qsizetype PairHeader = 48;
constexpr quint64 PairLimit = 64ull*1024*1024;
constexpr unsigned StackBytes = 512*1024;
// Instruction-boundary lockstep first; larger quanta require measured cable
// edge equivalence. No coroutine or callback runs in ordinary emulation.
constexpr unsigned Quantum = 0;
void append32(QByteArray& out,quint32 value) {
    const auto be=qToBigEndian(value);out.append(reinterpret_cast<const char*>(&be),4);
}
}
struct LibretroHost::LinkPair {
    struct Endpoint { LinkPair* pair; unsigned side; } endpoint[2]{{this,0},{this,1}};
    LibretroHost* host[2];
    cothread_t control=nullptr, fiber[2]{};
    unsigned cycles[2]{};
    bool done[2]{true,true};
    std::exception_ptr failure;
    static thread_local LinkPair* current;
    LinkPair(LibretroHost* a,LibretroHost* b):host{a,b} {}
    ~LinkPair() { for(auto f:fiber)if(f)co_delete(f); }
    static void entry0(){current->worker(0);}
    static void entry1(){current->worker(1);}
    void worker(unsigned side) {
        for(;;) {
            try {host[side]->api_->run();}
            catch(...) {failure=std::current_exception();}
            done[side]=true;co_switch(control);
        }
    }
    bool initialize() {
        control=co_active();
        fiber[0]=co_create(StackBytes,entry0);fiber[1]=co_create(StackBytes,entry1);
        return fiber[0]&&fiber[1];
    }
    static void yield(void* opaque,unsigned cycle) {
        const auto& e=*static_cast<Endpoint*>(opaque);auto& p=*e.pair;
        p.cycles[e.side]=cycle;
        if(p.done[1-e.side]) {p.host[e.side]->api_->link_deadline(~0u);return;}
        co_switch(p.control);
    }
    void connect() {
        for(unsigned i=0;i<2;++i)
            host[i]->api_->link_connect(host[1-i]->api_->link_io(),yield,&endpoint[i]);
    }
    void run(bool video,bool audio) {
        auto* previous=current;current=this;failure=nullptr;
        cycles[0]=cycles[1]=0;done[0]=done[1]=false;
        for(unsigned i=0;i<2;++i) {
            host[i]->audio_.clear();host[i]->videoEnabled_=video;
            // Advance both sound chips/resamplers identically; retain PCM only
            // for the presented console. The unpresented callback discards it.
            host[i]->audioEnabled_=audio && (host[0]->linkPresentationRole_==int(i+1));
            host[i]->api_->link_deadline(Quantum);
        }
        unsigned last=1;
        while(!done[0]||!done[1]) {
            const unsigned side=done[0]?1:done[1]?0:
                cycles[0]==cycles[1]?1-last:cycles[0]<cycles[1]?0:1;
            last=side;
            host[side]->api_->link_deadline(done[1-side]?~0u:cycles[1-side]+Quantum);
            active_=host[side];co_switch(fiber[side]);
            if(failure)break;
        }
        active_=host[0];current=previous;
        for(auto* h:host)h->videoEnabled_=h->audioEnabled_=true;
        if(failure)std::rethrow_exception(failure);
    }
};
thread_local LibretroHost::LinkPair* LibretroHost::LinkPair::current=nullptr;
void LibretroHost::LinkPairDeleter::operator()(LinkPair* p)const {delete p;}

const QImage& LibretroHost::frame() const {return frameForLinkRole(linkPresentationRole_);}
const QByteArray& LibretroHost::audio() const {
    return linkPeer_&&linkPresentationRole_==2?linkPeer_->audio_:audio_;
}
const QImage& LibretroHost::frameForLinkRole(int role) const {
    return linkPeer_&&role==2?linkPeer_->frame_:frame_;
}
double LibretroHost::aspectRatioForLinkRole(int role) const {
    const auto* h=linkPeer_&&role==2?linkPeer_.get():this;
    return h->av_.geometry.aspect_ratio>0?h->av_.geometry.aspect_ratio:
        h->av_.geometry.base_height?double(h->av_.geometry.base_width)/h->av_.geometry.base_height:4.0/3.0;
}
bool LibretroHost::setLinkCableEnabled(bool enabled,QString& error) {
    error.clear();
    if(linkParent_) {error=QStringLiteral("A cable peer cannot own another console");return false;}
    if(enabled && adapter_ != ControllerAdapter::None) {
        error=Localization::text(QStringLiteral("Отключите мультиадаптер перед включением Link Cable."));return false;
    }
    if(linkRequested_==enabled && (!loaded_ || !enabled || linkPair_))return true;
    if(enabled && loaded_ && (!api_->system_hw || api_->system_hw()!=0x80 || hasCdHardware() ||
            (api_->features&&(api_->features()&2u)) ||
            (api_->paprium_active&&api_->paprium_active()))) {
        error=Localization::text(QStringLiteral("Link Cable доступен для картриджей Mega Drive без Sega CD и 32X."));return false;
    }
    if(!enabled) {
        stopLinkedPair();linkRequested_=false;buttons_.fill(0);
        if(loaded_) {applyControllerPorts(true);refreshRecordingIdentity();}
        return true;
    }
    linkRequested_=true;
    if(!loaded_)return true;
    if(hardReset(error))return true;
    stopLinkedPair();linkRequested_=false;
    if(loaded_) {applyControllerPorts(true);refreshRecordingIdentity();}
    return false;
}
void LibretroHost::stopLinkedPair() {
    if(api_&&api_->link_connect)api_->link_connect(nullptr,nullptr,nullptr);
    if(linkPeer_&&linkPeer_->api_->link_connect)linkPeer_->api_->link_connect(nullptr,nullptr,nullptr);
    linkPair_.reset();
    if(linkPeer_) {Activation scope(linkPeer_.get());linkPeer_.reset();}
}
bool LibretroHost::startLinkedPair(QString& error) {
    if(linkParent_ || !linkRequested_ || !loaded_)return true;
    if(linkPair_)return true;
    if(!api_->system_hw || api_->system_hw()!=0x80 || hasCdHardware() ||
       (api_->features&&(api_->features()&2u)) || (api_->paprium_active&&api_->paprium_active())) {
        error=Localization::text(QStringLiteral("Link Cable доступен для картриджей Mega Drive без Sega CD и 32X."));return false;
    }
    if(!api_->link_io||!api_->link_connect||!api_->link_deadline||!api_->link_latch||
       !api_->link_restore_latch||!api_->link_audio_size||!api_->link_audio_save||!api_->link_audio_load||!api_->link_audio_size()) {
        error=Localization::text(QStringLiteral("Для Link Cable требуется обновлённое ядро."));return false;
    }
    const QFileInfo core(library_.fileName());
    const QString peerPath=core.dir().filePath(core.completeBaseName()+QStringLiteral("_linkpeer.")+core.suffix());
    if(!QFileInfo::exists(peerPath)) {error=Localization::text(QStringLiteral("Не найден модуль второй консоли Link Cable: %1")).arg(peerPath);return false;}
    auto peer=std::make_unique<LibretroHost>(peerPath,dataPath_);
    peer->linkParent_=this;peer->moviePlayback_=true;
    peer->optionOverrides_=variables_;peer->variables_=variables_;
    peer->autoFixChecksum_=autoFixChecksum_;peer->smsFmEnabled_=smsFmEnabled_;
    peer->controllerConnected_={{true,false,false}};
    peer->controllerSixButton_[0]=controllerSixButton_[1];
    bool ok=false;
    {
        Activation scope(peer.get());
        if(peer->initialize(error)) {
            if(peer->exactCoreHash_!=exactCoreHash_)error=QStringLiteral("Link Cable core modules must be from the same build");
            else {
                peer->content_=content_;peer->coreContent_=coreContent_;
                peer->encodedGamePath_=encodedGamePath_;peer->gamePath_=gamePath_;
                peer->contentExtension_=contentExtension_;peer->contentName_=contentName_;
                peer->romHash_=romHash_;peer->gameKey_=gameKey_;peer->legacyGameKey_=legacyGameKey_;
                peer->contentInfo_=contentInfo_;
                peer->contentInfo_.data=peer->coreContent_.constData();
                peer->contentInfoActive_=true;
                retro_game_info game{peer->encodedGamePath_.constData(),peer->coreContent_.constData(),size_t(peer->coreContent_.size()),nullptr};
                peer->loaded_=peer->api_->load_game(&game);peer->contentInfoActive_=false;
                if(peer->loaded_) {
                    peer->api_->get_system_av_info(&peer->av_);
                    if(std::abs(peer->fps()-fps())>1e-9 || peer->api_->system_hw()!=0x80)
                        error=QStringLiteral("Link Cable console hardware/timing differs");
                    else {
                        peer->applyControllerPorts(true);
                        for(unsigned i=0;i<2;++i) {
                            const unsigned id=i?RETRO_MEMORY_RTC:RETRO_MEMORY_SAVE_RAM;
                            peer->saveMemorySizes_[i]=peer->api_->get_memory_size(id);
                            if(peer->saveMemorySizes_[i]&&peer->saveMemorySizes_[i]==saveMemorySizes_[i])
                                std::memcpy(peer->api_->get_memory_data(id),api_->get_memory_data(id),saveMemorySizes_[i]);
                        }
                        peer->movieCheats_=cheats();peer->movieCheatsActive_=true;peer->applyCheats();ok=true;
                    }
                } else error=Localization::text(QStringLiteral("Не удалось загрузить ROM во вторую консоль."));
            }
        }
    }
    if(!ok) {Activation scope(peer.get());peer.reset();return false;}
    linkPeer_=std::move(peer);linkPair_.reset(new LinkPair(this,linkPeer_.get()));
    if(!linkPair_->initialize()) {error=QStringLiteral("Cannot allocate Link Cable execution contexts");stopLinkedPair();return false;}
    setDebugMasks(debugVideoMask_,debugAudioMask_);setDebugBackdrop(debugBackdrop_);
    { Activation scope(linkPeer_.get());
      for (unsigned channel=0;channel<debugAudioGains_.size();++channel)
          linkPeer_->setDebugAudioGain(channel,debugAudioGains_[channel]); }
    if (recordingVideoEnabled_ && !setRecordingVideoEnabled(true)) {
        error=Localization::text(QStringLiteral("Не удалось подготовить исходные кадры для записи видео."));
        stopLinkedPair();return false;
    }
    linkPair_->connect();applyControllerPorts(true);refreshRecordingIdentity();return true;
}
void LibretroHost::runLinkedFrame(bool video,bool audio) {linkPair_->run(video,audio);}
void LibretroHost::resetLinkedPair() {
    // Both contexts are suspended outside retro_run at this public boundary.
    api_->link_connect(nullptr,nullptr,nullptr);linkPeer_->api_->link_connect(nullptr,nullptr,nullptr);
    clearCoreCheats();api_->reset();resetAchievementMemory();applyCheats();audio_.clear();
    {Activation scope(linkPeer_.get());linkPeer_->reset();}
    linkPair_->connect();
}
qsizetype LibretroHost::serializedSize() const {
    if(!loaded_)return 0;
    if(!linkPeer_)return qsizetype(api_->serialize_size());
    quint64 total=PairHeader;
    for(const auto* h:std::array<const LibretroHost*,2>{this,linkPeer_.get()}) {
        total+=h->api_->serialize_size()+h->api_->link_audio_size()+h->saveMemorySizes_[0]+h->saveMemorySizes_[1];
    }
    return total<=PairLimit?qsizetype(total):0;
}
QByteArray LibretroHost::captureLinkedState() const {
    const auto total=serializedSize();if(!total)return {};
    std::array<quint32,8> size{};
    size[0]=api_->serialize_size();size[1]=linkPeer_->api_->serialize_size();
    size[2]=api_->link_audio_size();size[3]=linkPeer_->api_->link_audio_size();
    size[4]=saveMemorySizes_[0];size[5]=saveMemorySizes_[1];size[6]=linkPeer_->saveMemorySizes_[0];size[7]=linkPeer_->saveMemorySizes_[1];
    QByteArray out("KRLINK01",8);out.reserve(total);
    for(auto n:size)append32(out,n);
    append32(out,api_->link_latch());append32(out,linkPeer_->api_->link_latch());
    out.resize(total);qsizetype off=PairHeader;
    for(unsigned i=0;i<8;++i) {
        const auto* h=(i==1||i==3||i>=6)?linkPeer_.get():this;Activation scope(const_cast<LibretroHost*>(h));
        if(i<2) {if(!h->api_->serialize(out.data()+off,size[i]))return {};}
        else if(i<4) {if(!h->api_->link_audio_save(out.data()+off,size[i]))return {};}
        else if(size[i]) {const auto* data=h->api_->get_memory_data(i%2?RETRO_MEMORY_RTC:RETRO_MEMORY_SAVE_RAM);if(!data)return {};std::memcpy(out.data()+off,data,size[i]);}
        off+=size[i];
    }
    return out;
}
bool LibretroHost::restoreLinkedState(const QByteArray& state,bool transactional) {
    if(!linkPeer_||state.size()!=serializedSize()||state.left(8)!="KRLINK01")return false;
    std::array<quint32,8> sizes{};std::array<qsizetype,8> offset{};quint64 total=PairHeader;
    for(unsigned i=0;i<8;++i) {
        const auto* h=(i==1||i==3||i>=6)?linkPeer_.get():this;
        sizes[i]=qFromBigEndian<quint32>(state.constData()+8+4*i);offset[i]=qsizetype(total);total+=sizes[i];
        const size_t expected=i<2?h->api_->serialize_size():i<4?h->api_->link_audio_size():h->saveMemorySizes_[i%2];
        if(sizes[i]!=expected||total>quint64(state.size()))return false;
    }
    const unsigned latch[2]{qFromBigEndian<quint32>(state.constData()+40),qFromBigEndian<quint32>(state.constData()+44)};
    if(total!=quint64(state.size()) || (latch[0]&~0x703u) || (latch[1]&~0x703u))return false;
    for(auto value:latch) {const unsigned level=value>>8;if(level!=0&&level!=4&&level!=6)return false;}
    const auto previous=transactional?captureLinkedState():QByteArray();
    if(transactional&&previous.isEmpty())return false;
    const auto apply=[&]()->bool {
    for(unsigned i=0;i<2;++i) {
        auto* h=i?linkPeer_.get():this;Activation scope(h);
        if(!h->api_->unserialize(state.constData()+offset[i],sizes[i]))return false;
        if(!h->api_->link_audio_load(state.constData()+offset[i+2],sizes[i+2]))return false;
        for(unsigned j=0;j<2;++j) {
            const unsigned field=4+i*2+j;
            if(sizes[field]) {void* data=h->api_->get_memory_data(j?RETRO_MEMORY_RTC:RETRO_MEMORY_SAVE_RAM);if(!data)return false;std::memcpy(data,state.constData()+offset[field],sizes[field]);}
        }
        h->applyControllerPorts();h->audio_.clear();
    }
    linkPair_->connect();api_->link_restore_latch(latch[0]);linkPeer_->api_->link_restore_latch(latch[1]);return true;
    };
    if(apply())return true;
    if(transactional&&!restoreLinkedState(previous,false))unload();
    return false;
}
