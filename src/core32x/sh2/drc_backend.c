#ifdef KEGA_SH2_DRC
/* Kega Refusion x86-64 SH2 DRC lifecycle. The upstream compiler and emitter
 * retain their notices and non-commercial license in third_party/picodrive32x.
 * No Pico MD/CD execution, frontend or global emulated machine is imported. */
#include "drc_internal.h"
#include <picodrive32x/cpu/drc/cmn.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
KegaDrcMemory kega_drc_memory;
KegaSh2DrcStats kega_drc_stats;
static KegaSh2DrcBudgetStats budget_stats;
SH2 *kega_drc_cpus[2];
int kega_drc_failed, kega_drc_rom_constants, kega_drc_guarded;
jmp_buf kega_drc_guard;
u8 *kega_drc_emit_end;
/* A dedicated aligned DSO allocation keeps all helper rel32 calls near code.
 * We explicitly check every emitted relative call/jump as well. */
static u8 ALIGNED(4096) code_cache[DRC_TCACHE_SIZE];
u8 *tcache;
static SH2 *executing;
static bool ready, flush_pending;
static unsigned depth;
#ifdef _WIN32
static DWORD old_protection;
#endif
static bool protect_cache(bool executable) {
#ifdef _WIN32
    DWORD ignored;
    return VirtualProtect(code_cache, sizeof(code_cache),
        executable ? PAGE_EXECUTE_READWRITE : old_protection,
        executable ? &old_protection : &ignored) != 0;
#else
    return mprotect(code_cache, sizeof(code_cache),
        PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0)) == 0;
#endif
}
void drc_cmn_init(void) { tcache = code_cache; }
void drc_cmn_cleanup(void) { tcache = NULL; }
void kega_drc_fail(void) {
    kega_drc_failed = 1;
    /* Called solely from guarded translator/initial utility generation. */
    if (kega_drc_guarded) longjmp(kega_drc_guard, 1);
}
void kega_drc_emit_write(void *dst, const void *src, size_t n) {
    uintptr_t a=(uintptr_t)dst, b=(uintptr_t)code_cache;
    if (a < b || n > sizeof(code_cache) || a-b > sizeof(code_cache)-n) {
        kega_drc_fail(); return;
    }
    memcpy(dst,src,n);
}
void kega_drc_emit_reserve(void *dst,size_t n) {
    if((uintptr_t)dst > (uintptr_t)kega_drc_emit_end ||
       n > (uintptr_t)kega_drc_emit_end-(uintptr_t)dst) kega_drc_fail();
}
u32 kega_drc_rel32(const void *target, const void *after) {
    intptr_t delta = (intptr_t)target - (intptr_t)after;
    if (delta < INT32_MIN || delta > INT32_MAX) { kega_drc_fail(); return 0; }
    return (u32)(int32_t)delta;
}
void kega_drc_publish(const void *start, const void *end) {
#ifdef _WIN32
    if (!FlushInstructionCache(GetCurrentProcess(), start,
        (const u8 *)end-(const u8 *)start)) {
        kega_drc_fail();
        if(executing) kega_sh2_end_run((KegaSh2 *)executing);
    }
#else
    __builtin___clear_cache((char *)start,(char *)end);
#endif
}
/* First backend intentionally emits handler accesses, including constant
 * addresses. No direct pointer can evade machine waits, peers or SoC MMIO. */
void *p32x_sh2_get_mem_ptr(u32 a, u32 *mask, SH2 *s) {
    (void)a; (void)s; *mask=0; return NULL;
}
int p32x_sh2_mem_is_rom(u32 a, SH2 *s) { (void)a; (void)s; return 0; }
static u32 code_region(u32 a, SH2 *s) {
    if ((a & 0xfffff000u)==0xc0000000u) return 4;
    a &= 0xc7ffffffu;
    if (a < (s->is_slave ? 0x400u : 0x800u)) return 1;
    if ((a & 0xffc00000u)==0x02000000u) return 2;
    if ((a & 0xfffc0000u)==0x06000000u) return 3;
    return 0;
}
bool kega_drc_code_valid(u32 a, SH2 *s) {
    return !(a&1) && code_region(a,s)!=0 && kega_sh2_host_fetch_is_pure(s,a,2);
}
bool kega_drc_code_same_span(u32 a, u32 b, SH2 *s) {
    return kega_drc_code_valid(b,s) && code_region(a,s)==code_region(b,s);
}
u16 kega_drc_fetch16(SH2 *s, u32 a) {
    if (!kega_drc_code_valid(a,s)) { kega_drc_fail(); return 0; }
    return kega_sh2_host_fetch(s,a);
}
/* Recoverable source/emitter assertions must never abort the emulator. */
#undef assert
#define assert(condition) do { if (!(condition)) kega_drc_fail(); } while (0)
#include <picodrive32x/cpu/sh2/compiler.c>

void kega_drc_bind_ack(SH2 *, bool);
static sh2_memmap read_maps[2][3][128];
static const void *write_maps[2][3][128];
static void setup_maps(SH2 *s) {
    static const void *readers[3] = {p32x_sh2_read8,p32x_sh2_read16,p32x_sh2_read32};
    static const void *writers[3] = {p32x_sh2_write8,p32x_sh2_write16,p32x_sh2_write32};
    unsigned i=!!s->is_slave, w, b;
    for(w=0;w<3;++w) for(b=0;b<128;++b) {
        read_maps[i][w][b].addr=((uptr)readers[w]>>1)|((uptr)1<<(sizeof(uptr)*8-1));
        read_maps[i][w][b].mask=0;
        write_maps[i][w][b]=writers[w];
    }
    s->read8_map=read_maps[i][0]; s->read16_map=read_maps[i][1]; s->read32_map=read_maps[i][2];
    s->write8_tab=write_maps[i][0]; s->write16_tab=write_maps[i][1]; s->write32_tab=write_maps[i][2];
    sh2_drc_mem_setup(s);
}
bool kega_sh2_drc_enable(KegaSh2 *master, KegaSh2 *slave) {
    if (ready) return kega_drc_cpus[0]==(SH2 *)master && kega_drc_cpus[1]==(SH2 *)slave && !kega_drc_failed;
    if (!master || !slave || master==slave || kega_sh2_is_running(master) || kega_sh2_is_running(slave)) return false;
    SH2 *m=kega_sh2_native(master), *s=kega_sh2_native(slave);
    if(m->is_slave || !s->is_slave || m->delay || s->delay) return false;
    kega_drc_cpus[0]=m; kega_drc_cpus[1]=s; kega_drc_failed=0;
    memset(&kega_drc_memory,0,sizeof(kega_drc_memory));
    kega_sh2_drc_reset_stats();
    if (!protect_cache(true)) goto fail;
    ready=true;
    if(sh2_drc_init(m) || sh2_drc_init(s) || kega_drc_failed) goto fail;
    setup_maps(m); setup_maps(s);
    kega_drc_bind_ack(m,true); kega_drc_bind_ack(s,true);
    m->run=s->run=kega_drc_execute;
    return true;
fail:
    if(ready) { kega_drc_failed=1; sh2_drc_finish(m); protect_cache(false); }
    ready=false; kega_drc_cpus[0]=kega_drc_cpus[1]=NULL;
    return false;
}
void kega_sh2_drc_disable(void) {
    if(!ready || depth) return;
    for(unsigned i=0;i<2;++i) {
        SH2 *s=kega_drc_cpus[i]; s->run=sh2_execute_interpreter;
        kega_drc_bind_ack(s,false);
        s->read8_map=s->read16_map=s->read32_map=NULL;
        s->write8_tab=s->write16_tab=s->write32_tab=NULL;
        s->p_drcblk_ram=s->p_drcblk_da=NULL;
    }
    sh2_drc_finish(kega_drc_cpus[0]);
    protect_cache(false); ready=false; flush_pending=false;
    kega_drc_cpus[0]=kega_drc_cpus[1]=NULL;
}
bool kega_sh2_drc_active(const KegaSh2 *cpu) {
    return ready && !kega_drc_failed && (kega_drc_cpus[0]==(const SH2 *)cpu || kega_drc_cpus[1]==(const SH2 *)cpu);
}
size_t kega_sh2_drc_cache_capacity(void) { return DRC_TCACHE_SIZE; }
void kega_sh2_drc_get_stats(KegaSh2DrcStats *stats) { if(stats) *stats=kega_drc_stats; }
void kega_sh2_drc_reset_stats(void) {
    memset(&kega_drc_stats,0,sizeof(kega_drc_stats));
    memset(&budget_stats,0,sizeof(budget_stats));
}
void kega_sh2_drc_get_budget_stats(KegaSh2DrcBudgetStats *stats) { if(stats) *stats=budget_stats; }
void kega_sh2_drc_flush(void) {
    if(!ready) return;
    if(depth) { flush_pending=true; kega_sh2_end_run((KegaSh2 *)executing); }
    else if(!kega_drc_failed) sh2_drc_flush_all();
}
void kega_sh2_drc_invalidate(KegaSh2 *cpu, u32 a, size_t length) {
    if(!ready || kega_drc_failed || !length) return;
    SH2 *s=kega_sh2_native(cpu); if(s!=kega_drc_cpus[0] && s!=kega_drc_cpus[1]) return;
    a = a >= 0xc0000000u ? a : a & 0xc7ffffffu;
    if ((a&0xfffc0000u)==0x06000000u) {
        if(length>0x40000u || (a&0x3ffffu)+length>0x40000u) { kega_sh2_drc_flush(); return; }
        bool marked=false;
        for(size_t i=(a&0x3ffffu)/2;i<=((a&0x3ffffu)+length-1)/2;++i)
            if(kega_drc_memory.drcblk_ram[i]) { marked=true; break; }
        if(!marked) return;
        sh2_drc_wcheck_ram(a,(unsigned)length,s);
    } else if ((a&0xe0000000u)==0xc0000000u) {
        if(length>0x1000u || (a&0xfffu)+length>0x1000u) { kega_sh2_drc_flush(); return; }
        bool marked=false;
        for(size_t i=(a&0xfffu)/2;i<=((a&0xfffu)+length-1)/2;++i)
            if(kega_drc_memory.drcblk_da[!!s->is_slave][i]) { marked=true; break; }
        if(!marked) return;
        sh2_drc_wcheck_da(0xc0000000u+(a&0xfffu),(unsigned)length,s);
    } else return;
    /* Current translated block remains allocated, but its following instruction
     * must not execute from a stale translation after a self/peer code write. */
    if(depth) kega_sh2_end_run((KegaSh2 *)executing);
}
int kega_drc_execute(SH2 *s, int budget) {
    unsigned i=!!s->is_slave;
    unsigned bin=budget>256 ? 9 : budget<=1 ? 0 : 32u-(unsigned)__builtin_clz((unsigned)budget-1u);
    if(!budget_stats.calls[i] || (unsigned)budget<budget_stats.min_budget[i]) budget_stats.min_budget[i]=(unsigned)budget;
    if((unsigned)budget>budget_stats.max_budget[i]) budget_stats.max_budget[i]=(unsigned)budget;
    ++budget_stats.calls[i]; budget_stats.requested_cycles[i]+=(unsigned)budget;
    ++budget_stats.histogram[i][bin];
    /* A nested peer executes reference code: no live caller code is recycled. */
    if(depth || kega_drc_failed || !ready || s->delay || budget>262144) {
        bool nested=depth!=0;
        int left=sh2_execute_interpreter(s,budget);
        uint64_t done=(uint64_t)((int64_t)s->cycles_timeslice-left);
        if(nested) {
            ++kega_drc_stats.nested_interpreter_calls[i];
            kega_drc_stats.nested_interpreter_cycles[i]+=done;
        } else {
            ++kega_drc_stats.fallback_interpreter_calls[i];
            kega_drc_stats.fallback_interpreter_cycles[i]+=done;
        }
        return left;
    }
    executing=s; ++depth;
    int left=sh2_execute_drc(s,budget);
    --depth; executing=NULL;
    if(flush_pending) { flush_pending=false; if(!kega_drc_failed) sh2_drc_flush_all(); }
    /* Translation failures return through the normal generated register flush.
     * Spend the remaining budget, rather than claiming unexecuted cycles. */
    ++kega_drc_stats.jit_calls[i];
    if(kega_drc_failed && left>0) {
        kega_drc_stats.jit_cycles[i]+=(uint64_t)((int64_t)s->cycles_timeslice-left);
        int remaining=sh2_execute_interpreter(s,left);
        ++kega_drc_stats.fallback_interpreter_calls[i];
        kega_drc_stats.fallback_interpreter_cycles[i]+=(uint64_t)((int64_t)left-remaining);
        return remaining;
    }
    /* A callback IRQ is accepted after its instruction, as in the interpreter. */
    s->icount=left;
    if(s->test_irq) left=kega_sh2_service_irq(s,left);
    kega_drc_stats.jit_cycles[i]+=(uint64_t)((int64_t)s->cycles_timeslice-left);
    return left;
}
void kega_drc_cpu_destroyed(KegaSh2 *cpu) {
    if(ready && ((SH2 *)cpu==kega_drc_cpus[0] || (SH2 *)cpu==kega_drc_cpus[1])) kega_sh2_drc_disable();
}

#endif /* KEGA_SH2_DRC */
