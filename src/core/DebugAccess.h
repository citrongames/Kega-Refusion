#pragma once

#include "DebugExecutionAbi.h"
#include <QtGlobal>

// CPU and event are native KR_DEBUG_* bits. Execute addresses follow the
// processor's instruction address units; data events use logical byte addresses.
struct DebugAccessSubscription {
    unsigned instance, cpu, event;
    quint32 first, last;
};

struct DebugAccessEvent {
    unsigned instance, cpu, event;
    quint32 address, value;
    unsigned size;
};

inline quint32 debugAccessAddressMask(const kr_debug_target& target, unsigned event) {
    if (target.cpu == KR_DEBUG_SSP1601) return kr_debug_ssp_event_address_mask(event);
    return target.address_bits == 32 ? UINT32_MAX : (quint32(1) << target.address_bits) - 1;
}

inline bool debugAccessOverlaps(quint32 first, quint32 last, const DebugAccessEvent& access,
                               quint32 addressMask) {
    const quint64 end = quint64(access.address) + (access.size ? access.size - 1 : 0);
    if (end <= addressMask) return access.address <= last && end >= first;
    return access.address <= last || (end & addressMask) >= first;
}
