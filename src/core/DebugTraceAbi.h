#ifndef KEGA_DEBUG_TRACE_ABI_H
#define KEGA_DEBUG_TRACE_ABI_H
#include <stdint.h>
#include <stddef.h>
#define KR_TRACE_VERSION 1u
enum kr_trace_filter {
 KR_TRACE_INSTRUCTION=1, KR_TRACE_VIDEO=2, KR_TRACE_DMA=4,
 KR_TRACE_IRQ=8, KR_TRACE_DISC=16, KR_TRACE_LINK=32, KR_TRACE_MAPPER=64
};
enum kr_trace_kind {
 KR_TRACE_EXEC=1, KR_TRACE_VDP_WRITE, KR_TRACE_DMA_START, KR_TRACE_DMA_END,
 KR_TRACE_IRQ_ACCEPT, KR_TRACE_DISC_COMMAND, KR_TRACE_LINK_EDGE,
 KR_TRACE_BANK_CHANGE, KR_TRACE_DISCONTINUITY
};
enum kr_trace_device { KR_TRACE_CPU=0, KR_TRACE_VDP=1, KR_TRACE_CD=2,
 KR_TRACE_32X=3, KR_TRACE_MEGALD=4, KR_TRACE_CABLE=5, KR_TRACE_CART=6 };
enum kr_trace_flag { KR_TRACE_TIME_QUANTIZED=1, KR_TRACE_PEEK_TAIL=2,
 KR_TRACE_BYTES_TRUNCATED=4 };
/* Non-yielding observer, owned by the emulation thread. No strings, allocation,
 * filesystem or GUI calls. Timestamps may be delivered out of order by the
 * existing multi-CPU scheduler: preserve both raw local ticks and mapped MCLK.
 * The host adds frame/instance/sequence, never sorts away actual callback order.
 * EXEC first opcode comes from the actual fetched opcode, after any debugger
 * edit/IRQ redirection. Tail bytes are a bounded safe snapshot, explicitly tagged.
 * For hardware, address/value/aux carry the documented committed transaction. */
typedef struct kr_trace_record {
 int64_t mclk, local_ticks;
 uint32_t cpu, kind, device, address, value, aux, flags;
 uint8_t bytes[16];
 uint8_t valid_bytes, width;
 uint16_t reserved;
} kr_trace_record;
typedef void (*kr_trace_callback)(void *, const kr_trace_record *);
/* bool retro_kegarefusion_trace_bind_v1(unsigned version, uint32_t cpu_mask,
 *     uint32_t filters, kr_trace_callback, void *);
 * uint32_t retro_kegarefusion_trace_capabilities_v1(void);
 * Separate attachment from the invasive debugger. Null callback detaches.
 * cpu_mask combines native KR_DEBUG_* CPU bits, used only for instructions. */
#endif
