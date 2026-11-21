#include "LibretroHost.h"
#include <QSet>
#include <QScopeGuard>
#include "LibretroHostApi.h"
#include "i18n/Localization.h"
#include <libco.h>
#include <QRegularExpression>
#include <algorithm>
#include <array>
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

namespace {
constexpr unsigned instanceShift = 8;
constexpr unsigned nativeCpuMask = (1u << instanceShift) - 1;
constexpr unsigned debugEvents = KR_DEBUG_EXEC | KR_DEBUG_READ | KR_DEBUG_WRITE;
constexpr unsigned exceptionReturn = 8; // Internal flow state, never an ABI flag.

unsigned nativeCpu(unsigned id) { return id & nativeCpuMask; }
unsigned instance(unsigned id) { return id >> instanceShift; }
quint32 addressMask(const kr_debug_target& target) {
    return target.address_bits == 32 ? ~quint32(0) : (quint32(1) << target.address_bits) - 1;
}
unsigned cpuIndex(unsigned id) {
    unsigned index = 0;
    while (id > 1) { id >>= 1; ++index; }
    return index;
}
bool ascendingStack(const kr_debug_target& target) {
    return target.capabilities & KR_DEBUG_CAP_STACK_ASCENDING;
}
}

struct LibretroHost::DebugExecution {
    struct Endpoint { DebugExecution* debugger; unsigned instance; };
    using Masks = std::array<unsigned, KR_DEBUG_MAX_CPUS>;

    LibretroHost& host;
    std::array<Endpoint, 2> endpoints{{{this, 0}, {this, 1}}};
    cothread_t control = nullptr, fiber = nullptr, resumeFiber = nullptr;
    LibretroHost* resumeOwner = nullptr;
    std::array<QImage, 2> previousPictures;
    static thread_local DebugExecution* entering;
    bool pending = false, running = false, stopped = false, suppress = false;
    bool activeHooks = false, renderVideo = true, captureAudio = true;
    bool follow = true, pauseNext = false, stepping = false, over = false, out = false;
    unsigned cpu = KR_DEBUG_M68K, stepCpu = KR_DEBUG_M68K, stopCpu = 0, event = 0, accessSize = 0;
    quint32 address = 0, value = 0, navigate = 0, stepTarget = 0, startSp = 0;
    unsigned steps = 0, depth = 0, pendingFlow = 0;
    bool flowHasDelaySlot = false;
    quint32 flowSlot = 0;
    QVector<kr_debug_target> targets;
    QVector<DebuggerBreakpoint> breaks;
    QVector<Condition> conditions;
    std::exception_ptr failure;

    explicit DebugExecution(LibretroHost& owner) : host(owner) {}
    ~DebugExecution() { if (fiber) co_delete(fiber); }

    LibretroHost* console(unsigned side) const { return side ? host.linkPeer_.get() : &host; }

    const kr_debug_target* target(unsigned id) const {
        const auto found = std::find_if(targets.cbegin(), targets.cend(),
            [id](const auto& item) { return item.cpu == id; });
        return found == targets.cend() ? nullptr : &*found;
    }

    void discover() {
        targets.clear();
        for (unsigned side = 0; side < endpoints.size(); ++side) {
            auto* core = console(side);
            if (!core || !core->loaded_) continue;
            const auto& api = *core->api_;
            if (!api.debug_state || !api.debug_peek || !api.debug_disassemble) continue;
            Activation active(core);
            if (api.debug_targets && api.debug_connect_v2) {
                std::array<kr_debug_target, KR_DEBUG_MAX_CPUS> available{};
                const auto count = std::min(available.size(),
                    api.debug_targets(KR_DEBUG_VERSION, available.data(), available.size()));
                for (size_t i = 0; i < count; ++i) {
                    auto item = available[i];
                    if (!item.cpu || (item.cpu & (item.cpu - 1)) ||
                        item.cpu >= (1u << KR_DEBUG_MAX_CPUS) ||
                        !item.address_bits || item.address_bits > 32 || item.address_shift > 1)
                        continue;
                    item.cpu |= side << instanceShift;
                    item.name[sizeof(item.name) - 1] = 0;
                    if (!target(item.cpu)) targets.push_back(item);
                }
            } else if (!side && !host.linkPair_ && api.debug_connect) {
                // The previous core exposes only main 68000 and Z80 debugging.
                for (unsigned id : {KR_DEBUG_M68K, KR_DEBUG_Z80}) {
                    kr_debug_cpu state{};
                    if (!api.debug_state(id, &state, sizeof(state))) continue;
                    const bool z80 = id == KR_DEBUG_Z80;
                    kr_debug_target item{};
                    item.cpu = id;
                    item.address_bits = z80 ? 16 : 24;
                    item.capabilities = debugEvents | KR_DEBUG_CAP_STEP | KR_DEBUG_CAP_OVER |
                                        KR_DEBUG_CAP_OUT | KR_DEBUG_CAP_STACK;
                    item.clock_hz = host.fps() < 55.0 ? 53203424u : 53693175u;
                    item.flags_kind = z80 ? KR_DEBUG_FLAGS_Z80 : KR_DEBUG_FLAGS_M68K;
                    std::strcpy(item.name, z80 ? "Z80" : "68000");
                    targets.push_back(item);
                }
            }
        }
        // Remove breakpoints only after rediscovering the final topology.
        // During a power cycle the old peer is temporarily absent.
        for (qsizetype i = breaks.size(); i > 0; --i) {
            if (!target(breaks[i - 1].cpu)) {
                breaks.removeAt(i - 1);
                conditions.removeAt(i - 1);
            }
        }
        if (!target(cpu) && !targets.isEmpty()) cpu = targets.front().cpu;
        if (!target(stepCpu)) clearStep();
    }

    bool state(unsigned id, kr_debug_cpu& result) const {
        auto* core = console(instance(id));
        if (!core || !core->api_->debug_state) return false;
        Activation active(core);
        if (!core->api_->debug_state(nativeCpu(id), &result, sizeof(result))) return false;
        result.cpu = id;
        result.count = std::min(result.count, KR_DEBUG_MAX_REGS);
        for (auto& item : result.regs) item.name[sizeof(item.name) - 1] = 0;
        return true;
    }

    bool disassemble(unsigned id, quint32 at, kr_debug_instruction& result) const {
        auto* core = console(instance(id));
        if (!core || !core->api_->debug_disassemble) return false;
        Activation active(core);
        return core->api_->debug_disassemble(nativeCpu(id), at, &result, sizeof(result));
    }

    void clearStep() {
        pauseNext = stepping = over = out = false;
        depth = pendingFlow = 0;
        flowHasDelaySlot = false;
    }

    static void entry() { entering->worker(); }
    void worker() {
        for (;;) {
            try {
                if (host.linkPair_) host.runLinkedFrame(renderVideo, captureAudio);
                else host.api_->run();
            } catch (...) { failure = std::current_exception(); }
            pending = false;
            co_switch(control);
        }
    }

    bool connect(unsigned side, const Masks* mask) {
        auto* core = console(side);
        if (!core) return !mask;
        const auto& api = *core->api_;
        Activation active(core);
        if (api.debug_connect_v2)
            return api.debug_connect_v2(mask ? hook : nullptr, mask ? &endpoints[side] : nullptr,
                                        mask ? mask->data() : nullptr, mask ? mask->size() : 0);
        return api.debug_connect && api.debug_connect(mask ? hook : nullptr, mask ? &endpoints[side] : nullptr,
                                                       mask ? (*mask)[0] : 0, mask ? (*mask)[1] : 0);
    }

    bool masks() {
        std::array<Masks, 2> enabled{};
        if ((pauseNext || stepping) && target(stepCpu))
            enabled[instance(stepCpu)][cpuIndex(nativeCpu(stepCpu))] |= KR_DEBUG_EXEC;
        for (const auto& item : breaks)
            if (item.enabled && target(item.cpu))
                enabled[instance(item.cpu)][cpuIndex(nativeCpu(item.cpu))] |= item.events;
        activeHooks = false;
        for (unsigned side = 0; side < endpoints.size(); ++side) {
            if (!console(side)) continue;
            const bool active = std::any_of(enabled[side].begin(), enabled[side].end(),
                                           [](unsigned mask) { return mask != 0; });
            if (!connect(side, active ? &enabled[side] : nullptr)) {
                connect(0, nullptr); connect(1, nullptr);
                activeHooks = false;
                return false;
            }
            activeHooks |= active;
        }
        return true;
    }

    bool canEdit(unsigned id) const {
        const auto* core = console(instance(id));
        return !host.moviePlayback_ && core && core->api_->debug_register_writable &&
               core->api_->debug_write_register &&
               (!pending || (stopped && stopCpu == id && event == KR_DEBUG_EXEC));
    }

    void rememberFlow(const kr_debug_target& chip, const kr_debug_instruction& instruction) {
        pendingFlow = instruction.flags & (KR_DEBUG_CALL | KR_DEBUG_RETURN);
        if ((pendingFlow & KR_DEBUG_RETURN) && chip.flags_kind == KR_DEBUG_FLAGS_SH2 &&
            instruction.bytes[0] == 0 && instruction.bytes[1] == 0x2b)
            pendingFlow |= exceptionReturn;
        flowHasDelaySlot = pendingFlow && (instruction.flags & KR_DEBUG_DELAYED);
        flowSlot = (instruction.address + (instruction.length >> chip.address_shift)) & addressMask(chip);
    }

    unsigned completedFlow(quint32 at) {
        if (flowHasDelaySlot && at == flowSlot) {
            flowHasDelaySlot = false;
            return 0;
        }
        flowHasDelaySlot = false;
        const auto completed = pendingFlow;
        pendingFlow = 0;
        return completed;
    }

    void stop(unsigned which, unsigned kind, quint32 at, quint32 val, unsigned bytes) {
        cpu = stopCpu = which;
        event = kind;
        address = at;
        value = val;
        accessSize = bytes;
        stopped = true;
        clearStep();
        // Linked stops occur on an endpoint's stack, under the frame fiber.
        resumeFiber = co_active();
        resumeOwner = active_;
        active_ = &host;
        co_switch(control);
    }

    static void hook(void* opaque, unsigned native, unsigned kind, uint32_t at, uint32_t val, unsigned bytes) {
        auto& endpoint = *static_cast<Endpoint*>(opaque);
        auto& d = *endpoint.debugger;
        if (!d.running || d.suppress) return;
        const unsigned which = native | (endpoint.instance << instanceShift);
        const auto* chip = d.target(which);
        if (!chip) return;
        kr_debug_cpu state{};
        bool haveState = false;
        const auto readState = [&] {
            if (!haveState) haveState = d.state(which, state);
            return haveState;
        };
        bool hit = false;
        for (qsizetype i = 0; i < d.breaks.size(); ++i) {
            auto& b = d.breaks[i];
            if (!b.enabled || b.cpu != which || !(b.events & kind)) continue;
            const quint32 mask = addressMask(*chip);
            const quint64 last = quint64(at) + std::max(1u, bytes) - 1;
            const bool overlaps = last <= mask ? !(at > b.last || last < b.first)
                                              : at <= b.last || (last & mask) >= b.first;
            if (!overlaps) continue;
            if (b.hits != std::numeric_limits<quint64>::max()) ++b.hits;
            if (b.hits <= b.ignoreHits) continue;
            const auto& condition = d.conditions[i];
            if (condition.source == Condition::Register && !readState()) continue;
            if (!condition.matches(state, at, val, bytes, b.hits)) continue;
            if (b.oneShot) b.enabled = false;
            hit = true;
        }
        if (kind == KR_DEBUG_EXEC && which == d.stepCpu) {
            if (d.pauseNext) hit = true;
            else if (d.stepping) {
                const bool sh2 = chip->flags_kind == KR_DEBUG_FLAGS_SH2;
                const bool slot = d.flowHasDelaySlot && at == d.flowSlot;
                const unsigned flow = d.completedFlow(at);
                const bool returning = flow & KR_DEBUG_RETURN;
                const bool outerReturn = returning && d.depth == 0;
                if (flow & KR_DEBUG_CALL) ++d.depth;
                else if (returning && d.depth) --d.depth;
                if (d.over) {
                    if (!slot && at == d.stepTarget && d.depth == 0 && readState() &&
                        (ascendingStack(*chip) ? state.sp <= d.startSp : state.sp >= d.startSp))
                        hit = true;
                } else if (d.out) {
                    if (outerReturn && ((sh2 && !(flow & exceptionReturn)) ||
                        (readState() && (ascendingStack(*chip) ?
                            state.sp < d.startSp : state.sp > d.startSp))))
                        hit = true;
                } else if (d.steps == 0) hit = true;
                else --d.steps;
                if (!hit && !slot && (d.out || (d.over && sh2))) {
                    kr_debug_instruction instruction{};
                    if (d.disassemble(which, at, instruction)) d.rememberFlow(*chip, instruction);
                }
            }
        }
        if (hit) d.stop(which, kind, at, val, bytes);
    }
};

thread_local LibretroHost::DebugExecution* LibretroHost::DebugExecution::entering = nullptr;
void LibretroHost::DebugExecutionDeleter::operator()(DebugExecution* p) const { delete p; }

bool LibretroHost::debuggerAttach(QString& error) {
    error.clear();
    if (!loaded_) { error = tr("Игра не загружена."); return false; }
    if (debugger_ && debugger_->pending) return true;

    if (!debugger_) debugger_.reset(new DebugExecution(*this));
    debugger_->discover();
    if (debugger_->targets.isEmpty() || (linkPair_ &&
        std::none_of(debugger_->targets.begin(), debugger_->targets.end(),
                     [](const auto& target) { return instance(target.cpu) == 1; }))) {
        debuggerDetach();
        error = tr("Отладка процессоров этой системы пока недоступна.");
        return false;
    }
    if (!debugger_->masks()) {
        debuggerDetach();
        error = tr("Для отладчика требуется обновлённое ядро.");
        return false;
    }
    return true;
}

void LibretroHost::debuggerDisconnectPeer() {
    if (!debugger_ || !linkPeer_) return;
    finishDebugFrame();
    debugger_->connect(1, nullptr);
}

void LibretroHost::debuggerDetach() {
    if (!debugger_) return;
    finishDebugFrame();
    debugger_->connect(0, nullptr);
    debugger_->connect(1, nullptr);
    debugger_.reset();
}

bool LibretroHost::debuggerAttached() const { return bool(debugger_); }
bool LibretroHost::debuggerStopped() const { return debugger_ && debugger_->stopped; }
bool LibretroHost::debuggerRunRequested() const {
    return debugger_ && (debugger_->stepping || debugger_->pauseNext);
}
bool LibretroHost::debuggerFramePending() const { return debugger_ && debugger_->pending; }

const QImage* LibretroHost::debuggerFramePreview(int role) const {
    return debuggerFramePending() ? &debugger_->previousPictures[role == 2 ? 1 : 0] : nullptr;
}

bool LibretroHost::runDebugFrame(bool video, bool audio) {
    if (!debugger_ || (!debugger_->pending && !debugger_->activeHooks)) {
        runFrame(video, audio);
        return true;
    }
    auto& d = *debugger_;
    if (!d.pending) {
        if (!loaded_) return true;
        if (runRate_ != fps()) {
            runRate_ = fps();
            ticksPerFrame_ = quint64(std::llround(4294967296.0 / runRate_));
        }
        if (!video) {
            spriteOwnership_ = {};
            if (linkPeer_) linkPeer_->spriteOwnership_ = {};
        }
        if (tracer_) traceBeforeFrame();
        if (coverage_) coverageBeforeFrame();
        d.renderVideo = video;
        d.captureAudio = audio;
        d.previousPictures[0] = frame_;
        d.previousPictures[1] = linkPeer_ ? linkPeer_->frame_ : frame_;
        if (!linkPair_) {
            videoEnabled_ = video;
            audioEnabled_ = audio;
            audio_.clear();
        }
        d.event = d.stopCpu = 0;
        d.pending = true;
    }
    d.control = co_active();
    if (!d.fiber) d.fiber = co_create(512 * 1024, DebugExecution::entry);
    if (!d.fiber) throw std::bad_alloc();

    auto* previous = DebugExecution::entering;
    DebugExecution::entering = &d;
    d.running = true;
    d.failure = nullptr;
    {
        Activation owner(d.resumeOwner ? d.resumeOwner : this);
        auto next = d.resumeFiber ? d.resumeFiber : d.fiber;
        d.resumeFiber = nullptr;
        d.resumeOwner = nullptr;
        co_switch(next);
    }
    d.running = false;
    DebugExecution::entering = previous;
    if (d.failure) std::rethrow_exception(d.failure);
    if (d.pending) return false;

    d.previousPictures = {};
    videoEnabled_ = audioEnabled_ = true;
    ++runClock_.frames;
    runClock_.ticks += ticksPerFrame_;
    return true;
}

void LibretroHost::finishDebugFrame() {
    if (!debugger_) return;
    if (debugger_->pending) {
        debugger_->suppress = true;
        runDebugFrame();
        debugger_->suppress = false;
    }
    debugger_->clearStep();
    debugger_->stopped = false;
    debugger_->masks();
}

bool LibretroHost::debuggerCommand(const DebuggerCommand& command, QString& error) {
    error.clear();
    const bool configuration = command.kind == DebuggerCommand::Configure || command.kind == DebuggerCommand::Validate;
    const bool temporaryInspection = command.kind == DebuggerCommand::Validate && !debuggerAttached();
    const auto releaseInspection = qScopeGuard([&] { if (temporaryInspection) debuggerDetach(); });
    const auto configureReply = qScopeGuard([&] {
        if (configuration && command.configureRequestId)
            debugConfigureError_ = error;
    });
    if (configuration && command.configureRequestId) {
        debugConfigureId_ = command.configureRequestId;
        debugConfigureAccepted_ = false;
    }
    if (command.kind == DebuggerCommand::Detach) { debuggerDetach(); return true; }
    if (command.kind == DebuggerCommand::SetLink) {
        if (moviePlayback_) {
            error = tr("Изменение Link Cable недоступно при записи или воспроизведении ввода.");
            return false;
        }
        return setLinkCableEnabled(command.linkEnabled, error);
    }
    if (configuration && !command.gameKey.isEmpty() &&
        command.gameKey != gamePath_ + QLatin1Char('\n') + contentName_) {
        error=tr("Игра изменилась. Обновите проект отладки.");
        return false;
    }
    if (!debuggerAttach(error)) return false;
    auto& d = *debugger_;
    auto* selectedTarget = d.target(command.cpu);
    if (!selectedTarget && command.kind == DebuggerCommand::WriteRegister) {
        error = tr("Процессор больше недоступен.");
        return false;
    }
    if (!selectedTarget) selectedTarget = d.target(d.cpu);
    if (!selectedTarget) selectedTarget = &d.targets.front();
    const unsigned selected = selectedTarget->cpu;
    kr_debug_cpu state{};
    if (!d.state(selected, state)) {
        error = tr("Не удалось прочитать состояние процессора.");
        return false;
    }
    if (command.kind != DebuggerCommand::Validate) {
        d.cpu = selected;
        d.navigate = command.address;
        d.follow = command.followPc;
    }
    if (command.kind == DebuggerCommand::Attach || command.kind == DebuggerCommand::Refresh ||
        command.kind == DebuggerCommand::Navigate)
        return true;

    if (command.kind == DebuggerCommand::WriteRegister) {
        if (!d.canEdit(selected)) {
            error = moviePlayback_
                ? tr("Изменение регистров недоступно при записи или воспроизведении ввода.")
                : tr("Регистр можно изменить на границе кадра или перед инструкцией этого процессора.");
            return false;
        }
        auto* core = d.console(instance(selected));
        const auto name = command.registerName.trimmed().toUpper().toLatin1();
        const auto* begin = static_cast<const kr_debug_reg*>(state.regs);
        const auto* end = begin + state.count;
        const auto* current = std::find_if(begin, end,
            [&](const auto& item) { return name == item.name; });
        Activation owner(core);
        const unsigned native = nativeCpu(selected);
        if (current == end || !core->api_->debug_register_writable(native, name.constData()) ||
            !core->api_->debug_write_register(native, name.constData(),
                (current->value & ~command.registerMask) | (command.registerValue & command.registerMask))) {
            error = tr("Ядро отклонило значение регистра.");
            return false;
        }
        ++inspectionGeneration_;
        debugToolsBoundary(KR_TRACE_REGISTER_EDIT);
        return true;
    }

    if (configuration) {
        if (command.breakpoints.size() > 128) { error = tr("Не более 128 точек остановки."); return false; }
        QSet<quint32> ids;
        QVector<Condition> compiled;
        compiled.reserve(command.breakpoints.size());
        for (const auto& b : command.breakpoints) {
            const auto* chip = d.target(b.cpu);
            kr_debug_cpu registers{};
            Condition condition;
            const unsigned native=nativeCpu(b.cpu);
            const bool known=instance(b.cpu)<=1 && native && !(native&(native-1)) && native<=KR_DEBUG_SSP1601;
            if (!chip && command.kind==DebuggerCommand::Validate && known && b.id &&
                !ids.contains(b.id) && b.first<=b.last && b.events && !(b.events&~debugEvents) &&
                b.condition.size()<=256) {
                // A saved absent peer/expansion stays dormant. Its register
                // condition is checked when that actual target becomes available.
                const unsigned bits=(native==KR_DEBUG_Z80 || native==KR_DEBUG_SSP1601)?16:
                    (native==KR_DEBUG_M68K || native==KR_DEBUG_SUB68K)?24:32;
                if (bits==32 || b.last<=(quint32(1)<<bits)-1) { ids.insert(b.id); continue; }
            }
            if (!b.id || ids.contains(b.id) || b.condition.size() > 256 || !chip || b.first > b.last || b.last > addressMask(*chip) || !b.events ||
                (b.events & ~(debugEvents & chip->capabilities)) || !d.state(b.cpu, registers) ||
                !condition.compile(b.condition, registers)) {
                error = tr("Неверная точка остановки или условие.");
                return false;
            }
            ids.insert(b.id);
            compiled.push_back(condition);
        }
        if (command.kind==DebuggerCommand::Validate) {
            if (command.configureRequestId) debugConfigureAccepted_=true;
            return true;
        }
        auto next = command.breakpoints;
        for (auto& b : next) {
            const auto old = std::find_if(d.breaks.cbegin(), d.breaks.cend(),
                [&](const auto& item) { return item.id == b.id; });
            if (!command.resetBreakpointHits && old != d.breaks.cend() && old->cpu == b.cpu && old->events == b.events &&
                old->first == b.first && old->last == b.last && old->condition == b.condition &&
                old->ignoreHits == b.ignoreHits && old->oneShot == b.oneShot) {
                b.hits = old->hits;
                if (b.id != command.editedBreakpointId) b.enabled = old->enabled;
            } else b.hits = 0;
        }
        auto previous = std::move(d.breaks);
        auto previousConditions = std::move(d.conditions);
        d.breaks = std::move(next);
        d.conditions = std::move(compiled);
        if (!d.masks()) {
            d.breaks = std::move(previous);
            d.conditions = std::move(previousConditions);
            d.masks();
            error = tr("Не удалось обновить точки остановки для текущих процессоров.");
            return false;
        }
        if (command.configureRequestId) debugConfigureAccepted_ = true;
        return true;
    }
    if (command.kind == DebuggerCommand::Pause) {
        if (!d.pending && (d.stepping || d.pauseNext)) {
            d.clearStep();
            d.stopped = true;
        } else {
            d.stepCpu = selected;
            d.pauseNext = true;
            d.stopped = false;
        }
    }
    if (command.kind == DebuggerCommand::Continue) {
        d.clearStep();
        d.stopped = false;
    }
    if (command.kind == DebuggerCommand::StepInto || command.kind == DebuggerCommand::StepOver ||
        command.kind == DebuggerCommand::StepOut) {
        const unsigned required = command.kind == DebuggerCommand::StepOver ? KR_DEBUG_CAP_OVER :
                                  command.kind == DebuggerCommand::StepOut ? KR_DEBUG_CAP_OUT : KR_DEBUG_CAP_STEP;
        if (!(selectedTarget->capabilities & required)) {
            error = tr("Этот процессор не поддерживает выбранный шаг.");
            return false;
        }
        if (!d.stopped && d.pending) { error = tr("Сначала остановите выполнение."); return false; }
        // Returning from the pending execute hook still runs the selected
        // instruction. Earlier hooks on other CPUs do not satisfy this step.
        const bool atExecute = d.pending && d.stopCpu == selected && d.event == KR_DEBUG_EXEC;
        d.clearStep();
        d.stepCpu = selected;
        d.steps = d.pending && d.stopCpu == selected ? 0 : 1;
        d.stepping = true;
        d.stopped = false;
        d.startSp = state.sp;
        kr_debug_instruction instruction{};
        const bool decoded = d.disassemble(selected, state.pc, instruction);
        if (command.kind == DebuggerCommand::StepOver && decoded && (instruction.flags & KR_DEBUG_CALL)) {
            d.over = true;
            const unsigned bytes = instruction.length + ((instruction.flags & KR_DEBUG_DELAYED) ? 2u : 0);
            d.stepTarget = (state.pc + (bytes >> selectedTarget->address_shift)) & addressMask(*selectedTarget);
            if (atExecute && selectedTarget->flags_kind == KR_DEBUG_FLAGS_SH2)
                d.rememberFlow(*selectedTarget, instruction);
        } else if (command.kind == DebuggerCommand::StepOut) {
            d.out = true;
            if (atExecute && decoded) d.rememberFlow(*selectedTarget, instruction);
        }
    }
    if (!d.masks()) {
        error = tr("Не удалось обновить точки остановки для текущих процессоров.");
        return false;
    }
    return true;
}

DebuggerSnapshot LibretroHost::debuggerSnapshot() const {
    DebuggerSnapshot out;
    out.configureRequestId = debugConfigureId_;
    out.configureAccepted = debugConfigureAccepted_;
    out.configureError = debugConfigureError_;
    out.generation = inspectionGeneration_;
    out.frame = runClock_.frames;
    out.linkSupported = loaded_ && supportsLinkCable();
    out.linkEnabled = bool(linkPair_);
    if (loaded_) out.gameKey = gamePath_ + QLatin1Char('\n') + contentName_;
    if (!debugger_) return out;

    const auto& d = *debugger_;
    out.attached = true;
    out.stopped = d.stopped;
    out.partialFrame = d.pending;
    out.selectedCpu = d.cpu;
    out.eventCpu = d.pending ? d.stopCpu : 0;
    out.event = d.pending ? d.event : 0;
    out.address = d.address;
    out.value = d.value;
    out.accessSize = d.accessSize;
    out.breakpoints = d.breaks;
    out.targets = d.targets;
    for (const auto& chip : out.targets) {
        kr_debug_cpu state{};
        if (d.state(chip.cpu, state)) out.cpus.push_back(state);
    }

    const auto* chip = d.target(d.cpu);
    auto* core = d.console(instance(d.cpu));
    const auto state = std::find_if(out.cpus.cbegin(), out.cpus.cend(),
        [&](const auto& item) { return item.cpu == d.cpu; });
    if (!chip || !core || state == out.cpus.cend()) return out;

    quint32 at = (d.follow ? state->pc : d.navigate) & addressMask(*chip);
    for (unsigned i = 0; i < 32; ++i) {
        kr_debug_instruction instruction{};
        if (!d.disassemble(d.cpu, at, instruction)) instruction.address = at;
        instruction.text[sizeof(instruction.text) - 1] = 0;
        if (!instruction.length) instruction.length = nativeCpu(d.cpu) == KR_DEBUG_Z80 ? 1 : 2;
        if (instruction.length > sizeof(instruction.bytes) ||
            instruction.length < (1u << chip->address_shift))
            break;
        out.instructions.push_back(instruction);
        at = (at + (instruction.length >> chip->address_shift)) & addressMask(*chip);
    }

    Activation active(core);
    const unsigned native = nativeCpu(d.cpu);
    if (d.canEdit(d.cpu)) {
        for (unsigned i = 0; i < state->count; ++i)
            if (core->api_->debug_register_writable(native, state->regs[i].name))
                out.writableRegisters.push_back(QString::fromLatin1(state->regs[i].name));
        out.canEdit = !out.writableRegisters.isEmpty();
    }
    if (chip->capabilities & KR_DEBUG_CAP_STACK) {
        out.stackAddress = state->sp & addressMask(*chip);
        out.stack.resize(64);
        out.stackValid.resize(64);
        const auto n = std::min<size_t>(64, core->api_->debug_peek(native, out.stackAddress,
            reinterpret_cast<uint8_t*>(out.stack.data()), reinterpret_cast<uint8_t*>(out.stackValid.data()), 64));
        out.stack.resize(qsizetype(n));
        out.stackValid.resize(qsizetype(n));
    }
    return out;
}
