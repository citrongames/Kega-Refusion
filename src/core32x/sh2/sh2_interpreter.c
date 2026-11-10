/*
 * Kega Refusion host-independent SH-2 reference bridge.
 * Adapted from PicoDrive cpu/sh2/sh2.c and mame/sh2pico.c:
 * (C) notaz, 2009,2010; (C) irixxxx, 2019-2024.
 * Licensed under the non-commercial terms in third_party/picodrive32x/COPYING.
 * Changed for Kega Refusion: per-instance callbacks, cycle accounting,
 * pointer-free snapshots, and no Pico/global CPU/frontend dependencies.
 * The included opcode implementation retains its notice; ORIGIN.txt records
 * the narrow well-defined sign-extension corrections.
 */
#include "sh2_interpreter.h"
#ifdef KEGA_SH2_DRC
#include "drc_internal.h"
#endif
#include <picodrive32x/cpu/sh2/sh2.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct KegaSh2 {
    SH2 core; /* First member: same context/address can later execute the DRC. */
    KegaSh2Host host;
    void *allocation;
    uint64_t cycles;
    bool running, stop, idle_fastpath;
    KegaSh2InternalIrqAck internal_ack;
    void *internal_ack_user;
};

static KegaSh2 *owner(SH2 *core) { return (KegaSh2 *)core; }
/* The machine's SDRAM and local data array store native 16-bit words.
 * SH-2 byte reads select the high or low byte of that numeric word. */
static void *plain_base(SH2 *s, uint32_t a, uint32_t *offset) {
    if (s->p_da && (a & 0xe0000000u)==0xc0000000u) {
        *offset=a & 0xfffu;
        return s->p_da;
    }
    if (s->p_sdram && (a & 0xc6000000u)==0x06000000u) {
        *offset=a & 0x3ffffu;
        return s->p_sdram;
    }
    return NULL;
}
static uint16_t plain_word(const void *base, uint32_t offset) {
    uint16_t word;
    memcpy(&word, (const uint8_t *)base + offset, sizeof(word));
    return word;
}
static uint8_t read8(SH2 *s, uint32_t a) {
    uint32_t offset;
    const void *base=plain_base(s,a,&offset);
    if (base) {
        uint16_t word=plain_word(base,offset & ~1u);
        return (uint8_t)(word >> ((offset & 1u) ? 0 : 8));
    }
    KegaSh2 *c = owner(s); return c->host.read8(c->host.user, c, a);
}
static uint16_t read16(SH2 *s, uint32_t a) {
    uint32_t offset;
    const void *base=plain_base(s,a,&offset);
    if (base) return plain_word(base,offset & ~1u);
    KegaSh2 *c = owner(s); return c->host.read16(c->host.user, c, a);
}
static uint16_t fetch16(SH2 *s, uint32_t a) {
    uint32_t offset;
    const void *base=(a & 1u) ? NULL : plain_base(s,a,&offset);
    if (base) return plain_word(base,offset);
    KegaSh2 *c = owner(s);
    return c->host.fetch16 ? c->host.fetch16(c->host.user, c, a) : read16(s, a);
}
static uint32_t read32(SH2 *s, uint32_t a) {
    uint32_t offset;
    const void *base=plain_base(s,a,&offset);
    if (base) {
        offset &= ~3u;
        return ((uint32_t)plain_word(base,offset) << 16) |
            plain_word(base,offset+2u);
    }
    KegaSh2 *c = owner(s); return c->host.read32(c->host.user, c, a);
}
/* Plain machine RAM has no write side effects other than translated code
   invalidation. Check the affected native word(s), including unaligned
   guest addresses and physical mirrors, before changing their contents. */
static inline bool store_plain(SH2 *s, uint32_t a, uint32_t v, unsigned size) {
    uint32_t offset;
    uint8_t *base=plain_base(s,a,&offset);
    if (!base) return false;
    offset &= ~(size-1u);
#ifdef KEGA_SH2_DRC
    bool local=(a & 0xe0000000u)==0xc0000000u;
    const uint8_t *marked=local?s->p_drcblk_da:s->p_drcblk_ram;
    if (marked && (marked[offset>>1] || (size==4 && marked[(offset>>1)+1]))) {
        uint32_t physical=(local?0xc0000000u:0x06000000u) | offset;
        kega_sh2_drc_invalidate(owner(s),physical,size);
    }
#endif
    uint16_t word=(uint16_t)v;
    if (size==1) {
        unsigned shift=(offset & 1u)?0:8;
        offset &= ~1u;
        word=(uint16_t)((plain_word(base,offset) & ~(0xffu<<shift)) | ((v & 0xffu)<<shift));
    } else if (size==4) {
        uint16_t high=(uint16_t)(v>>16);
        memcpy(base+offset,&high,sizeof(high));
        offset+=2;
    }
    memcpy(base+offset,&word,sizeof(word));
    return true;
}
static void write8(SH2 *s, uint32_t a, uint8_t v) {
    if (store_plain(s,a,v,1)) return;
    KegaSh2 *c = owner(s);
#ifdef KEGA_SH2_DRC
    kega_sh2_drc_invalidate(c,a,1);
#endif
    c->host.write8(c->host.user, c, a, v);
}
static void write16(SH2 *s, uint32_t a, uint16_t v) {
    if (store_plain(s,a,v,2)) return;
    KegaSh2 *c = owner(s);
#ifdef KEGA_SH2_DRC
    kega_sh2_drc_invalidate(c,a,2);
#endif
    c->host.write16(c->host.user, c, a, v);
}
static void write32(SH2 *s, uint32_t a, uint32_t v) {
    if (store_plain(s,a,v,4)) return;
    KegaSh2 *c = owner(s);
#ifdef KEGA_SH2_DRC
    kega_sh2_drc_invalidate(c,a,4);
#endif
    c->host.write32(c->host.user, c, a, v);
}

/* Only the opcode implementation is included, not Pico's dispatcher. */
typedef int8_t INT8;
typedef int16_t INT16;
typedef int32_t INT32;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
#define sh2_state SH2
#define RB(s,a) read8((s),(a))
#define RW(s,a) read16((s),(a))
#define RL(s,a) read32((s),(a))
#define WB(s,a,v) write8((s),(a),(uint8_t)(v))
#define WW(s,a,v) write16((s),(a),(uint16_t)(v))
#define WL(s,a,v) write32((s),(a),(v))
#define T 0x00000001u
#define S 0x00000002u
#define I 0x000000f0u
#define Q 0x00000100u
#define M 0x00000200u
#define AM 0xc7ffffffu
#define FLAGS (M|Q|I|S|T)
#define Rn ((opcode >> 8) & 15)
#define Rm ((opcode >> 4) & 15)
#define BUSY_LOOP_HACKS 0 /* Reference path; keep native idle/poll hints intact. */
#define rlog(x) ((void)0)
#define rlog1(...) ((void)0)
#define rlog2(...) ((void)0)
#define logerror(...) ((void)0)
#include <picodrive32x/cpu/sh2/mame/sh2.c>

static void refresh_irq(SH2 *s) {
    s->pending_level = s->pending_irl > s->pending_int_irq
        ? s->pending_irl : s->pending_int_irq;
    s->test_irq = 1;
}

static int REGPARM(2) acknowledge_irq(SH2 *s, int level) {
    KegaSh2 *c = owner(s);
    unsigned vector;
    /* SH7604 table5.4: external IRL wins ties against on-chip modules. */
    if (s->pending_irl >= s->pending_int_irq) {
        vector = c->host.irq_ack
            ? c->host.irq_ack(c->host.user, c, (unsigned)level)
            : 64u + (unsigned)level / 2;
    } else {
        vector = (unsigned)s->pending_int_vector;
        s->pending_int_irq = 0;
        refresh_irq(s);
        if (c->internal_ack)
            c->internal_ack(c->internal_ack_user, c, (unsigned)level, vector & 255u);
    }
    return (int)(vector & 255u);
}

static void take_irq(SH2 *s) {
    int level = s->pending_level;
    s->test_irq = 0;
    if (level > (int)((s->sr >> 4) & 15)) {
        unsigned vector = (unsigned)s->irq_callback(s, level);
        s->sr &= FLAGS;
        s->r[15] -= 4; write32(s, s->r[15], s->sr);
        s->r[15] -= 4; write32(s, s->r[15], s->pc);
        s->sr = (s->sr & ~I) | ((uint32_t)level << 4);
        s->pc = read32(s, s->vbr + vector * 4);
        s->icount -= 13;
    }
}

/* Dispatch follows PicoDrive sh2pico.c, preserving atomic delay-slot execution. */
int sh2_execute_interpreter(SH2 *s, int cycles) {
    KegaSh2 *c = owner(s);
    s->icount = cycles;
    if (!s->delay) take_irq(s);
    while ((s->icount > 0 && !c->stop) || s->delay) {
        uint16_t opcode;
        if (s->delay) {
            s->ppc = s->delay;
            opcode = fetch16(s, s->delay);
            if ((opcode >> 13) == 5) { /* BRA/BSR in a delay slot. */
                s->r[15] -= 4; write32(s, s->r[15], s->sr);
                s->r[15] -= 4; write32(s, s->r[15], s->pc);
                s->pc = read32(s, s->vbr + 6 * 4);
                s->icount -= 5;
                opcode = 9;
            }
            s->pc -= 2;
        } else {
            s->ppc = s->pc;
            opcode = fetch16(s, s->pc);
        }
        if(c->idle_fastpath && !s->delay && s->icount>0 && !c->stop &&
           opcode==0xaffe && s->pc<=UINT32_MAX-3u && c->host.can_idle_fetch &&
           c->host.can_idle_fetch(c->host.user,c,s->pc,4) &&
           s->pending_level<=(int)((s->sr>>4)&15) && fetch16(s,s->pc+2)==0x0009) {
            /* One complete BRA (2 cycles) + its NOP delay slot (1). The
             * reference always finishes the pair, including budget 1 or 2. */
            unsigned pairs=((unsigned)s->icount+2u)/3u;
            s->ppc=s->pc+2; s->ea=s->pc; s->delay=0;
            s->icount-=(int)(pairs*3u);
            continue;
        }
        s->delay = 0;
        s->pc += 2;
        switch (opcode >> 12) {
        case 0: op0000(s, opcode); break;
        case 1: op0001(s, opcode); break;
        case 2: op0010(s, opcode); break;
        case 3: op0011(s, opcode); break;
        case 4: op0100(s, opcode); break;
        case 5: op0101(s, opcode); break;
        case 6: op0110(s, opcode); break;
        case 7: op0111(s, opcode); break;
        case 8: op1000(s, opcode); break;
        case 9: op1001(s, opcode); break;
        case 10: op1010(s, opcode); break;
        case 11: op1011(s, opcode); break;
        case 12: op1100(s, opcode); break;
        case 13: op1101(s, opcode); break;
        case 14: op1110(s, opcode); break;
        default: op1111(s, opcode); break;
        }
        --s->icount;
        if (s->test_irq && !s->delay) take_irq(s);
    }
    return s->icount;
}

KegaSh2 *kega_sh2_create(const KegaSh2Host *host, bool is_slave) {
    void *allocation;
    KegaSh2 *c;
    if (!host || !host->read8 || !host->read16 || !host->read32 ||
        !host->write8 || !host->write16 || !host->write32) return NULL;
    allocation = malloc(sizeof(*c) + 31);
    if (!allocation) return NULL;
    c = (KegaSh2 *)(((uintptr_t)allocation + 31) & ~(uintptr_t)31);
    memset(c, 0, sizeof(*c));
    c->allocation = allocation;
    c->host = *host;
    c->core.is_slave = is_slave ? 1 : 0;
    c->core.irq_callback = acknowledge_irq;
    c->core.run = sh2_execute_interpreter;
    return c;
}
void kega_sh2_destroy(KegaSh2 *cpu) {
    if (cpu && !cpu->running) {
#ifdef KEGA_SH2_DRC
        kega_drc_cpu_destroyed(cpu);
#endif
        free(cpu->allocation);
    }
}
void kega_sh2_set_peer(KegaSh2 *cpu, KegaSh2 *peer) {
    if (cpu) cpu->core.other_sh2 = peer ? &peer->core : NULL;
}
bool kega_sh2_set_idle_fastpath(KegaSh2 *cpu, bool enabled) {
    if(!cpu || cpu->running) return false;
    cpu->idle_fastpath=enabled; return true;
}
struct SH2_ *kega_sh2_native(KegaSh2 *cpu) { return cpu ? &cpu->core : NULL; }
bool kega_sh2_reset(KegaSh2 *cpu) {
    SH2 *s;
    if (!cpu || cpu->running) return false;
    s = &cpu->core;
#ifdef KEGA_SH2_DRC
    kega_sh2_drc_flush();
#endif
    memset(s->r, 0, offsetof(SH2, read8_map));
    s->state = s->poll_addr = s->poll_cycles = s->poll_cnt = s->no_polling = 0;
    s->ea = s->delay = s->test_irq = 0;
    s->icount = s->irq_cycles = 0;
    s->pending_level = s->pending_irl = s->pending_int_irq = s->pending_int_vector = 0;
    s->rts_cache_idx = 0;
    memset(s->rts_cache, -1, sizeof(s->rts_cache));
    memset(s->branch_cache, -1, sizeof(s->branch_cache));
    cpu->cycles = 0; cpu->stop = false;
    s->pc = read32(s, 0); s->r[15] = read32(s, 4); s->sr = I;
    return true;
}
uint32_t kega_sh2_run(KegaSh2 *cpu, uint32_t budget) {
    int remaining;
    uint32_t done;
    if (!cpu || cpu->running || !budget || budget > INT_MAX / 2) return 0;
    cpu->running = true; cpu->stop = false;
    cpu->core.state |= SH2_STATE_RUN;
    cpu->core.cycles_timeslice = budget;
    remaining = cpu->core.run(&cpu->core, (int)budget);
    done = (uint32_t)((int64_t)cpu->core.cycles_timeslice - remaining);
    cpu->cycles += done;
    cpu->running = false;
    cpu->core.state &= ~SH2_STATE_RUN;
    if (cpu->host.cycles_elapsed)
        cpu->host.cycles_elapsed(cpu->host.user, cpu, done);
    return done;
}
uint64_t kega_sh2_cycles(const KegaSh2 *cpu) {
    if (!cpu) return 0;
    int left=cpu->core.icount;
#ifdef KEGA_SH2_DRC
    if(cpu->core.state & SH2_IN_DRC) left=((int32_t)cpu->core.sr >> 12)+1;
#endif
    return cpu->cycles + (cpu->running
        ? (uint64_t)((int64_t)cpu->core.cycles_timeslice - left) : 0);
}
bool kega_sh2_add_wait_cycles(KegaSh2 *cpu, uint32_t cycles) {
    int64_t remaining;
    if (!cpu || !cpu->running) return false;
#ifdef KEGA_SH2_DRC
    if(cpu->core.state & SH2_IN_DRC) {
        remaining=((int32_t)cpu->core.sr >> 12)-(int64_t)cycles;
        if(remaining < -524288) return false;
        cpu->core.sr=(cpu->core.sr & 0xfffu) | ((uint32_t)remaining << 12);
        return true;
    }
#endif
    remaining = (int64_t)cpu->core.icount - cycles;
    if (remaining < INT_MIN + 128) return false;
    cpu->core.icount = (int)remaining;
    return true;
}
void kega_sh2_end_run(KegaSh2 *cpu) {
    if(!cpu) return;
    cpu->stop=true;
#ifdef KEGA_SH2_DRC
    if(cpu->running && (cpu->core.state & SH2_IN_DRC)) {
        int left=((int32_t)cpu->core.sr >> 12)+1;
        if(left>0) {
            cpu->core.cycles_timeslice-=(unsigned)left;
            cpu->core.sr=(cpu->core.sr & 0xfffu) | 0xfffff000u;
        }
    }
#endif
}
bool kega_sh2_set_irq(KegaSh2 *cpu, unsigned level) {
    if (!cpu || level > 15) return false;
    cpu->core.pending_irl = (int)level; refresh_irq(&cpu->core); return true;
}
bool kega_sh2_set_internal_irq(KegaSh2 *cpu, unsigned level, unsigned vector) {
    if (!cpu || level > 15 || vector > 255) return false;
    cpu->core.pending_int_irq = (int)level;
    cpu->core.pending_int_vector = (int)vector;
    refresh_irq(&cpu->core); return true;
}

void kega_sh2_set_internal_irq_ack(KegaSh2 *cpu, KegaSh2InternalIrqAck callback, void *user) {
    if (cpu) { cpu->internal_ack = callback; cpu->internal_ack_user = user; }
}

#define COPY_FIELD(dst,src,field) memcpy(&(dst)->field, &(src)->field, sizeof((dst)->field))
#define STATE_FIELDS(X) \
    X(r) X(pc) X(ppc) X(pr) X(sr) X(gbr) X(vbr) X(mach) X(macl) \
    X(ea) X(delay) X(test_irq) X(pending_level) X(pending_irl) \
    X(pending_int_irq) X(pending_int_vector) X(state) X(poll_addr) X(poll_cycles) \
    X(poll_cnt) X(no_polling) X(m68krcycles_done) X(mult_m68k_to_sh2) \
    X(mult_sh2_to_m68k) X(data_array) X(peri_regs)
bool kega_sh2_get_state(const KegaSh2 *cpu, KegaSh2State *state) {
    if (!cpu || !state || cpu->running) return false;
    memset(state, 0, sizeof(*state));
    state->version = KEGA_SH2_STATE_VERSION;
#define EXPORT(field) COPY_FIELD(state, &cpu->core, field);
    STATE_FIELDS(EXPORT)
#undef EXPORT
    state->state &= ~(SH2_STATE_RUN | SH2_IN_DRC);
    state->cycles = cpu->cycles;
    return true;
}
bool kega_sh2_validate_state(const KegaSh2State *state) {
    unsigned pending;
    if (!state || state->version != KEGA_SH2_STATE_VERSION ||
        (state->sr & ~FLAGS) || state->pending_irl > 15 || state->pending_int_irq > 15 ||
        state->pending_int_vector > 255 || state->test_irq > 1 ||
        (state->state & (SH2_STATE_RUN | SH2_IN_DRC))) return false;
    pending = state->pending_irl > state->pending_int_irq
        ? state->pending_irl : state->pending_int_irq;
    if (state->pending_level != pending || state->poll_cnt > INT_MAX) return false;
    return true;
}
bool kega_sh2_set_state(KegaSh2 *cpu, const KegaSh2State *state) {
    if(!cpu || cpu->running || !kega_sh2_validate_state(state)) return false;
#ifdef KEGA_SH2_DRC
    kega_sh2_drc_flush();
#endif
#define IMPORT(field) COPY_FIELD(&cpu->core, state, field);
    STATE_FIELDS(IMPORT)
#undef IMPORT
    cpu->cycles = state->cycles;
    cpu->core.icount = 0; cpu->core.cycles_timeslice = 0;
    cpu->core.rts_cache_idx = 0;
    memset(cpu->core.rts_cache, -1, sizeof(cpu->core.rts_cache));
    memset(cpu->core.branch_cache, -1, sizeof(cpu->core.branch_cache));
    cpu->stop = false;
    return true;
}

#ifdef KEGA_SH2_DRC
bool kega_sh2_is_running(const KegaSh2 *cpu) { return cpu && cpu->running; }
u32 kega_sh2_host_read(SH2 *s,u32 a,unsigned width) {
    return width==8 ? read8(s,a) : width==16 ? read16(s,a) : read32(s,a);
}
void kega_sh2_host_write(SH2 *s,u32 a,u32 v,unsigned width) {
    if(width==8) write8(s,a,(uint8_t)v); else if(width==16) write16(s,a,(uint16_t)v); else write32(s,a,v);
}
u16 kega_sh2_host_fetch(SH2 *s,u32 a) { return fetch16(s,a); }
bool kega_sh2_host_fetch_is_pure(SH2 *s,u32 a,size_t bytes) {
    KegaSh2 *c=owner(s);
    /* Legacy DRC hosts already promise pure code fetches. A supplied predicate
     * narrows that contract for bank/handler pages before any compile peek. */
    return !c->host.can_idle_fetch || c->host.can_idle_fetch(c->host.user,c,a,bytes);
}
int kega_sh2_service_irq(SH2 *s,int remaining) {
    s->icount=remaining;
    if(!s->delay) take_irq(s);
    return s->icount;
}
#endif
