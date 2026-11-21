#include "LibretroHost.h"
#include "LibretroHostApi.h"
#include "DebugTraceBuffer.h"
#include "i18n/Localization.h"
#include <algorithm>
#include <array>

namespace {
QString tr(const char* text) { return Localization::text(QString::fromUtf8(text)); }
}

struct LibretroHost::TraceExecution {
    struct Endpoint { TraceExecution* trace; unsigned instance; };
    LibretroHost& host;
    DebugTraceBuffer buffer;
    DebugTraceConfig config;
    std::array<Endpoint,2> endpoints{{{this,0},{this,1}}};
    quint64 sequence=0, frame=0;
    quint32 epoch=0;
    bool bound=false, needsBind=false;
    QString game, capturedPath, failure;
    QByteArray capturedHash;
    bool capturedLink=false;
    quint64 captureId=0;
    QVector<kr_debug_target> capturedTargets;
    mutable QVector<DebugTraceRow> decodedRows;
    mutable quint64 decodedSequence=~quint64(0), decodedTotal=~quint64(0);
    explicit TraceExecution(LibretroHost& owner):host(owner) {}
    LibretroHost* console(unsigned side) const { return side?host.linkPeer_.get():&host; }
    static void capture(void* opaque,const kr_trace_record* native) {
        auto& endpoint=*static_cast<Endpoint*>(opaque);
        auto& t=*endpoint.trace;
        // Reconstructed rewind pictures are not executed game history.
        if (!native || t.host.rewindPreview_ || !t.buffer.active()) return;
        DebugTraceRecord record{};
        record.sequence=++t.sequence;
        record.frame=t.frame;
        record.instance=endpoint.instance;
        record.epoch=t.epoch;
        record.native=*native;
        t.buffer.append(record);
    }
    void detach() {
        for(unsigned side=0;side<endpoints.size();++side) {
            auto* core=console(side);
            if(core && core->api_->trace_bind) {
                Activation active(core);
                core->api_->trace_bind(KR_TRACE_VERSION,0,0,nullptr,nullptr);
            }
        }
        bound=false;
    }
    bool bind() {
        detach();
        if (!buffer.active()) {needsBind=false;return true;}
        bool any=false, selected=!config.cpu;
        for(unsigned side=0;side<endpoints.size();++side) {
            auto* core=console(side);
            if(!core || !core->loaded_ || !core->api_->trace_bind || !core->api_->trace_capabilities) continue;
            Activation active(core);
            std::array<kr_debug_target,KR_DEBUG_MAX_CPUS> targets{};
            unsigned available=0;
            if(core->api_->debug_targets) {
                const auto count=std::min(targets.size(),
                    core->api_->debug_targets(KR_DEBUG_VERSION,targets.data(),targets.size()));
                for(size_t i=0;i<count;++i)available|=targets[i].cpu;
            }
            const unsigned cpuMask=config.cpu ? (config.cpu>>8==side?config.cpu&available:0u) : available;
            if(cpuMask)selected=true;
            unsigned filters=config.filters&core->api_->trace_capabilities();
            if(!cpuMask)filters&=~KR_TRACE_INSTRUCTION;
            if(!filters)continue;
            if(!core->api_->trace_bind(KR_TRACE_VERSION,cpuMask,filters,capture,&endpoints[side])) {
                detach();buffer.stop();needsBind=false;return false;
            }
            any=true;
        }
        bound=any;needsBind=false;
        if(!any || !selected) {detach();buffer.stop();return false;}
        return true;
    }
};

void LibretroHost::TraceExecutionDeleter::operator()(TraceExecution* p) const { delete p; }

bool LibretroHost::traceActive() const { return tracer_ && tracer_->buffer.active(); }
void LibretroHost::traceStop() {
    if(!tracer_)return;
    tracer_->detach();
    tracer_->needsBind=false;
    tracer_->buffer.stop();
}
void LibretroHost::traceDisconnectPeer() {
    if(!tracer_ || !linkPeer_ || !linkPeer_->api_->trace_bind)return;
    Activation active(linkPeer_.get());
    linkPeer_->api_->trace_bind(KR_TRACE_VERSION,0,0,nullptr,nullptr);
    tracer_->needsBind=true;
}
void LibretroHost::traceBeforeFrame() {
    auto& t=*tracer_;
    if(!t.buffer.active()) {
        if(t.bound)t.detach();
        return;
    }
    if(t.needsBind && !t.bind()) {
        t.failure=tr("Трассировка остановлена: выбранное устройство больше недоступно.");
        return;
    }
    t.frame=runClock_.frames+1;
}
void LibretroHost::traceBoundary(unsigned reason) {
    if(!traceActive() || rewindPreview_)return;
    auto& t=*tracer_;
    DebugTraceRecord record{};
    record.sequence=++t.sequence;
    record.frame=runClock_.frames+(debuggerFramePending()?1:0);
    record.epoch=++t.epoch;
    record.native.kind=KR_TRACE_DISCONTINUITY;
    record.native.value=reason;
    t.buffer.append(record);
    t.needsBind=true;
}
bool LibretroHost::traceCommand(const DebugTraceCommand& command,QString& error) {
    error.clear();
    if(command.kind==DebugTraceCommand::Refresh)return true;
    if(command.kind==DebugTraceCommand::Stop) {traceStop();return true;}
    if(!tracer_)tracer_.reset(new TraceExecution(*this));
    auto& t=*tracer_;
    if(command.kind==DebugTraceCommand::Clear) {
        if(!t.buffer.clear(error))return false;
        return true;
    }
    if(command.kind==DebugTraceCommand::Export)return t.buffer.exportRecent(command.path,error);
    if(command.kind!=DebugTraceCommand::Start)return false;
    const auto snapshot=traceSnapshot();
    if(!snapshot.available) {
        error=tr("Трассировка недоступна для текущего ядра или игры.");return false;
    }
    if(command.config.cpu && std::none_of(snapshot.targets.cbegin(),snapshot.targets.cend(),
        [&](const auto& target){return target.cpu==command.config.cpu;})) {
        error=tr("Выбранный процессор недоступен.");return false;
    }
    auto config=command.config;
    config.filters&=snapshot.filters;
    if(!config.filters) {error=tr("Выберите события для трассировки.");return false;}
    if(t.buffer.writing()) {
        error=tr("Дождитесь окончания записи файла трассы.");return false;
    }
    if(config.memoryMiB<1 || config.memoryMiB>64) {
        error=tr("Выберите размер истории от 1 до 64 МиБ.");return false;
    }
    traceStop();
    if(!t.buffer.start(config,gameKey_,error))return false;
    t.game=gameKey_;t.sequence=0;t.epoch=0;t.failure.clear();
    t.capturedTargets=snapshot.targets;
    t.capturedPath=gamePath_;t.capturedHash=romHash_;t.capturedLink=bool(linkPeer_);
    ++t.captureId;t.decodedRows.clear();
    t.decodedSequence=t.decodedTotal=~quint64(0);
    t.frame=runClock_.frames+(debuggerFramePending()?1:0);
    t.config=config;
    if(!t.bind()) {t.buffer.stop();error=tr("Ядро не смогло включить трассировку.");return false;}
    return true;
}
DebugTraceSnapshot LibretroHost::traceSnapshot() const {
    DebugTraceSnapshot out;
    for(unsigned side=0;side<2;++side) {
        auto* core=side?linkPeer_.get():this;
        if(!core || !core->loaded_ || !core->api_->trace_bind ||
           !core->api_->trace_capabilities || !core->api_->debug_targets)continue;
        Activation active(const_cast<LibretroHost*>(core));
        out.filters|=core->api_->trace_capabilities();
        std::array<kr_debug_target,KR_DEBUG_MAX_CPUS> targets{};
        const auto count=std::min(targets.size(),
            core->api_->debug_targets(KR_DEBUG_VERSION,targets.data(),targets.size()));
        for(size_t i=0;i<count;++i) {
            auto target=targets[i];
            if(!target.cpu || target.cpu>=64 || (target.cpu&(target.cpu-1)))continue;
            target.cpu|=side<<8;target.name[sizeof(target.name)-1]=0;
            out.targets.push_back(target);
        }
    }
    out.available=!out.targets.isEmpty() && out.filters;
    if(!tracer_)return out;
    const auto& t=*tracer_;
    out.active=t.buffer.active();out.writing=t.buffer.writing();
    out.captureId=t.captureId;out.recordTargets=t.capturedTargets;
    out.canNavigate=loaded_ && t.capturedPath==gamePath_ && t.capturedHash==romHash_ &&
                    t.capturedLink==bool(linkPeer_);
    out.total=t.buffer.total();out.overwritten=t.buffer.overwritten();
    out.capacity=t.buffer.capacity();out.epoch=t.epoch;
    out.status=t.buffer.status();
    if(out.status.isEmpty())out.status=t.failure;
    out.filePath=t.buffer.filePath();out.game=t.game;
    if(t.decodedSequence==t.sequence && t.decodedTotal==t.buffer.total()) {
        out.rows=t.decodedRows;
        return out;
    }
    const auto records=t.buffer.latest(256);
    out.rows.reserve(records.size());
    // The decoder consumes the captured bytes, never the current mutable ROM
    // bank. It shares native disassembler scratch state with the debugger, so
    // run here on the same worker, not in the asynchronous file writer.
    for(const auto& record:records) {
        DebugTraceRow row;row.record=record;
        if(record.native.kind==KR_TRACE_EXEC && api_->trace_decode) {
            kr_debug_instruction instruction{};
            Activation active(const_cast<LibretroHost*>(this));
            if(api_->trace_decode(record.native.cpu,record.native.address,record.native.bytes,
                std::min<unsigned>(record.native.valid_bytes,16),&instruction,sizeof(instruction))) {
                instruction.text[sizeof(instruction.text)-1]=0;
                row.instruction=QString::fromLatin1(instruction.text);
                row.instructionBytes=instruction.length;
            }
        }
        out.rows.push_back(std::move(row));
    }
    t.decodedRows=out.rows;
    t.decodedSequence=t.sequence;t.decodedTotal=t.buffer.total();
    return out;
}
