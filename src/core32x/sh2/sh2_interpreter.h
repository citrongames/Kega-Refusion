#ifndef KEGA_SH2_INTERPRETER_H
#define KEGA_SH2_INTERPRETER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KegaSh2 KegaSh2;
struct SH2_;

/* Memory callbacks use SH-2 logical addresses and numeric big-endian values.
 * They may query cycles, add bus wait cycles, change IRQs, or end this slice.
 * Do not reset/restore/destroy a CPU from its callbacks or run it recursively.
 * A callback may run the other CPU: no process-global active CPU is used. */
typedef struct KegaSh2Host {
    void *user;
    uint8_t (*read8)(void *user, KegaSh2 *cpu, uint32_t address);
    uint16_t (*read16)(void *user, KegaSh2 *cpu, uint32_t address);
    uint32_t (*read32)(void *user, KegaSh2 *cpu, uint32_t address);
    void (*write8)(void *user, KegaSh2 *cpu, uint32_t address, uint8_t value);
    void (*write16)(void *user, KegaSh2 *cpu, uint32_t address, uint16_t value);
    void (*write32)(void *user, KegaSh2 *cpu, uint32_t address, uint32_t value);
    /* External IRL only; NULL selects the SH-2 autovector 64 + level/2.
     * Internal IRQs use their explicitly supplied vector and auto-clear. */
    unsigned (*irq_ack)(void *user, KegaSh2 *cpu, unsigned level);
    /* Optional batch notification once after a run, never per instruction. */
    void (*cycles_elapsed)(void *user, KegaSh2 *cpu, uint32_t cycles);
    /* Optional instruction fetch path; NULL uses read16. Does not replace
     * data reads, so hosts can avoid shared-data synchronization on code fetch. */
    uint16_t (*fetch16)(void *user, KegaSh2 *cpu, uint32_t address);
    /* Optional pure predicate authorizing extra instruction peeks/batching.
     * It must validate the complete span and reject MMIO/handler side effects.
     * NULL forbids idle batching even when the optimization is enabled. */
    bool (*can_idle_fetch)(void *user, KegaSh2 *cpu, uint32_t address, size_t bytes);
} KegaSh2Host;

#define KEGA_SH2_STATE_VERSION 1u
/* Pointer-free CPU state, independent of the donor's native pointer layout.
 * This is an in-memory API object, not a raw-byte portable file format. */
typedef struct KegaSh2State {
    uint32_t version;
    uint32_t r[16], pc, ppc, pr, sr, gbr, vbr, mach, macl;
    uint32_t ea, delay, test_irq;
    uint32_t pending_level, pending_irl, pending_int_irq, pending_int_vector;
    uint32_t state, poll_addr, poll_cycles, poll_cnt, no_polling;
    uint32_t m68krcycles_done, mult_m68k_to_sh2, mult_sh2_to_m68k;
    uint8_t data_array[0x1000];
    uint32_t peri_regs[0x200 / 4];
    uint64_t cycles;
} KegaSh2State;

/* One aligned allocation per CPU; no allocation during reset/run/IRQ/state. */
KegaSh2 *kega_sh2_create(const KegaSh2Host *host, bool is_slave);
void kega_sh2_destroy(KegaSh2 *cpu);
void kega_sh2_set_peer(KegaSh2 *cpu, KegaSh2 *peer);
/* Opt-in exact BRA-self/NOP batching. The host guarantees that instruction
 * fetches authorized by can_idle_fetch are side-effect-free.
 * Events remain bounded by the supplied run budget; default is disabled.
 * Runtime optimization preference, retained across reset/state restore. */
bool kega_sh2_set_idle_fastpath(KegaSh2 *cpu, bool enabled);
/* Same native donor context, including maps/idle hints/run function, for the
 * optional SH-2 DRC and 32X SoC adapter; never serialize this structure as bytes. */
struct SH2_ *kega_sh2_native(KegaSh2 *cpu);
/* Clears architectural registers/CPU IRQs/cycle count; reads PC/SP at 0/4.
 * Host bindings, peer, memory maps, data-array and peripheral RAM are retained. */
bool kega_sh2_reset(KegaSh2 *cpu);
/* Returns actual cycles, possibly exceeding budget to finish an instruction,
 * its delay slot, or IRQ entry. Zero/invalid/reentrant requests consume none.
 * Budget must not exceed INT_MAX/2; an instruction is never interrupted. */
uint32_t kega_sh2_run(KegaSh2 *cpu, uint32_t budget);
uint64_t kega_sh2_cycles(const KegaSh2 *cpu);
bool kega_sh2_add_wait_cycles(KegaSh2 *cpu, uint32_t cycles);
void kega_sh2_end_run(KegaSh2 *cpu);
/* IRL: 0 clears, 1..15 asserts; internal: 0 clears, vector is 0..255. */
bool kega_sh2_set_irq(KegaSh2 *cpu, unsigned level);
bool kega_sh2_set_internal_irq(KegaSh2 *cpu, unsigned level, unsigned vector);
/* Peripheral controller observer: called after clearing the accepted internal
 * slot, before IRQ entry; may publish the next pending internal cause. External
 * IRL acknowledge does not invoke it. Runtime binding survives state restore. */
typedef void (*KegaSh2InternalIrqAck)(void *user, KegaSh2 *, unsigned level, unsigned vector);
void kega_sh2_set_internal_irq_ack(KegaSh2 *, KegaSh2InternalIrqAck, void *user);
bool kega_sh2_get_state(const KegaSh2 *cpu, KegaSh2State *state);
bool kega_sh2_validate_state(const KegaSh2State *state);
bool kega_sh2_set_state(KegaSh2 *cpu, const KegaSh2State *state);

#ifdef __cplusplus
}
#endif
#endif
