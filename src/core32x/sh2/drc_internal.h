#ifndef KEGA_SH2_DRC_INTERNAL_H
#define KEGA_SH2_DRC_INTERNAL_H
#include "drc.h"
#include <picodrive32x/cpu/sh2/sh2.h>
#include <picodrive32x/cpu/sh2/compiler.h>
#include <setjmp.h>
#include <string.h>
#include <limits.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SH2_READ_SHIFT 25
#define SH2_WRITE_SHIFT 25
#define SH2_DRCBLK_RAM_SHIFT 1
#define SH2_DRCBLK_DA_SHIFT 1
#define EL_STATUS 0
#define EL_ANOMALY 0
#define elprintf(...) ((void)0)
#define elprintf_sh2(...) ((void)0)
#define lprintf(...) ((void)0)
#define kega_drc_diagnostic(...) ((void)0)
typedef struct { uptr addr; u32 mask; } sh2_memmap;
typedef struct {
  u8 drcblk_ram[0x20000], drclit_ram[0x20000];
  u8 drcblk_da[2][0x800], drclit_da[2][0x800];
} KegaDrcMemory;
extern KegaDrcMemory kega_drc_memory;
extern SH2 *kega_drc_cpus[2];
extern int kega_drc_failed, kega_drc_rom_constants, kega_drc_guarded;
extern jmp_buf kega_drc_guard;
extern u8 *kega_drc_emit_end;
extern KegaSh2DrcStats kega_drc_stats;
void kega_drc_fail(void);
void kega_drc_emit_write(void *, const void *, size_t);
void kega_drc_emit_reserve(void *, size_t);
u32 kega_drc_rel32(const void *, const void *);
void kega_drc_publish(const void *, const void *);
bool kega_drc_code_valid(u32, SH2 *);
bool kega_drc_code_same_span(u32, u32, SH2 *);
u16 kega_drc_fetch16(SH2 *, u32);
void *p32x_sh2_get_mem_ptr(u32, u32 *, SH2 *);
int p32x_sh2_mem_is_rom(u32, SH2 *);
u32 p32x_sh2_poll_memory8(u32, u32, SH2 *);
u32 p32x_sh2_poll_memory16(u32, u32, SH2 *);
u32 p32x_sh2_poll_memory32(u32, u32, SH2 *);
static inline void memset32(void *p, u32 v, unsigned n) { u32 *q=p; while(n--) *q++=v; }
/* Only the DRC callback TU reserves RBX. Keep normal host/interpreter code
 * independent of the donor's fixed-register convention and disable LTO. */
u32 kega_sh2_host_read(SH2 *, u32, unsigned width);
void kega_sh2_host_write(SH2 *, u32, u32, unsigned width);
u16 kega_sh2_host_fetch(SH2 *, u32);
bool kega_sh2_host_fetch_is_pure(SH2 *, u32, size_t);
bool kega_sh2_is_running(const KegaSh2 *);
int kega_drc_execute(SH2 *, int);
int kega_sh2_service_irq(SH2 *, int);
void kega_drc_cpu_destroyed(KegaSh2 *);
#endif
