/* Kega Refusion SH-2 SoC; derived from PicoDrive 07abb005 sh2soc.c. */
#ifndef KEGA_SH2_SOC_H
#define KEGA_SH2_SOC_H
#include "sh2_interpreter.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct KegaSh2Soc KegaSh2Soc;
typedef struct KegaSh2SocHost {
    void *user;
    uint8_t (*read8)(void *, KegaSh2 *, uint32_t);
    uint16_t (*read16)(void *, KegaSh2 *, uint32_t);
    uint32_t (*read32)(void *, KegaSh2 *, uint32_t);
    void (*write8)(void *, KegaSh2 *, uint32_t, uint8_t);
    void (*write16)(void *, KegaSh2 *, uint32_t, uint16_t);
    void (*write32)(void *, KegaSh2 *, uint32_t, uint32_t);
    /* Optional: NULL sends the internal IRQ directly to the associated CPU. */
    /* Includes level0 to clear the internal line. Controller callbacks must
     * forward the selected level/vector to kega_sh2_set_internal_irq(). */
    void (*interrupt)(void *, KegaSh2 *, unsigned level, unsigned vector);
    /* Absolute native SH-2 cycles; UINT64_MAX means no pending event. */
    void (*schedule_changed)(void *, KegaSh2 *, uint64_t deadline);
    /* Committed words in the shared 32X DREQ0 FIFO; never exposes partial groups. */
    unsigned (*dreq_available)(void *, KegaSh2 *);
    uint16_t (*dreq_pop)(void *, KegaSh2 *);
} KegaSh2SocHost;

#define KEGA_SH2_SOC_STATE_VERSION 1u
/* Pending-cause arbitration: highest level first. SH7604 table5.4 fixes ties:
 * DIVU > DMA0 > DMA1 > WDT > SCI(error,RX,TX,end) > FRT(capture,compare,overflow).
 * Programmed vector numbers do not change source priority. Each cause is a
 * level request derived from its status flag plus interrupt-enable bit. CPU
 * acknowledge does not clear it: the ISR must clear/disable the source or it
 * remains pending after RTE. Lower active causes remain pending throughout. */
enum KegaSh2SocIrqCause {
    KEGA_SH2_SOC_FRT_CAPTURE, KEGA_SH2_SOC_FRT_COMPARE, KEGA_SH2_SOC_FRT_OVERFLOW,
    KEGA_SH2_SOC_DMA0, KEGA_SH2_SOC_DMA1, KEGA_SH2_SOC_WDT,
    KEGA_SH2_SOC_SCI_ERROR, KEGA_SH2_SOC_SCI_RX, KEGA_SH2_SOC_SCI_TX,
    KEGA_SH2_SOC_SCI_END, KEGA_SH2_SOC_DIVU, KEGA_SH2_SOC_IRQ_CAUSES
};
/* Pointer-free snapshot. regs are the associated native SH2.peri_regs words;
 * serialize fields explicitly rather than writing structure padding. */
typedef struct KegaSh2SocState {
    uint32_t version;
    uint32_t regs[0x200 / 4];
    uint64_t now, frt_last, wdt_last, dma_due[2];
    uint32_t frt_phase, wdt_phase, dma_remaining[2];
    uint32_t irq_pending, irq_selected;
    uint16_t ocra, ocrb;
    uint8_t dma_active[2], dreq_tokens[2], wdt_key, rst_key, dma_turn;
} KegaSh2SocState;

KegaSh2Soc *kega_sh2_soc_create(KegaSh2 *, const KegaSh2SocHost *);
void kega_sh2_soc_destroy(KegaSh2Soc *);
void kega_sh2_soc_set_peer(KegaSh2Soc *, KegaSh2Soc *);
void kega_sh2_soc_reset(KegaSh2Soc *, uint64_t now);
/* Accept full peripheral addresses or offsets; multi-byte accesses align down.
 * MMIO updates timers and schedules DMA, but never executes DMA or another CPU. */
uint8_t kega_sh2_soc_read8(KegaSh2Soc *, uint32_t);
uint16_t kega_sh2_soc_read16(KegaSh2Soc *, uint32_t);
uint32_t kega_sh2_soc_read32(KegaSh2Soc *, uint32_t);
void kega_sh2_soc_write8(KegaSh2Soc *, uint32_t, uint8_t);
void kega_sh2_soc_write16(KegaSh2Soc *, uint32_t, uint16_t);
void kega_sh2_soc_write32(KegaSh2Soc *, uint32_t, uint32_t);
/* At most four DMA scalar units per call (one 16-byte burst counts as four),
 * only when due by target. Caller schedules again at next_deadline. */
void kega_sh2_soc_advance(KegaSh2Soc *, uint64_t target);
uint64_t kega_sh2_soc_next_deadline(const KegaSh2Soc *, uint64_t now);
uint64_t kega_sh2_soc_now(const KegaSh2Soc *);
void kega_sh2_soc_dreq(KegaSh2Soc *, unsigned channel);
/* Event-driven request: uses this event time, never an unrelated CPU lead. */
void kega_sh2_soc_dreq_at(KegaSh2Soc *, unsigned channel, uint64_t now);
/* External FRT capture pin; timestamp comes from the CPU/native scheduler. */
void kega_sh2_soc_capture(KegaSh2Soc *);
bool kega_sh2_soc_get_state(const KegaSh2Soc *, KegaSh2SocState *);
bool kega_sh2_soc_validate_state(const KegaSh2SocState *state);
bool kega_sh2_soc_set_state(KegaSh2Soc *, const KegaSh2SocState *);
#ifdef __cplusplus
}
#endif
#endif
