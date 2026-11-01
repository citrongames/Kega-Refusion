/* Internal 32X device assembly. Not a libretro core or a MD/CD replacement. */
#ifndef KEGA32X_MACHINE_H
#define KEGA32X_MACHINE_H
#include "bus.h"
#include "audio/pwm.h"
#include "sh2/sh2_interpreter.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Kega32xMachine Kega32xMachine;
typedef struct Kega32xMachineHost {
    void *userdata;
    /* Cartridge mapping stays owned by GPX. Addresses here are ROM-relative. */
    uint16_t (*rom_read16)(void *, uint32_t);
    /* CPU on-chip register callbacks supplied by the SH7604 SoC adapter. */
    uint32_t (*peripheral_read)(void *, unsigned cpu, uint32_t address, unsigned size);
    void (*peripheral_write)(void *, unsigned cpu, uint32_t address, uint32_t value, unsigned size);
    void (*reset_peripherals)(void *, unsigned cpu, uint64_t mclk);
    void (*dreq)(void *, unsigned channel, uint64_t mclk);
    void (*changed)(void *, unsigned source, unsigned flags, uint32_t address, uint64_t mclk);
    void (*render_sync)(void *, uint64_t mclk);
    void (*audio_delta)(void *, uint64_t mclk, int32_t left, int32_t right);
    /* Pure query for the complete ROM-relative instruction span. NULL forbids
       extra ROM peeks/idle batching; never read a mapper/MMIO register here. */
    bool (*rom_fetch_is_pure)(void *, uint32_t romOffset, size_t bytes);
} Kega32xMachineHost;
/* BIOS arrays are raw file order, exact sizes checked before allocation.
   ROM/BIOS files are loaded by the host; this module performs no file I/O. */
Kega32xMachine *kega32x_machine_create(const Kega32xMachineHost *,
    const uint8_t *master_bios, size_t master_size,
    const uint8_t *slave_bios, size_t slave_size, int pal, int no_cart);
void kega32x_machine_destroy(Kega32xMachine *);
/* Full restart retains allocated devices/BIOS and host bindings. */
void kega32x_machine_power_reset(Kega32xMachine *, int pal, int no_cart);
Kega32xBus *kega32x_machine_bus(Kega32xMachine *);
KegaSh2 *kega32x_machine_cpu(Kega32xMachine *, unsigned cpu);
Kega32xPwm *kega32x_machine_pwm(Kega32xMachine *);
uint64_t kega32x_machine_cpu_mclk(const Kega32xMachine *, unsigned cpu);
/* Runs only the two SH2s and 32X events; the outer GPX scheduler must interleave
   its own CPUs, CD and video events. Instruction overshoot is retained. */
void kega32x_machine_run_until(Kega32xMachine *, uint64_t master_clock);
uint64_t kega32x_machine_next_event(const Kega32xMachine *);
/* SH2 memory callbacks also usable by DMA; all words are numeric big-endian. */
uint32_t kega32x_machine_read(Kega32xMachine *, unsigned cpu, uint32_t address, unsigned size);
void kega32x_machine_write(Kega32xMachine *, unsigned cpu, uint32_t address, uint32_t value, unsigned size);
/* Caller-owned snapshot, defined in state.h; capture/restore only at an idle
   instruction boundary. No allocation and invalid restore has no side effects.
   Restore keeps callbacks/BIOS/ROM bindings; caller restores its outer timeline
   and clears/rebases audio before the held PWM amplitude is published. */
struct Kega32xMachineState;
int kega32x_machine_get_state(const Kega32xMachine *, struct Kega32xMachineState *);
int kega32x_machine_set_state(Kega32xMachine *, const struct Kega32xMachineState *);
#ifdef __cplusplus
}
#endif
#endif
