/*
 * PicoDrive PWM adaptation for Kega Refusion
 * (C) notaz, 2009,2010,2013
 * (C) irixxxx, 2019-2023
 *
 * Derived from pico/32x/pwm.c, revision
 * 07abb00550528f7bf53087d932dd3d747cf2d0df.
 * This work is licensed under the terms of the MAME noncommercial license.
 * See third_party/picodrive32x/COPYING.
 *
 * Explicit instances, GPX master-clock timestamps and amplitude deltas replace
 * donor globals/PCM storage. The programmed IRQ interval is never adapted.
 */
#include "pwm.h"
#include "../clock.h"
#include <string.h>

#ifdef KEGAREFUSION_32X
extern uint64_t kega_debug_audio_mask;
extern uint8_t kega_debug_audio_gain[38];
#endif
static unsigned output_gain(unsigned channel)
{
#ifdef KEGAREFUSION_32X
    return kega_debug_audio_gain[channel];
#else
    (void)channel;
    return 100;
#endif
}
static unsigned output_mask(void)
{
#ifdef KEGAREFUSION_32X
    return (unsigned)((kega_debug_audio_mask >> 34) & 3);
#else
    return 0;
#endif
}

static uint64_t master_clock(uint64_t sh2)
{
    if (sh2 > sh2_clock(UINT64_MAX)) return UINT64_MAX;
    return sh2 * 2 + sh2 / 3 + (sh2 % 3 != 0);
}
static unsigned period(const Kega32xPwmState *s) { return (s->cycle - 1u) & 0xfff; }
static unsigned reload(const Kega32xPwmState *s)
{
    unsigned n = (s->control >> 8) & 15;
    return n ? n : 16;
}
static void routed(const Kega32xPwmState *s, int32_t *left, int32_t *right)
{
    unsigned l = s->control & 3, r = (s->control >> 2) & 3;
    *left = l == 1 ? s->current[0] : l == 2 ? s->current[1] : 0;
    *right = r == 1 ? s->current[1] : r == 2 ? s->current[0] : 0;
}
static void publish(Kega32xPwm *p, uint64_t t)
{
    int32_t left, right, dl, dr;
    routed(&p->s, &left, &right);
    p->debug_mask = output_mask();
    p->debug_gain[0] = (uint8_t)output_gain(34);
    p->debug_gain[1] = (uint8_t)output_gain(35);
    if (p->debug_mask & 1) left = 0;
    else if (p->debug_gain[0] != 100) left = (left * p->debug_gain[0]) / 100;
    if (p->debug_mask & 2) right = 0;
    else if (p->debug_gain[1] != 100) right = (right * p->debug_gain[1]) / 100;
    dl = left - p->emitted[0]; dr = right - p->emitted[1];
    p->emitted[0] = left; p->emitted[1] = right;
    if ((dl || dr) && p->host.delta) p->host.delta(p->host.userdata, t, dl, dr);
}
static int32_t sample(uint16_t value, unsigned clocks)
{
    uint32_t mult = (UINT32_C(0x10000) << 8) / (clocks + 1);
    if (value > clocks) value = (uint16_t)clocks;
    return (int32_t)(((uint32_t)value * mult) >> 8) - 0x8000;
}
static void push(Kega32xPwmState *s, unsigned channel, uint16_t value)
{
    if (s->count[channel] == 3) {
        s->read[channel] = (uint8_t)((s->read[channel] + 1) % 3);
        --s->count[channel];
    }
    s->fifo[channel][(s->read[channel] + s->count[channel]) % 3] = (value - 1u) & 0xfff;
    ++s->count[channel];
}
static uint64_t raw_deadline(const Kega32xPwm *p)
{
    unsigned clocks = period(&p->s), steps;
    if (!clocks) return UINT64_MAX;
    if (p->s.count[0] || p->s.count[1]) steps = 1;
    else if (p->host.irq || ((p->s.control & KEGA32X_PWM_RTP) && p->host.dreq))
        steps = p->s.irq_count;
    else return UINT64_MAX;
    return master_clock(p->s.cursor_sh2 + (uint64_t)steps * clocks);
}
static void refresh_deadline(Kega32xPwm *p) { p->next_deadline = raw_deadline(p); }
void kega32x_pwm_advance(Kega32xPwm *p, uint64_t t)
{
    uint64_t target;
    unsigned changed = 0;
    if (t < p->s.now_mclk || p->advancing) return;
    target = sh2_clock(t);
    /* Release a held amplitude when the presentation mask changes. */
    if (p->debug_mask != output_mask() || p->debug_gain[0] != output_gain(34) ||
        p->debug_gain[1] != output_gain(35)) publish(p, p->s.now_mclk);
    p->advancing = 1;
    for (;;) {
        unsigned clocks = period(&p->s), first_irq, interval, fired;
        uint64_t due, steps, event;
        int observable_irq;
        if (!clocks) { p->s.cursor_sh2 = target; break; }
        uint64_t elapsed = target - p->s.cursor_sh2;
        if (elapsed < clocks) break;
        due = elapsed / clocks;
        first_irq = p->s.irq_count;
        interval = reload(&p->s);
        observable_irq = p->host.irq != 0 ||
                         ((p->s.control & KEGA32X_PWM_RTP) && p->host.dreq);
        if (p->s.count[0] || p->s.count[1]) steps = 1;
        else steps = observable_irq && due > first_irq ? first_irq : due;
        p->s.cursor_sh2 += steps * clocks;
        changed = 1;
        event = master_clock(p->s.cursor_sh2);
        /* Shortening CYCLE can make an old partial interval immediately due.
           Apply it at the register write, never publish a timestamp in the past. */
        if (event < p->s.now_mclk) event = p->s.now_mclk;
        p->s.now_mclk = event;
        if (steps == 1) {
            unsigned channel;
            for (channel = 0; channel < 2; ++channel) {
                if (p->s.count[channel]) {
                    p->s.current[channel] = sample(p->s.fifo[channel][p->s.read[channel]], clocks);
                    p->s.read[channel] = (uint8_t)((p->s.read[channel] + 1) % 3);
                    --p->s.count[channel];
                }
            }
        }
        fired = steps >= first_irq;
        p->s.irq_count = fired ? (uint8_t)(interval - (steps - first_irq) % interval)
                              : (uint8_t)(first_irq - steps);
        publish(p, event);
        if (fired && observable_irq) {
            int request = (p->s.control & KEGA32X_PWM_RTP) != 0;
            if (p->host.irq) p->host.irq(p->host.userdata, event);
            if (request && p->host.dreq) p->host.dreq(p->host.userdata, event);
        }
    }
    p->s.now_mclk = t;
    if (changed) refresh_deadline(p);
    p->advancing = 0;
}
uint64_t kega32x_pwm_next_deadline(const Kega32xPwm *p)
{
    /* Callbacks may enqueue samples while advance is processing an event. */
    uint64_t deadline = p->advancing ? raw_deadline(p) : p->next_deadline;
    return deadline < p->s.now_mclk ? p->s.now_mclk : deadline;
}
void kega32x_pwm_reset(Kega32xPwm *p, uint64_t t)
{
    if (p->advancing) return;
    memset(&p->s, 0, sizeof(p->s));
    p->s.now_mclk = t; p->s.cursor_sh2 = sh2_clock(t); p->s.irq_count = 16;
    refresh_deadline(p);
    publish(p, t);
}
void kega32x_pwm_power(Kega32xPwm *p, const Kega32xPwmHost *h, uint64_t t)
{
    memset(p, 0, sizeof(*p));
    if (h) p->host = *h;
    kega32x_pwm_reset(p, t);
}
uint16_t kega32x_pwm_read16(Kega32xPwm *p, unsigned reg, uint64_t t)
{
    unsigned channel;
    kega32x_pwm_advance(p, t);
    switch (reg & ~1u) {
    case 0x30: return p->s.control;
    case 0x32: return p->s.cycle;
    case 0x34: channel = 0; break;
    case 0x36: case 0x38: channel = 1; break;
    default: return 0;
    }
    return p->s.count[channel] == 3 ? KEGA32X_PWM_FULL :
           p->s.count[channel] == 0 ? KEGA32X_PWM_EMPTY : 0;
}
void kega32x_pwm_write16(Kega32xPwm *p, unsigned reg, uint16_t value, uint64_t t)
{
    /* Peer CPU instructions can finish just beyond a shared-bus deadline.
       Preserve the write at the device frontier rather than dropping it or
       replaying elapsed periods. The scheduler minimizes this approximation. */
    if (t < p->s.now_mclk) t = p->s.now_mclk;
    kega32x_pwm_advance(p, t);
    switch (reg & ~1u) {
    case 0x30:
        p->s.control = value & KEGA32X_PWM_CONTROL_MASK;
        p->s.irq_count = (uint8_t)reload(&p->s);
        refresh_deadline(p);
        publish(p, t);
        break;
    case 0x32:
        p->s.cycle = value & 0xfff;
        refresh_deadline(p);
        kega32x_pwm_advance(p, t);
        break;
    case 0x38: push(&p->s, 1, value); push(&p->s, 0, value); refresh_deadline(p); break;
    case 0x36: push(&p->s, 1, value); refresh_deadline(p); break;
    case 0x34: push(&p->s, 0, value); refresh_deadline(p); break;
    default: break;
    }
}
void kega32x_pwm_get_state(const Kega32xPwm *p, Kega32xPwmState *s) { *s = p->s; }
int kega32x_pwm_set_state(Kega32xPwm *p, const Kega32xPwmState *s)
{
    unsigned channel, i, clocks = period(s);
    uint64_t now = sh2_clock(s->now_mclk);
    if (p->advancing || (s->control & ~KEGA32X_PWM_CONTROL_MASK) || s->cycle > 0xfff ||
        !s->irq_count || s->irq_count > reload(s) || s->cursor_sh2 > now ||
        (clocks ? now - s->cursor_sh2 >= clocks : now != s->cursor_sh2)) return 0;
    for (channel = 0; channel < 2; ++channel) {
        if (s->read[channel] >= 3 || s->count[channel] > 3 ||
            s->current[channel] < -32768 || s->current[channel] > 32767) return 0;
        for (i = 0; i < 3; ++i) if (s->fifo[channel][i] > 0xfff) return 0;
    }
    p->s = *s;
    p->emitted[0] = p->emitted[1] = 0;
    refresh_deadline(p);
    publish(p, s->now_mclk);
    return 1;
}
