#include "audio/pwm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "PWM check failed at %d: %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct Recorder {
    unsigned deltas, irqs, requests, feed;
    uint64_t times[128], irq_times[128];
    int32_t left, right;
    Kega32xPwm *pwm;
} Recorder;
static void delta(void *user, uint64_t t, int32_t left, int32_t right)
{
    Recorder *r = user;
    CHECK(r->deltas < 128);
    r->times[r->deltas++] = t;
    r->left += left; r->right += right;
}
static void irq(void *user, uint64_t t)
{
    Recorder *r = user;
    CHECK(r->irqs < 128); r->irq_times[r->irqs++] = t;
}
static void request(void *user, uint64_t t)
{
    Recorder *r = user;
    ++r->requests;
    if (r->feed) {
        kega32x_pwm_write16(r->pwm, 0x38, (uint16_t)(r->feed % 2 + 1), t);
        --r->feed;
    }
}
static Kega32xPwmHost host(Recorder *r)
{
    Kega32xPwmHost h = {r, delta, irq, request};
    return h;
}
static int32_t amplitude(unsigned raw, unsigned clocks)
{
    unsigned mult = (0x10000u << 8) / (clocks + 1);
    if (raw > clocks) raw = clocks;
    return (int32_t)((raw * mult) >> 8) - 32768;
}
static void fifo_and_routes(void)
{
    Kega32xPwm p;
    Recorder r = {0};
    Kega32xPwmHost h = host(&r);
    Kega32xPwmState state;
    unsigned i;
    kega32x_pwm_power(&p, &h, 0);
    kega32x_pwm_write16(&p, 0x32, 11, 0); /* period = ten SH2 clocks */
    kega32x_pwm_write16(&p, 0x30, 0x305, 0);
    CHECK(kega32x_pwm_read16(&p, 0x34, 0) == KEGA32X_PWM_EMPTY);
    for (i = 2; i <= 5; ++i) kega32x_pwm_write16(&p, 0x34, (uint16_t)i, 0);
    CHECK(kega32x_pwm_read16(&p, 0x34, 0) == KEGA32X_PWM_FULL);
    CHECK(kega32x_pwm_read16(&p, 0x38, 0) == KEGA32X_PWM_EMPTY);
    CHECK(kega32x_pwm_next_deadline(&p) == 24);
    kega32x_pwm_advance(&p, 24);
    CHECK(r.left == amplitude(2, 10) && r.deltas == 1 && r.times[0] == 24);
    CHECK(kega32x_pwm_next_deadline(&p) == 47);
    kega32x_pwm_advance(&p, 70);
    CHECK(r.left == amplitude(4, 10) && r.deltas == 3);
    CHECK(r.times[1] == 47 && r.times[2] == 70);
    CHECK(r.irqs == 1 && r.irq_times[0] == 70); /* full FIFO never adapts IRQ rate */
    kega32x_pwm_get_state(&p, &state);
    CHECK(state.count[0] == 0 && state.irq_count == 3);
    kega32x_pwm_write16(&p, 0x36, 8, 70);
    kega32x_pwm_advance(&p, 94);
    CHECK(r.left == amplitude(4, 10) && r.right == amplitude(7, 10));
    kega32x_pwm_write16(&p, 0x30, 0x30a, 94); /* swapped */
    CHECK(r.left == amplitude(7, 10) && r.right == amplitude(4, 10));
    kega32x_pwm_write16(&p, 0x30, 0x309, 94); /* left source, both outputs */
    CHECK(r.left == amplitude(4, 10) && r.right == r.left);
    kega32x_pwm_write16(&p, 0x30, 0x306, 94); /* right source, both outputs */
    CHECK(r.left == amplitude(7, 10) && r.right == r.left);
    kega32x_pwm_write16(&p, 0x30, 0x30f, 94);
    CHECK(r.left == 0 && r.right == 0);
    kega32x_pwm_write16(&p, 0x38, 4, 94);
    kega32x_pwm_advance(&p, 117);
    kega32x_pwm_write16(&p, 0x30, 0x301, 117); /* left output only */
    CHECK(r.left == amplitude(3, 10) && r.right == 0);
}
static void exact_clock_and_silence(void)
{
    Kega32xPwm p;
    Recorder r = {0};
    Kega32xPwmHost h = host(&r);
    unsigned n, before;
    kega32x_pwm_power(&p, &h, 0);
    kega32x_pwm_write16(&p, 0x32, 2, 0); /* one SH2 tick, 7/3 master clocks */
    kega32x_pwm_write16(&p, 0x30, 0x185, 0); /* IRQ each tick, DREQ, normal */
    kega32x_pwm_write16(&p, 0x38, 1, 0);
    for (n = 1; n <= 42; ++n) {
        uint64_t expected = ((uint64_t)n * 7 + 2) / 3;
        CHECK(kega32x_pwm_next_deadline(&p) == expected);
        kega32x_pwm_advance(&p, expected);
        CHECK(r.irqs == n && r.requests == n && r.irq_times[n - 1] == expected);
    }
    CHECK(r.deltas == 1 && r.times[0] == 3); /* held amplitude has no repeated callbacks */
    kega32x_pwm_write16(&p, 0x30, 0x180, 98); /* silent route, still consumes and requests */
    before = r.deltas;
    kega32x_pwm_write16(&p, 0x38, 1, 98);
    kega32x_pwm_advance(&p, 110);
    CHECK(r.deltas == before && r.irqs == 47 && r.requests == 47);
    CHECK(p.s.count[0] == 0 && p.s.count[1] == 0);
    kega32x_pwm_write16(&p, 0x32, 1, 110); /* zero effective period must not loop */
    CHECK(kega32x_pwm_next_deadline(&p) == UINT64_MAX);
    kega32x_pwm_advance(&p, UINT64_C(1000000000000));
    CHECK(r.irqs == 47 && r.requests == 47);
    kega32x_pwm_write16(&p, 0x32, 0, UINT64_C(1000000000000));
    CHECK(kega32x_pwm_next_deadline(&p) > UINT64_C(1000000000000)); /* zero register means period4095 */
    /* No sink/IRQ callbacks: skip enormous silent spans arithmetically. */
    kega32x_pwm_power(&p, NULL, 0);
    kega32x_pwm_write16(&p, 0x32, 2, 0);
    CHECK(kega32x_pwm_next_deadline(&p) == UINT64_MAX);
    kega32x_pwm_advance(&p, UINT64_MAX - 2);
    CHECK(p.s.now_mclk == UINT64_MAX - 2 && p.s.irq_count >= 1 && p.s.irq_count <= 16);
}
static void late_peer_write(void)
{
    Kega32xPwm p;
    Recorder r = {0};
    Kega32xPwmHost h = host(&r);
    kega32x_pwm_power(&p, &h, 0);
    kega32x_pwm_write16(&p, 0x32, 11, 0);
    kega32x_pwm_write16(&p, 0x30, 5, 0);
    kega32x_pwm_advance(&p, 10);
    kega32x_pwm_write16(&p, 0x38, 5, 9); /* Other SH2's last instruction was slightly behind. */
    CHECK(p.s.now_mclk == 10 && p.s.count[0] == 1 && p.s.count[1] == 1);
    kega32x_pwm_advance(&p, 24);
    CHECK(r.deltas == 1 && r.times[0] == 24 && r.left == amplitude(4, 10));
    kega32x_pwm_write16(&p, 0x30, 0, 23);
    CHECK(p.s.control == 0 && p.s.now_mclk == 24 && r.left == 0 && r.right == 0);
    CHECK(r.deltas == 2 && r.times[1] == 24);
}

static void restore_and_reentrant_fifo(void)
{
    Kega32xPwm p, restored;
    Recorder r = {0}, loaded = {0};
    Kega32xPwmHost h = host(&r), h2 = host(&loaded);
    Kega32xPwmState good, bad, before, after;
    kega32x_pwm_power(&p, &h, 0);
    r.pwm = &p; r.feed = 3;
    kega32x_pwm_write16(&p, 0x32, 2, 0);
    kega32x_pwm_write16(&p, 0x30, 0x185, 0);
    kega32x_pwm_write16(&p, 0x38, 1, 0);
    kega32x_pwm_advance(&p, 7);
    CHECK(r.requests == 3 && p.s.count[0] == 1 && p.s.count[1] == 1 && !p.advancing);
    kega32x_pwm_get_state(&p, &good);
    before = good;
    bad = good; bad.count[1] = 4;
    CHECK(!kega32x_pwm_set_state(&p, &bad));
    bad = good; bad.read[0] = 3;
    CHECK(!kega32x_pwm_set_state(&p, &bad));
    bad = good; bad.control |= 0x10;
    CHECK(!kega32x_pwm_set_state(&p, &bad));
    bad = good; bad.cycle = 0x1000;
    CHECK(!kega32x_pwm_set_state(&p, &bad));
    bad = good; bad.cursor_sh2 = UINT64_MAX;
    CHECK(!kega32x_pwm_set_state(&p, &bad));
    bad = good; bad.irq_count = 0;
    CHECK(!kega32x_pwm_set_state(&p, &bad));
    bad = good; --bad.cursor_sh2; /* Unprocessed past events cannot be restored as current state. */
    CHECK(!kega32x_pwm_set_state(&p, &bad));
    bad = good; bad.fifo[0][0] = 0x1000;
    CHECK(!kega32x_pwm_set_state(&p, &bad));
    kega32x_pwm_get_state(&p, &after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    kega32x_pwm_power(&restored, &h2, 0);
    CHECK(kega32x_pwm_set_state(&restored, &good));
    CHECK(loaded.left == r.left && loaded.right == r.right);
    CHECK(kega32x_pwm_next_deadline(&restored) == kega32x_pwm_next_deadline(&p));
    kega32x_pwm_advance(&restored, 14);
    kega32x_pwm_advance(&p, 14);
    kega32x_pwm_get_state(&p, &before);
    kega32x_pwm_get_state(&restored, &after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    kega32x_pwm_write16(&p, 0x38, 1, 14);
    kega32x_pwm_advance(&p, 17);
    CHECK(r.left == -32768 && r.right == -32768);
    kega32x_pwm_reset(&p, 18);
    CHECK(r.left == 0 && r.right == 0);
}
#ifdef KEGAREFUSION_32X
/* Standalone integration build supplies the same output-only mask as libretro. */
uint64_t kega_debug_audio_mask;
uint8_t kega_debug_audio_gain[38];
static void debug_output_mute(void)
{
    Kega32xPwm p;
    Recorder r = {0};
    Kega32xPwmHost h = host(&r);
    Kega32xPwmState before, after;
    int32_t left, right;
    kega32x_pwm_power(&p, &h, 0);
    kega32x_pwm_write16(&p, 0x32, 11, 0);
    kega32x_pwm_write16(&p, 0x30, 0x305, 0);
    kega32x_pwm_write16(&p, 0x34, 4, 0);
    kega32x_pwm_write16(&p, 0x36, 8, 0);
    kega32x_pwm_advance(&p, 24);
    left = r.left; right = r.right;
    CHECK(left && right);
    kega32x_pwm_get_state(&p, &before);
    kega_debug_audio_mask = UINT64_C(1) << 34;
    kega32x_pwm_advance(&p, 24);
    CHECK(r.left == 0 && r.right == right);
    kega32x_pwm_get_state(&p, &after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    kega_debug_audio_mask |= UINT64_C(1) << 35;
    kega32x_pwm_advance(&p, 24);
    CHECK(r.left == 0 && r.right == 0);
    kega_debug_audio_mask = 0;
    kega32x_pwm_advance(&p, 24);
    CHECK(r.left == left && r.right == right);
    kega_debug_audio_gain[34] = 50;
    kega32x_pwm_advance(&p, 24);
    CHECK(r.left == (left * 50) / 100 && r.right == right);
    kega_debug_audio_gain[34] = 100;
    kega32x_pwm_advance(&p, 24);
    CHECK(r.left == left && r.right == right);
    kega32x_pwm_get_state(&p, &after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
}
#endif

int main(void)
{
#ifdef KEGAREFUSION_32X
    for (unsigned i = 0; i < 38; ++i) kega_debug_audio_gain[i] = 100;
#endif
    fifo_and_routes(); exact_clock_and_silence(); late_peer_write(); restore_and_reentrant_fifo();
#ifdef KEGAREFUSION_32X
    debug_output_mute();
#endif
    puts("32X PWM tests passed: FIFO, routes, exact clocks, silence, IRQ/DREQ, state");
    return 0;
}
