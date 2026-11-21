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
    if (!loaded_) {
        out.status = Localization::text(QStringLiteral("Игра не загружена"));
        return out;
    }
    if (linkCableEnabled()) {
        out.status = Localization::text(QStringLiteral("Просмотр связанных консолей пока не поддерживается."));
        return out;
    }
    if (!api_->inspect_info || !api_->inspect_read ||
        !api_->inspect_info(KR_INSPECT_VERSION,out.parts,&out.info,sizeof(out.info)) ||
        out.info.version != KR_INSPECT_VERSION || out.info.struct_size != sizeof(out.info) ||
        out.info.domain_count > KR_INSPECT_MAX_DOMAINS ||
        out.info.value_count > KR_INSPECT_MAX_VALUES) {
        out.info = {};
        out.status = Localization::text(QStringLiteral("Ядро не поддерживает просмотр состояния."));
        return out;
    }
    out.info.system[sizeof(out.info.system)-1] = 0;
    for (unsigned i=0; i<out.info.domain_count; ++i)
        out.info.domains[i].name[sizeof(out.info.domains[i].name)-1] = 0;
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
        const auto actual = api_->inspect_read(domain.id,offset,bytes.data(),size);
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
