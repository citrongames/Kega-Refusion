#include "LibretroHost.h"
#include "LibretroHostApi.h"
#include "DebugCoverageMap.h"
#include "i18n/Localization.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace {
QString tr(const char* s) {return Localization::text(QString::fromUtf8(s));}
}

struct LibretroHost::CoverageExecution {
    LibretroHost& host;
    DebugCoverageMap map;
    DebugCoverageConfig config;
    quint32 execMask=0,readMask=0;
    quint64 captureId=0,boundaries=0;
    bool bound=false,needsBind=false,hasCapture=false;
    QString path,game,failure;
    QByteArray hash;
    explicit CoverageExecution(LibretroHost& h):host(h) {}
    LibretroHost* console() const {return config.instance?host.linkPeer_.get():&host;}
    static void capture(void* opaque,const kr_cov_event* event) {
        auto& c=*static_cast<CoverageExecution*>(opaque);
        if(event && !c.host.rewindPreview_)c.map.mark(*event);
    }
    void detach() {
        auto* core=console();
        if(bound && core && core->api_->coverage_bind) {
            Activation active(core);
            core->api_->coverage_bind(KR_COV_VERSION,0,0,nullptr,nullptr);
        }
        bound=false;
    }
    bool bind() {
        detach();needsBind=false;
        auto* core=console();
        if(!core || !core->loaded_ || !core->api_->coverage_bind || !core->api_->coverage_sources)return false;
        Activation active(core);
        std::array<kr_cov_source,KR_COV_MAX_SOURCES> current{};
        const auto count=std::min(current.size(),core->api_->coverage_sources(KR_COV_VERSION,current.data(),current.size()));
        quint32 availableExec=0,availableRead=0;
        for(size_t i=0;i<count;++i) {
            availableExec|=current[i].exec_cpu_mask;availableRead|=current[i].read_cpu_mask;
        }
        if((execMask&~availableExec) || (readMask&~availableRead))return false;
        const auto storedSources=map.sources();
        size_t eligible=0;
        for(size_t i=0;i<count;++i) {
            const auto& source=current[i];
            if(!(source.exec_cpu_mask&execMask) && !(source.read_cpu_mask&readMask))continue;
            ++eligible;
            const auto match=std::find_if(storedSources.cbegin(),storedSources.cend(),[&](const auto& stored) {
                return source.id==stored.native.id && source.size==stored.native.size &&
                    source.flags==stored.native.flags &&
                    (source.exec_cpu_mask&execMask)==(stored.native.exec_cpu_mask&execMask) &&
                    (source.read_cpu_mask&readMask)==(stored.native.read_cpu_mask&readMask);
            });
            if(match==storedSources.cend())return false;
        }
        if(eligible!=size_t(storedSources.size()))return false;
        bound=core->api_->coverage_bind(KR_COV_VERSION,execMask,readMask,capture,this);
        return bound;
    }
};

void LibretroHost::CoverageExecutionDeleter::operator()(CoverageExecution* p) const {delete p;}
bool LibretroHost::coverageActive() const {return coverage_ && coverage_->map.active();}
void LibretroHost::coverageStop() {
    if(!coverage_)return;
    coverage_->detach();coverage_->needsBind=false;coverage_->map.stop();
}
void LibretroHost::coverageDisconnectPeer() {
    if(coverage_ && coverage_->config.instance==1) {
        coverage_->detach();coverage_->needsBind=true;
    }
}
void LibretroHost::coverageBeforeFrame() {
    if(!coverageActive())return;
    auto& c=*coverage_;
    if(c.needsBind && !c.bind()) {
        c.map.stop();c.failure=tr("Учёт остановлен: изменилось доступное оборудование.");
    }
}
void LibretroHost::debugToolsBoundary(unsigned reason) {
    traceBoundary(reason);
    if(coverageActive() && !rewindPreview_) {
        ++coverage_->boundaries;coverage_->needsBind=true;
    }
}
bool LibretroHost::coverageCommand(const DebugCoverageCommand& command,QString& error) {
    error.clear();
    if(command.kind==DebugCoverageCommand::Refresh) {
        coverageViewInstance_=command.instance;
        coverageViewSource_=command.source;coverageViewOffset_=command.offset;
        return true;
    }
    if(command.kind==DebugCoverageCommand::Stop) {coverageStop();return true;}
    if(command.kind==DebugCoverageCommand::Clear || command.kind==DebugCoverageCommand::Export) {
        if(!coverage_ || !coverage_->hasCapture || command.captureId!=coverage_->captureId) {
            error=tr("Набор данных изменился. Обновите окно.");return false;
        }
        if(command.kind==DebugCoverageCommand::Clear) {
            coverage_->map.clear();coverage_->boundaries=0;return true;
        }
        return coverage_->map.exportFile(command.path,error);
    }
    if(command.kind!=DebugCoverageCommand::Start)return false;
    if(coverage_ && coverage_->map.writing()) {
        error=tr("Дождитесь окончания экспорта учёта кода и данных.");return false;
    }
    auto* core=command.config.instance==0?this:command.config.instance==1?linkPeer_.get():nullptr;
    if(!core || !core->loaded_ || !core->api_->coverage_sources || !core->api_->coverage_bind) {
        error=tr("Учёт кода и данных недоступен для выбранной консоли.");return false;
    }
    std::array<kr_cov_source,KR_COV_MAX_SOURCES> all{};
    size_t count;
    {
        Activation active(core);
        count=core->api_->coverage_sources(KR_COV_VERSION,all.data(),all.size());
    }
    if(!count || count>all.size()) {
        error=tr("Ядро не предоставило физические области памяти.");return false;
    }
    quint32 availableExec=0,availableRead=0;
    for(size_t i=0;i<count;++i) {
        availableExec|=all[i].exec_cpu_mask;availableRead|=all[i].read_cpu_mask;
    }
    const auto cpu=command.config.cpu;
    if(cpu && ((cpu&(cpu-1)) || !(cpu&(availableExec|availableRead)))) {
        error=tr("Выбранный процессор недоступен.");return false;
    }
    const auto exec=command.config.execution?(cpu?cpu&availableExec:availableExec):0u;
    const auto reads=command.config.reads?(cpu?cpu&availableRead:availableRead):0u;
    if(!exec && !reads) {error=tr("Выберите поддерживаемый вид обращений.");return false;}
    QVector<kr_cov_source> sources;
    for(size_t i=0;i<count;++i)
        if((all[i].exec_cpu_mask&exec) || (all[i].read_cpu_mask&reads)) {
            all[i].name[sizeof(all[i].name)-1]=0;
            all[i].exec_cpu_mask&=exec;all[i].read_cpu_mask&=reads;
            sources.push_back(all[i]);
        }
    if(!coverage_)coverage_.reset(new CoverageExecution(*this));
    auto& c=*coverage_;
    if(!c.map.start(sources,gameKey_,romHash_,error,command.config.instance)) {
        if(!c.map.active())coverageStop();
        return false;
    }
    // No guest executes while this owner-thread command replaces the map.
    // Detach the old console before changing its selected instance.
    c.detach();
    c.config=command.config;c.execMask=exec;c.readMask=reads;
    c.path=gamePath_;c.hash=romHash_;c.game=gameKey_;c.failure.clear();
    c.hasCapture=true;++c.captureId;c.boundaries=0;
    coverageViewInstance_=c.config.instance;coverageViewSource_=0;coverageViewOffset_=0;
    if(!c.bind()) {
        c.map.stop();error=tr("Ядро не смогло включить учёт кода и данных.");return false;
    }
    return true;
}
DebugCoverageSnapshot LibretroHost::coverageSnapshot() const {
    DebugCoverageSnapshot out;
    if(loaded_)out.instances.push_back(0);
    if(linkPeer_ && linkPeer_->loaded_)out.instances.push_back(1);
    const auto view=out.instances.contains(coverageViewInstance_)?coverageViewInstance_:0u;
    out.instance=view;
    const auto* core=view?linkPeer_.get():this;
    QVector<kr_cov_source> available;
    if(core && core->loaded_ && core->api_->coverage_sources && core->api_->coverage_bind) {
        Activation active(const_cast<LibretroHost*>(core));
        std::array<kr_cov_source,KR_COV_MAX_SOURCES> sources{};
        const auto count=std::min(sources.size(),core->api_->coverage_sources(KR_COV_VERSION,sources.data(),sources.size()));
        for(size_t i=0;i<count;++i) {
            auto source=sources[i];source.name[sizeof(source.name)-1]=0;
            if(!source.id || source.id>=KR_COV_MAX_SOURCES || !source.size)continue;
            available.push_back(source);
            out.execCpus|=source.exec_cpu_mask;out.readCpus|=source.read_cpu_mask;
        }
        if(core->api_->debug_targets) {
            std::array<kr_debug_target,KR_DEBUG_MAX_CPUS> targets{};
            const auto targetCount=std::min(targets.size(),
                core->api_->debug_targets(KR_DEBUG_VERSION,targets.data(),targets.size()));
            for(size_t i=0;i<targetCount;++i) {
                auto target=targets[i];target.name[sizeof(target.name)-1]=0;
                if(target.cpu&(out.execCpus|out.readCpus))out.targets.push_back(target);
            }
        }
        out.available=!available.isEmpty() && !out.targets.isEmpty();
    }
    if(!coverage_ || !coverage_->hasCapture) {
        for(const auto& source:available)out.sources.push_back({source,0,0,0});
        return out;
    }
    const auto& c=*coverage_;
    out.hasCapture=true;out.capturedInstance=c.config.instance;
    out.captureId=c.captureId;out.revision=c.map.revision();out.boundaries=c.boundaries;
    out.active=c.map.active();out.writing=c.map.writing();out.game=c.game;
    out.allocatedBytes=c.map.allocatedBytes();
    out.unknownCode=c.map.unknownCode();out.unknownData=c.map.unknownData();
    out.sources=c.map.sources();out.status=c.failure.isEmpty()?c.map.status():c.failure;
    if(out.sources.isEmpty())return out;
    auto source=std::find_if(out.sources.cbegin(),out.sources.cend(),
        [&](const auto& entry){return entry.native.id==coverageViewSource_;});
    if(source==out.sources.cend())source=out.sources.cbegin();
    out.source=source->native.id;
    out.offset=std::min(coverageViewOffset_,source->native.size-1)&~255u;
    out.page=c.map.page(out.source,out.offset);
    auto* captured=c.console();
    if(captured && captured->loaded_ && c.path==gamePath_ && c.hash==romHash_ &&
        captured->api_->coverage_sources) {
        // Link offsets come from today's physical view, not the captured
        // CD Word RAM layout or a prior bank mapping.
        Activation active(captured);
        std::array<kr_cov_source,KR_COV_MAX_SOURCES> sources{};
        const auto count=std::min(sources.size(),captured->api_->coverage_sources(KR_COV_VERSION,sources.data(),sources.size()));
        for(size_t i=0;i<count;++i)if(sources[i].id==out.source && sources[i].size==source->native.size) {
            const quint64 offset=quint64(sources[i].inspect_offset)+out.offset;
            if(sources[i].inspect_domain && offset<=std::numeric_limits<quint32>::max()) {
                out.canNavigate=true;out.inspectDomain=sources[i].inspect_domain;out.inspectOffset=quint32(offset);
            }
            break;
        }
    }
    return out;
}
