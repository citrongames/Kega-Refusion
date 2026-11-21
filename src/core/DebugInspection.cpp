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
    if (api.inspect_write(request.domain, request.offset, request.after.constData(), size_t(length)) != size_t(length)) {
        error = Localization::text(QStringLiteral("Значение не подходит для выбранной области памяти."));
        return false;
    }
    ++inspectionGeneration_;
    return true;
}
