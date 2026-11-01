#ifndef KEGA_SH2_DRC_H
#define KEGA_SH2_DRC_H
#include "sh2_interpreter.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One process-wide, emulation-thread-owned pair, matching the libretro owner.
 * Optional x86-64 backend. Failure leaves both CPUs on the interpreter.
 * Call only while both CPUs are stopped; disable before destroying either.
 * No ROM/RAM data is cached: only bounded generated code and metadata.
 * Code fetch16 (or read16 fallback on code addresses) must be a side-effect-free
 * numeric peek: translation fetches ahead without advancing emulated time. */
bool kega_sh2_drc_enable(KegaSh2 *master, KegaSh2 *slave);
void kega_sh2_drc_disable(void);
bool kega_sh2_drc_active(const KegaSh2 *cpu);
/* Flush after ROM/bank changes or external state restore. A flush requested
 * from a host callback ends the active instruction and is deferred safely. */
void kega_sh2_drc_flush(void);
/* SH2 writes invalidate automatically. The machine must call this BEFORE
 * DMA/68K/debugger writes to SDRAM or this CPU's data array. Aliases accepted.
 * Non-RAM writes are ignored; ROM/BIOS replacement needs flush instead. */
void kega_sh2_drc_invalidate(KegaSh2 *cpu, uint32_t address, size_t length);
size_t kega_sh2_drc_cache_capacity(void);
/* Emulation-thread diagnostics, outside emulated/saved state. Calls/cycles are
 * native SH2 cycles, indexed master/slave, updated once per execution slice. */
typedef struct KegaSh2DrcStats {
    uint64_t jit_calls[2], jit_cycles[2];
    uint64_t nested_interpreter_calls[2], nested_interpreter_cycles[2];
    uint64_t fallback_interpreter_calls[2], fallback_interpreter_cycles[2];
    uint64_t cache_misses, cache_evictions;
} KegaSh2DrcStats;
void kega_sh2_drc_get_stats(KegaSh2DrcStats *stats);
void kega_sh2_drc_reset_stats(void);
/* Requested-budget bins: <=1, <=2, <=4, <=8, <=16, <=32, <=64,
 * <=128, <=256, >256. Separate getter keeps the basic stats layout stable. */
typedef struct KegaSh2DrcBudgetStats {
    uint64_t calls[2], requested_cycles[2], min_budget[2], max_budget[2];
    uint64_t histogram[2][10];
} KegaSh2DrcBudgetStats;
void kega_sh2_drc_get_budget_stats(KegaSh2DrcBudgetStats *stats);
#ifdef __cplusplus
}
#endif
#endif
