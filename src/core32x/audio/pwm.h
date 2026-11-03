/* Kega Refusion PWM, adapted from PicoDrive 07abb005 (see pwm.c). */
#ifndef KEGA32X_PWM_H
#define KEGA32X_PWM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { KEGA32X_PWM_CONTROL_MASK = 0x0f8f, KEGA32X_PWM_FULL = 0x8000,
       KEGA32X_PWM_EMPTY = 0x4000, KEGA32X_PWM_RTP = 0x0080 };
/* Pointer-free hardware state. Serialize fields, not C structure padding.
   Times are absolute GPX master clocks; cursor_sh2 uses exact floor(3*t/7). */
typedef struct Kega32xPwmState {
    uint64_t now_mclk, cursor_sh2;
    uint16_t control, cycle;
    uint16_t fifo[2][3];
    uint8_t read[2], count[2], irq_count;
    int32_t current[2];
} Kega32xPwmState;
typedef struct Kega32xPwmHost {
    void *userdata;
    void (*delta)(void *, uint64_t mclk, int32_t left, int32_t right);
    void (*irq)(void *, uint64_t mclk);
    void (*dreq)(void *, uint64_t mclk);
} Kega32xPwmHost;
typedef struct Kega32xPwm {
    Kega32xPwmState s;
    Kega32xPwmHost host;
    int32_t emitted[2];
    unsigned debug_mask; /* presentation only; never serialized */
    uint8_t debug_gain[2];
    unsigned advancing;
} Kega32xPwm;
/* Callbacks may enqueue FIFO data at their event timestamp; they must not run
   a CPU, advance to a future time, reset or restore this device recursively. */
void kega32x_pwm_power(Kega32xPwm *, const Kega32xPwmHost *, uint64_t mclk);
void kega32x_pwm_reset(Kega32xPwm *, uint64_t mclk);
/* Offsets are the bus's system-register offsets 0x30..0x38, numeric words.
   Bus owner applies CPU-specific write masks before calling this API.
   A late peer write is applied at max(mclk,now_mclk), without rewinding time;
   this is a bounded instruction-boundary approximation, not a dropped write. */
uint16_t kega32x_pwm_read16(Kega32xPwm *, unsigned reg, uint64_t mclk);
void kega32x_pwm_write16(Kega32xPwm *, unsigned reg, uint16_t value, uint64_t mclk);
void kega32x_pwm_advance(Kega32xPwm *, uint64_t mclk);
/* UINT64_MAX if no externally observable event is scheduled. */
uint64_t kega32x_pwm_next_deadline(const Kega32xPwm *);
void kega32x_pwm_get_state(const Kega32xPwm *, Kega32xPwmState *);
/* Rejects invalid state without mutation/callbacks. On success emits the held
   amplitude from zero: caller clears/rebases the destination blip first. */
int kega32x_pwm_set_state(Kega32xPwm *, const Kega32xPwmState *);
#ifdef __cplusplus
}
#endif
#endif
