#include "LibretroHost.h"
#include "LibretroHostApi.h"
#include "i18n/Localization.h"
#include <algorithm>
#include <cstring>

DebugInspectionSnapshot LibretroHost::inspect(const DebugInspectionRequest& request) const {
    DebugInspectionSnapshot out;
    out.serial = request.serial;
    out.parts = request.parts & 15u;
    out.generation = inspectionGeneration_;
    out.frame = runClock_.frames;
    if (loaded_) out.gameKey = gamePath_ + QLatin1Char('\n') + contentName_;
    out.instance = request.instance;
    out.instanceCount = linkPeer_ ? 2u : 1u;
    if (!loaded_) {
        out.status = Localization::text(QStringLiteral("Игра не загружена"));
        return out;
    }
    const auto* target = request.instance == 0 ? this : request.instance == 1 ? linkPeer_.get() : nullptr;
    if (!target || !target->loaded_) {
        out.status = Localization::text(QStringLiteral("Выбранная консоль недоступна."));
        return out;
    }
    const Activation scope(const_cast<LibretroHost*>(target));
    const auto& api = *target->api_;
    if (!api.inspect_info || !api.inspect_read ||
        !api.inspect_info(KR_INSPECT_VERSION,out.parts,&out.info,sizeof(out.info)) ||
        out.info.version != KR_INSPECT_VERSION || out.info.struct_size != sizeof(out.info) ||
        out.info.domain_count > KR_INSPECT_MAX_DOMAINS ||
        out.info.value_count > KR_INSPECT_MAX_VALUES) {
        out.info = {};
        out.status = Localization::text(QStringLiteral("Ядро не поддерживает просмотр состояния."));
        return out;
    }
    out.info.system[sizeof(out.info.system)-1] = 0;
    for (unsigned i=0; i<out.info.domain_count; ++i) {
        auto& domain = out.info.domains[i];
        domain.name[sizeof(domain.name)-1] = 0;
        if ((out.parts & KR_INSPECT_MEMORY) && api.inspect_write && api.inspect_writable &&
            api.inspect_writable(domain.id))
            out.writableDomains.append(domain.id);
    }
    for (unsigned i=0; i<out.info.value_count; ++i) {
        auto& value = out.info.values[i];
        value.device[sizeof(value.device)-1] = 0;
        value.name[sizeof(value.name)-1] = 0;
        value.digits = std::min(value.digits,16u);
    }
    auto read = [&](const kr_inspect_domain& domain, quint32 offset, quint32 length) {
        if (offset >= domain.size || !length) return QByteArray{};
        const auto size = std::min({length,domain.size-offset,quint32(KR_INSPECT_MAX_READ)});
        QByteArray bytes(qsizetype(size),Qt::Uninitialized);
        const auto actual = api.inspect_read(domain.id,offset,bytes.data(),size);
        if (actual > size) return QByteArray{};
        bytes.resize(qsizetype(actual));
        return bytes;
    };
    if (out.parts & KR_INSPECT_MEMORY) {
        const kr_inspect_domain* selected = nullptr;
        for (unsigned i=0; i<out.info.domain_count; ++i)
            if (out.info.domains[i].id == request.domain) selected = &out.info.domains[i];
        // A stale selection after changing system returns a valid new catalog,
        // not bytes from an unrelated domain.
        if (selected) {
            out.memoryDomain = selected->id;
            out.memoryOffset = request.offset;
            out.memory = read(*selected,request.offset,std::min(request.length,4096u));
        }
    }
    if (out.parts & KR_INSPECT_VIDEO) {
        for (unsigned i=0; i<out.info.domain_count; ++i) {
            const auto& d = out.info.domains[i];
            switch (d.id) {
            case KR_MEM_VRAM: case KR_MEM_CRAM: case KR_MEM_VSRAM:
            case KR_MEM_VDP_REGS: case KR_MEM_32X_FB0: case KR_MEM_32X_FB1:
            case KR_MEM_32X_CRAM:
                // Never present a truncated graphics domain as a full snapshot.
                if (d.size <= KR_INSPECT_MAX_READ) out.blocks.insert(d.id,read(d,0,d.size));
                break;
            default: break;
            }
        }
    }
    if (request.watchCatalog || !request.watches.isEmpty() || !request.scriptWatches.isEmpty()) {
        // One safe catalog per actual console, then only the requested 1–4 byte
        // values. No bus handlers, whole-memory scan or per-frame subscription.
        const auto projectCount = std::min(request.watches.size(), qsizetype(DebugWatchLimit));
        const auto scriptCount = std::min(request.scriptWatches.size(), qsizetype(DebugScriptWatchLimit));
        const auto watchAt = [&](qsizetype i) -> const DebugWatchEntry& {
            return i < projectCount ? request.watches[i] : request.scriptWatches[i - projectCount];
        };
        out.watchValues.reserve(projectCount + scriptCount);
        for (qsizetype i = 0; i < projectCount + scriptCount; ++i)
            out.watchValues.push_back({watchAt(i).id, {}, false});
        for (quint32 side=0; side<out.instanceCount; ++side) {
            const auto* console=side==0?this:linkPeer_.get();
            if (!console || !console->loaded_) continue;
            const Activation owner(const_cast<LibretroHost*>(console));
            const auto& native=*console->api_;
            kr_inspect_info info{};
            if (side==request.instance) info=out.info;
            else if (!native.inspect_info ||
                     !native.inspect_info(KR_INSPECT_VERSION,KR_INSPECT_MEMORY,&info,sizeof(info)))
                continue;
            if (info.version!=KR_INSPECT_VERSION || info.struct_size!=sizeof(info) ||
                info.domain_count>KR_INSPECT_MAX_DOMAINS || !native.inspect_read) continue;
            for (unsigned d=0; d<info.domain_count; ++d) {
                auto domain=info.domains[d];
                domain.name[sizeof(domain.name)-1]=0;
                if (request.watchCatalog) out.watchDomains.push_back({side,domain});
                for (qsizetype i = 0; i < out.watchValues.size(); ++i) {
                    const auto& watch = watchAt(i);
                    if (watch.instance!=side || watch.domain!=domain.id || !watch.id ||
                        (watch.width!=1 && watch.width!=2 && watch.width!=4) ||
                        watch.offset>=domain.size || watch.width>domain.size-watch.offset)
                        continue;
                    char bytes[4]{};
                    if (native.inspect_read(domain.id,watch.offset,bytes,watch.width)==watch.width) {
                        out.watchValues[i].bytes=QByteArray(bytes,int(watch.width));
                        out.watchValues[i].bigEndian=domain.big_endian!=0;
                    }
                }
            }
        }
    }
    return out;
}


bool LibretroHost::writeInspectionMemory(const DebugMemoryWrite& request, QString& error) {
    error.clear();
    if (!loaded_ || debuggerFramePending() || request.generation != inspectionGeneration_) {
        error = Localization::text(QStringLiteral("Обновите снимок памяти перед записью."));
        return false;
    }
    auto* target = request.instance == 0 ? this : request.instance == 1 ? linkPeer_.get() : nullptr;
    if (!target || !target->loaded_) {
        error = Localization::text(QStringLiteral("Выбранная консоль недоступна."));
        return false;
    }
    const Activation scope(target);
    const auto& api = *target->api_;
    const auto length = request.after.size();
    if (length <= 0 || length > KR_INSPECT_MAX_WRITE || request.before.size() != length ||
        !api.inspect_writable || !api.inspect_read || !api.inspect_write || !api.inspect_writable(request.domain)) {
        error = Localization::text(QStringLiteral("Эта область памяти доступна только для чтения."));
        return false;
    }
    char before[KR_INSPECT_MAX_WRITE];
    if (api.inspect_read(request.domain, request.offset, before, size_t(length)) != size_t(length) ||
        std::memcmp(before, request.before.constData(), size_t(length)) != 0) {
        error = Localization::text(QStringLiteral("Байты памяти изменились. Обновите снимок."));
        return false;
    }
    if (!inspectionWrite(request.instance, request.domain, request.offset,
                         request.after.constData(), size_t(length))) {
        error = Localization::text(QStringLiteral("Значение не подходит для выбранной области памяти."));
        return false;
    }
    return true;
}

bool LibretroHost::canWriteInspection() const {
    return loaded_ && !moviePlayback_ && !rewindPreview_ &&
           !debuggerFramePending() && !dispatchingDebugAccess_;
}

bool LibretroHost::inspectionDomainWritable(unsigned instance, quint32 domain) const {
    const auto* console = inspectionConsole(instance);
    if (!console || !console->api_->inspect_write || !console->api_->inspect_writable) return false;
    const Activation scope(const_cast<LibretroHost*>(console));
    return console->api_->inspect_writable(domain);
}

bool LibretroHost::inspectionWrite(unsigned instance, quint32 domain, quint32 offset,
                                  const void* data, size_t size) {
    if (!canWriteInspection() || !data || !size || size > KR_INSPECT_MAX_WRITE) return false;
    auto* console = const_cast<LibretroHost*>(inspectionConsole(instance));
    if (!console || !console->api_->inspect_write || !console->api_->inspect_writable) return false;
    {
        const Activation scope(console);
        if (!console->api_->inspect_writable(domain) ||
            console->api_->inspect_write(domain, offset, data, size) != size)
            return false;
    }
    ++inspectionGeneration_;
    debugToolsBoundary(KR_TRACE_MEMORY_EDIT);
    return true;
}

const LibretroHost* LibretroHost::inspectionConsole(unsigned instance) const {
    const auto* console = instance == 0 ? this : instance == 1 ? linkPeer_.get() : nullptr;
    return console && console->loaded_ ? console : nullptr;
}

unsigned LibretroHost::inspectionInstanceCount() const {
    return loaded_ ? 1u + unsigned(inspectionConsole(1) != nullptr) : 0;
}

bool LibretroHost::inspectionCatalog(unsigned instance, kr_inspect_info& catalog) const {
    const auto* console = inspectionConsole(instance);
    if (!console || !console->api_->inspect_info || !console->api_->inspect_read) return false;
    const Activation scope(const_cast<LibretroHost*>(console));
    catalog = {};
    if (!console->api_->inspect_info(KR_INSPECT_VERSION, KR_INSPECT_MEMORY, &catalog, sizeof(catalog)) ||
        catalog.version != KR_INSPECT_VERSION || catalog.struct_size != sizeof(catalog) ||
        catalog.domain_count > KR_INSPECT_MAX_DOMAINS)
        return false;
    catalog.system[sizeof(catalog.system) - 1] = 0;
    for (unsigned i = 0; i < catalog.domain_count; ++i)
        catalog.domains[i].name[sizeof(catalog.domains[i].name) - 1] = 0;
    return true;
}

bool LibretroHost::inspectionRead(unsigned instance, quint32 domain, quint32 offset,
                                  void* data, size_t size) const {
    const auto* console = inspectionConsole(instance);
    if (!console || !console->api_->inspect_read || !data || !size || size > KR_INSPECT_MAX_READ)
        return false;
    const Activation scope(const_cast<LibretroHost*>(console));
    return console->api_->inspect_read(domain, offset, data, size) == size;
}

QVector<kr_debug_target> LibretroHost::inspectionCpuTargets(unsigned instance) const {
    const auto* console = inspectionConsole(instance);
    if (!console || !console->api_->debug_state || !console->api_->debug_peek)
        return {};
    const Activation scope(const_cast<LibretroHost*>(console));
    const auto& api = *console->api_;
    QVector<kr_debug_target> result;
    if (api.debug_targets) {
        kr_debug_target targets[KR_DEBUG_MAX_CPUS]{};
        const auto count = std::min<size_t>(KR_DEBUG_MAX_CPUS,
            api.debug_targets(KR_DEBUG_VERSION, targets, KR_DEBUG_MAX_CPUS));
        for (size_t i = 0; i < count; ++i) {
            auto& target = targets[i];
            if (!target.cpu || (target.cpu & (target.cpu - 1)) ||
                target.cpu >= (1u << KR_DEBUG_MAX_CPUS) || !target.address_bits ||
                target.address_bits > 32 || target.address_shift > 1 ||
                std::any_of(result.cbegin(), result.cend(),
                            [&](const auto& item) { return item.cpu == target.cpu; }))
                continue;
            if (!api.debug_connect_v2 &&
                (!api.debug_connect || instance || linkPeer_ || target.cpu > KR_DEBUG_Z80))
                target.capabilities &= ~(KR_DEBUG_CAP_EXEC | KR_DEBUG_CAP_READ | KR_DEBUG_CAP_WRITE);
            target.name[sizeof(target.name) - 1] = 0;
            result.push_back(target);
        }
    } else {
        // Older compatible cores export state/peek for only the main 68k and Z80.
        for (unsigned cpu : { KR_DEBUG_M68K, KR_DEBUG_Z80 }) {
            kr_debug_cpu state{};
            if (!api.debug_state(cpu, &state, sizeof(state))) continue;
            kr_debug_target target{};
            const bool z80 = cpu == KR_DEBUG_Z80;
            target.cpu = cpu;
            target.address_bits = z80 ? 16 : 24;
            if (api.debug_connect && !instance && !linkPeer_)
                target.capabilities = KR_DEBUG_CAP_EXEC | KR_DEBUG_CAP_READ | KR_DEBUG_CAP_WRITE;
            std::strcpy(target.name, z80 ? "Z80" : "Main 68000");
            result.push_back(target);
        }
    }
    return result;
}

bool LibretroHost::inspectionRegisters(unsigned instance, unsigned cpu, kr_debug_cpu& state) const {
    const auto* console = inspectionConsole(instance);
    if (!console || !console->api_->debug_state) return false;
    const Activation scope(const_cast<LibretroHost*>(console));
    state = {};
    if (!console->api_->debug_state(cpu, &state, sizeof(state)) ||
        state.cpu != cpu || state.count > KR_DEBUG_MAX_REGS)
        return false;
    for (unsigned i = 0; i < state.count; ++i)
        state.regs[i].name[sizeof(state.regs[i].name) - 1] = 0;
    return true;
}

bool LibretroHost::inspectionPeek(unsigned instance, unsigned cpu, quint32 address,
                                  uint8_t* data, size_t size) const {
    const auto* console = inspectionConsole(instance);
    if (!console || !console->api_->debug_peek || !data || !size || size > KR_DEBUG_MAX_PEEK)
        return false;
    const Activation scope(const_cast<LibretroHost*>(console));
    uint8_t validity[KR_DEBUG_MAX_PEEK];
    return console->api_->debug_peek(cpu, address, data, validity, size) == size &&
        std::all_of(validity, validity + size, [](uint8_t valid) { return valid != 0; });
}

bool LibretroHost::inspectionCodeLocation(unsigned instance, unsigned cpu, quint32 address,
                                         kr_cov_location& location) const {
    const auto* console = inspectionConsole(instance);
    if (!console || !console->api_->debug_locations) return false;
    const Activation scope(const_cast<LibretroHost*>(console));
    location = {};
    return console->api_->debug_locations(KR_COV_VERSION, cpu, &address, &location, 1, 0) == 1 &&
           location.source != 0;
}
