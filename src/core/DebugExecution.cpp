#include "LibretroHost.h"
#include "LibretroHostApi.h"
#include "i18n/Localization.h"
#include <libco.h>
#include <QRegularExpression>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <exception>
#include <limits>

namespace {
QString tr(const char* s) { return Localization::text(QString::fromUtf8(s)); }
bool number(QString text, quint64& value) {
    int base = 10;
    if (text.startsWith('$')) {text.remove(0,1);base=16;}
    else if (text.startsWith("0x",Qt::CaseInsensitive)) {text.remove(0,2);base=16;}
    bool ok=false;value=text.toULongLong(&ok,base);return ok;
}
// Conditions compile on editing, never in an instruction/memory callback.
struct Condition {
    enum Source { Always, Register, Address, Value, Size, Hit } source=Always;
    unsigned reg=0, compare=0;
    quint64 mask=~quint64(0), rhs=0;
    bool compile(const QString& text,const kr_debug_cpu& cpu) {
        if(text.trimmed().isEmpty())return true;
        static const QRegularExpression expression(
            R"(^\s*([A-Za-z][A-Za-z0-9']*)\s*(?:&\s*(\$[0-9a-fA-F]+|0[xX][0-9a-fA-F]+|[0-9]+))?\s*(==|!=|<=|>=|<|>)\s*(\$[0-9a-fA-F]+|0[xX][0-9a-fA-F]+|[0-9]+)\s*$)");
        const auto m=expression.match(text);
        if(!m.hasMatch())return false;
        const auto name=m.captured(1).toUpper();
        if(name=="ADDR")source=Address;
        else if(name=="VALUE")source=Value;
        else if(name=="SIZE")source=Size;
        else if(name=="HIT")source=Hit;
        else {
            source=Register;bool found=false;
            for(unsigned i=0;i<std::min(cpu.count,KR_DEBUG_MAX_REGS);++i)
                if(name==QString::fromLatin1(cpu.regs[i].name)) {reg=i;found=true;break;}
            if(!found)return false;
        }
        if(!m.captured(2).isEmpty()&&!number(m.captured(2),mask))return false;
        if(!number(m.captured(4),rhs))return false;
        compare=QStringList{"==","!=","<","<=",">",">="}.indexOf(m.captured(3));
        return true;
    }
    bool matches(const kr_debug_cpu& cpu,quint32 address,quint32 value,unsigned size,quint64 hits)const {
        if(source==Always)return true;
        quint64 lhs=source==Address?address:source==Value?value:source==Size?size:
                    source==Hit?hits:cpu.regs[reg].value;
        lhs&=mask;
        switch(compare) {case 0:return lhs==rhs;case 1:return lhs!=rhs;
        case 2:return lhs<rhs;case 3:return lhs<=rhs;case 4:return lhs>rhs;default:return lhs>=rhs;}
    }
};
}

struct LibretroHost::DebugExecution {
    LibretroHost& host;
    cothread_t control=nullptr, fiber=nullptr;
    static thread_local DebugExecution* entering;
    bool pending=false, running=false, stopped=false, suppress=false;
    bool follow=true, pauseNext=false, stepping=false, over=false, out=false;
    unsigned cpu=KR_DEBUG_M68K, stepCpu=KR_DEBUG_M68K, stopCpu=0, event=0, accessSize=0;
    quint32 address=0,value=0,navigate=0,target=0,startSp=0;
    unsigned steps=0;
    bool previousReturn=false;
    QVector<DebuggerBreakpoint> breaks;
    QVector<Condition> conditions;
    std::exception_ptr failure;
    explicit DebugExecution(LibretroHost& h):host(h){}
    ~DebugExecution(){if(fiber)co_delete(fiber);}
    static void entry(){entering->worker();}
    void worker() {
        for(;;) {
            try {host.api_->run();}catch(...){failure=std::current_exception();}
            pending=false;co_switch(control);
        }
    }
    void masks() {
        unsigned masks[2]{};
        if(pauseNext||stepping)masks[stepCpu-1]|=KR_DEBUG_EXEC;
        for(const auto& b:breaks)if(b.enabled)masks[b.cpu-1]|=b.events;
        host.api_->debug_connect(hook,this,masks[0],masks[1]);
    }
    void stop(unsigned which,unsigned kind,quint32 addr,quint32 val,unsigned bytes) {
        cpu=stopCpu=which;event=kind;address=addr;value=val;accessSize=bytes;stopped=true;
        pauseNext=stepping=over=out=previousReturn=false;
        co_switch(control);
        // Resume this exact invocation once. A self-branch hits again next time.
    }
    static void hook(void* opaque,unsigned which,unsigned kind,uint32_t addr,uint32_t val,unsigned bytes) {
        auto& d=*static_cast<DebugExecution*>(opaque);
        if(!d.running||d.suppress)return;
        kr_debug_cpu state{};
        bool haveState=false;
        const auto readState=[&] {
            if(!haveState)haveState=d.host.api_->debug_state(which,&state,sizeof(state));
            return haveState;
        };
        bool hit=false;
        for(int i=0;i<d.breaks.size();++i) {
            auto& b=d.breaks[i];
            if(!b.enabled||b.cpu!=which||!(b.events&kind))continue;
            const quint32 mask=which==KR_DEBUG_Z80?0xffffu:0xffffffu;
            const quint64 last=quint64(addr)+std::max(1u,bytes)-1;
            const bool overlaps=last<=mask ? !(addr>b.last||last<b.first)
                                          : (addr<=b.last || (last&mask)>=b.first);
            if(!overlaps)continue;
            if(b.hits!=std::numeric_limits<quint64>::max())++b.hits;
            if(b.hits<=b.ignoreHits)continue;
            const auto& c=d.conditions[i];
            if(c.source==Condition::Register&&!readState())continue;
            if(!c.matches(state,addr,val,bytes,b.hits))continue;
            if(b.oneShot)b.enabled=false;
            hit=true;
        }
        if(kind==KR_DEBUG_EXEC&&which==d.stepCpu) {
            if(d.pauseNext)hit=true;
            else if(d.stepping) {
                if(d.over) {if(addr==d.target&&readState()&&state.sp>=d.startSp)hit=true;}
                else if(d.out) {
                    if(d.previousReturn&&readState()&&state.sp>d.startSp)hit=true;
                    kr_debug_instruction instruction{};
                    d.previousReturn=d.host.api_->debug_disassemble(which,addr,&instruction,sizeof(instruction))&&
                        (instruction.flags&KR_DEBUG_RETURN);
                } else if(d.steps==0)hit=true;
                else --d.steps;
            }
        }
        if(hit)d.stop(which,kind,addr,val,bytes);
    }
};
thread_local LibretroHost::DebugExecution* LibretroHost::DebugExecution::entering=nullptr;
void LibretroHost::DebugExecutionDeleter::operator()(DebugExecution* p)const {delete p;}

bool LibretroHost::debuggerAttach(QString& error) {
    if(debugger_) {
        kr_debug_cpu state{};
        if(api_->debug_state(KR_DEBUG_M68K,&state,sizeof(state)) ||
           api_->debug_state(KR_DEBUG_Z80,&state,sizeof(state))) {
            debugger_->masks();return true;
        }
        debuggerDetach();
    }
    error.clear();
    if(!loaded_) {error=tr("Игра не загружена.");return false;}
    if(linkRequested_) {error=tr("Отладка процессоров Link Cable пока недоступна.");return false;}
    if(!api_->debug_connect||!api_->debug_state||!api_->debug_peek||!api_->debug_disassemble) {
        error=tr("Для отладчика требуется обновлённое ядро.");return false;
    }
    auto candidate=std::unique_ptr<DebugExecution,DebugExecutionDeleter>(new DebugExecution(*this));
    if(!api_->debug_connect(DebugExecution::hook,candidate.get(),0,0)) {
        error=tr("Отладка процессоров этой системы пока недоступна.");return false;
    }
    kr_debug_cpu state{};
    if(!api_->debug_state(KR_DEBUG_M68K,&state,sizeof(state)))candidate->cpu=KR_DEBUG_Z80;
    debugger_=std::move(candidate);return true;
}
void LibretroHost::debuggerDetach() {
    if(!debugger_)return;
    finishDebugFrame();
    api_->debug_connect(nullptr,nullptr,0,0);
    debugger_.reset();
}
bool LibretroHost::debuggerAttached()const{return bool(debugger_);}
bool LibretroHost::debuggerStopped()const{return debugger_&&debugger_->stopped;}
bool LibretroHost::debuggerRunRequested()const{return debugger_&&(debugger_->stepping||debugger_->pauseNext);}
bool LibretroHost::debuggerFramePending()const{return debugger_&&debugger_->pending;}
bool LibretroHost::runDebugFrame(bool video,bool audio) {
    if(!debugger_) {runFrame(video,audio);return true;}
    auto& d=*debugger_;
    if(!d.pending) {
        if(!loaded_)return true;
        if(runRate_!=fps()) {runRate_=fps();ticksPerFrame_=quint64(std::llround(4294967296.0/runRate_));}
        if(!video)spriteOwnership_={};
        videoEnabled_=video;audioEnabled_=audio;audio_.clear();
        d.event=0;d.stopCpu=0;d.pending=true;
    }
    d.control=co_active();
    if(!d.fiber)d.fiber=co_create(512*1024,DebugExecution::entry);
    if(!d.fiber)throw std::bad_alloc();
    auto* previous=DebugExecution::entering;
    DebugExecution::entering=&d;
    d.running=true;d.failure=nullptr;
    co_switch(d.fiber);
    d.running=false;DebugExecution::entering=previous;
    if(d.failure)std::rethrow_exception(d.failure);
    if(d.pending)return false;
    videoEnabled_=audioEnabled_=true;
    ++runClock_.frames;runClock_.ticks+=ticksPerFrame_;
    return true;
}
void LibretroHost::finishDebugFrame() {
    if(!debugger_)return;
    if(debugger_->pending) {
        debugger_->suppress=true;
        runDebugFrame();
        debugger_->suppress=false;
    }
    debugger_->pauseNext=debugger_->stepping=debugger_->over=debugger_->out=false;
    debugger_->previousReturn=debugger_->stopped=false;
    debugger_->masks();
}
bool LibretroHost::debuggerCommand(const DebuggerCommand& command,QString& error) {
    error.clear();
    if(command.kind==DebuggerCommand::Detach) {debuggerDetach();return true;}
    if(!debuggerAttach(error))return false;
    auto& d=*debugger_;
    kr_debug_cpu state{};
    unsigned selected=command.cpu;
    if(!api_->debug_state(selected,&state,sizeof(state))) {
        selected=api_->debug_state(KR_DEBUG_M68K,&state,sizeof(state))?KR_DEBUG_M68K:KR_DEBUG_Z80;
        if(!api_->debug_state(selected,&state,sizeof(state)))return false;
    }
    d.cpu=selected;d.navigate=command.address;d.follow=command.followPc;
    if(command.kind==DebuggerCommand::Configure) {
        if(command.breakpoints.size()>128) {error=tr("Не более 128 точек остановки.");return false;}
        QVector<Condition> compiled;compiled.reserve(command.breakpoints.size());
        for(const auto& b:command.breakpoints) {
            kr_debug_cpu registers{};Condition c;
            const quint32 limit=b.cpu==KR_DEBUG_Z80?0xffffu:0xffffffu;
            if(b.first>b.last||b.last>limit||!b.events||(b.events&~7u)||
               !api_->debug_state(b.cpu,&registers,sizeof(registers))||!c.compile(b.condition,registers)) {
                error=tr("Неверная точка остановки или условие.");return false;
            }
            compiled.push_back(c);
        }
        auto next=command.breakpoints;
        for(auto& b:next) {
            const auto old=std::find_if(d.breaks.cbegin(),d.breaks.cend(),[&](const auto& p){return p.id==b.id;});
            if(old!=d.breaks.cend()&&old->cpu==b.cpu&&old->events==b.events&&
               old->first==b.first&&old->last==b.last&&old->condition==b.condition&&
               old->ignoreHits==b.ignoreHits&&old->oneShot==b.oneShot) {
                b.hits=old->hits;
                if(b.id!=command.editedBreakpointId)b.enabled=old->enabled;
            } else b.hits=0;
        }
        d.breaks=std::move(next);d.conditions=std::move(compiled);
    }
    if(command.kind==DebuggerCommand::Pause) {
        if(!d.pending&&(d.stepping||d.pauseNext)) {
            d.stepping=d.pauseNext=d.over=d.out=false;d.stopped=true;
        } else {d.stepCpu=selected;d.pauseNext=true;d.stopped=false;}
    }
    if(command.kind==DebuggerCommand::Continue) {
        d.pauseNext=d.stepping=d.over=d.out=d.previousReturn=d.stopped=false;
    }
    if(command.kind==DebuggerCommand::StepInto||command.kind==DebuggerCommand::StepOver||
       command.kind==DebuggerCommand::StepOut) {
        if(!d.stopped && d.pending) {error=tr("Сначала остановите выполнение.");return false;}
        // An EXEC stop is already inside its callback: resume executes that
        // instruction before another hook. At other boundaries count one hook.
        const bool atExecute=d.pending&&d.stopCpu==selected&&d.event==KR_DEBUG_EXEC&&d.address==state.pc;
        d.stepCpu=selected;d.steps=(d.pending&&d.stopCpu==selected)?0:1;d.stepping=true;d.pauseNext=d.stopped=false;
        d.over=d.out=d.previousReturn=false;d.startSp=state.sp;
        kr_debug_instruction instruction{};
        if(command.kind==DebuggerCommand::StepOver&&
           api_->debug_disassemble(selected,state.pc,&instruction,sizeof(instruction))&&
           (instruction.flags&KR_DEBUG_CALL)) {
            d.over=true;d.target=(state.pc+instruction.length)&(selected==KR_DEBUG_Z80?0xffff:0xffffff);
        } else if(command.kind==DebuggerCommand::StepOut) {
            d.out=true;
            if(atExecute&&api_->debug_disassemble(selected,state.pc,&instruction,sizeof(instruction)))
                d.previousReturn=instruction.flags&KR_DEBUG_RETURN;
        }
    }
    d.masks();return true;
}
DebuggerSnapshot LibretroHost::debuggerSnapshot()const {
    DebuggerSnapshot out;out.generation=inspectionGeneration_;out.frame=runClock_.frames;
    if(!debugger_)return out;
    const auto& d=*debugger_;
    out.gameKey=gamePath_+QLatin1Char('\n')+contentName_;
    out.attached=true;out.stopped=d.stopped;out.partialFrame=d.pending;
    out.selectedCpu=d.cpu;out.eventCpu=d.stopCpu;out.event=d.pending?d.event:0;out.address=d.address;out.value=d.value;out.accessSize=d.accessSize;
    out.breakpoints=d.breaks;
    for(unsigned cpu: {KR_DEBUG_M68K,KR_DEBUG_Z80}) {
        kr_debug_cpu state{};
        if(api_->debug_state(cpu,&state,sizeof(state))) {
            state.count=std::min(state.count,KR_DEBUG_MAX_REGS);
            for(auto& r:state.regs)r.name[sizeof(r.name)-1]=0;
            out.cpus.push_back(state);
        }
    }
    const auto state=std::find_if(out.cpus.begin(),out.cpus.end(),[&](const auto& s){return s.cpu==d.cpu;});
    if(state==out.cpus.end())return out;
    quint32 address=d.follow?state->pc:d.navigate;
    const quint32 mask=d.cpu==KR_DEBUG_Z80?0xffff:0xffffff;
    for(unsigned i=0;i<32;++i) {
        kr_debug_instruction instruction{};
        if(!api_->debug_disassemble(d.cpu,address,&instruction,sizeof(instruction))) {
            instruction.address=address;instruction.length=d.cpu==KR_DEBUG_Z80?1:2;
        }
        instruction.text[sizeof(instruction.text)-1]=0;
        if(!instruction.length)instruction.length=d.cpu==KR_DEBUG_Z80?1:2;
        if(instruction.length>16)break;
        out.instructions.push_back(instruction);
        address=(address+instruction.length)&mask;
    }
    out.stackAddress=state->sp;
    out.stack.resize(64);out.stackValid.resize(64);
    const auto n=std::min<size_t>(64,api_->debug_peek(d.cpu,state->sp,
        reinterpret_cast<uint8_t*>(out.stack.data()),reinterpret_cast<uint8_t*>(out.stackValid.data()),64));
    out.stack.resize(n);out.stackValid.resize(n);
    return out;
}
