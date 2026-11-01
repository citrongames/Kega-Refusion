/* Kega Refusion 32X bus, derived from PicoDrive 07abb005 (see bus.c). */
#ifndef KEGA32X_BUS_H
#define KEGA32X_BUS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { KEGA32X_MASTER = 0, KEGA32X_SLAVE = 1, KEGA32X_M68K = 2 };
enum { KEGA32X_IRQ_PWM = 8, KEGA32X_IRQ_CMD = 16,
       KEGA32X_IRQ_HINT = 32, KEGA32X_IRQ_VINT = 64, KEGA32X_IRQ_RESET = 128 };
enum { KEGA32X_MAP_CHANGED = 1, KEGA32X_RESET_ASSERTED = 2,
       KEGA32X_RESET_RELEASED = 4, KEGA32X_ENABLE_CHANGED = 8,
       KEGA32X_COMM_CHANGED = 16, KEGA32X_HINT_CHANGED = 32 };
/* Pointer-free device state. Serialize fields explicitly, never the C padding. */
typedef struct Kega32xBusState {
    uint16_t regs[32];
    uint16_t sh2_control;
    uint16_t vdp[8];
    uint16_t palette[256];
    uint16_t dreq_fifo[8];
    uint8_t irq_mask[2], irq_pending[2];
    uint8_t fifo_read, fifo_write, fifo_ready, fifo_count, pending_fb, hcount;
    uint64_t fill_end_mclk;
} Kega32xBusState;
/* Allocated only when attached. No duplicate ROM or MD/CD RAM. */
typedef struct Kega32xMemory {
    uint16_t framebuffer[2][65536];
    uint16_t sdram[131072];
} Kega32xMemory;
typedef struct Kega32xBusHost {
    void *userdata;
    /* Caller synchronizes CPUs BEFORE shared bus accesses. Callbacks below may
       wake/end a CPU timeslice; they must not run a CPU recursively. */
    void (*changed)(void *, unsigned source, unsigned flags, uint32_t address, uint64_t mclk);
    void (*irq)(void *, unsigned cpu, unsigned level, uint64_t mclk);
    void (*dreq)(void *, uint64_t mclk);
    void (*render_sync)(void *, uint64_t mclk);
    uint16_t (*pwm_read)(void *, unsigned reg, uint64_t mclk);
    void (*pwm_write)(void *, unsigned reg, uint16_t value, uint64_t mclk);
} Kega32xBusHost;
typedef struct Kega32xBus {
    Kega32xBusState s;
    Kega32xMemory *memory;
    Kega32xBusHost host;
} Kega32xBus;
void kega32x_bus_power(Kega32xBus *, Kega32xMemory *, const Kega32xBusHost *, int pal, int no_cart);
/* Offsets: 0..0x3f system, 0x100..0x10f VDP, 0x200..0x3ff palette.
   Values are numeric words, independent of host byte order. */
uint16_t kega32x_bus_read16(Kega32xBus *, unsigned source, uint32_t offset, uint64_t mclk);
uint8_t kega32x_bus_read8(Kega32xBus *, unsigned source, uint32_t offset, uint64_t mclk);
void kega32x_bus_write16(Kega32xBus *, unsigned source, uint32_t offset, uint16_t value, uint64_t mclk);
void kega32x_bus_write8(Kega32xBus *, unsigned source, uint32_t offset, uint8_t value, uint64_t mclk);
void kega32x_bus_irq(Kega32xBus *, unsigned mask, uint64_t mclk);
void kega32x_bus_blank(Kega32xBus *, int vblank, int hblank, uint64_t mclk);
void kega32x_bus_advance(Kega32xBus *, uint64_t mclk);
/* Recompute derived IRQ lines after loading state. */
void kega32x_bus_restore(Kega32xBus *, uint64_t mclk);
unsigned kega32x_dreq_available(const Kega32xBus *);
uint16_t kega32x_dreq_pop(Kega32xBus *);
uint16_t kega32x_fb_read16(const Kega32xBus *, uint32_t byte_address);
void kega32x_fb_write16(Kega32xBus *, uint32_t byte_address, uint16_t value);
void kega32x_fb_write8(Kega32xBus *, uint32_t byte_address, uint8_t value);
#ifdef __cplusplus
}
#endif
#endif
