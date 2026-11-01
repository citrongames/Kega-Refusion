#ifdef KEGA_SH2_DRC
/* Kega Refusion callback bridge for PicoDrive's SH2 x86-64 DRC.
 * The fixed SR register must survive every ordinary host callback. */
#include "drc_internal.h"
DRC_DECLARE_SR;
static void callback_done(SH2 *s) {
    if (s->pending_level > (int)((s->sr >> 4) & 15))
        kega_sh2_end_run((KegaSh2 *)s);
}
#define READ_FN(bits, cast) \
u32 ALIGNED(2) REGPARM(2) p32x_sh2_read##bits(u32 a, SH2 *s) { \
    DRC_SAVE_SR(s); \
    u32 v = (u32)(cast)kega_sh2_host_read(s, a, bits); \
    callback_done(s); DRC_RESTORE_SR(s); return v; \
}
READ_FN(8, s8)
READ_FN(16, s16)
READ_FN(32, u32)
#define WRITE_FN(bits) \
void ALIGNED(2) REGPARM(3) p32x_sh2_write##bits(u32 a, u32 v, SH2 *s) { \
    DRC_SAVE_SR(s); \
    kega_sh2_host_write(s, a, v, bits); \
    callback_done(s); DRC_RESTORE_SR(s); \
}
WRITE_FN(8)
WRITE_FN(16)
WRITE_FN(32)
/* Poll loop elision is disabled in this first backend. Helpers remain valid
 * ordinary reads, so no stale value can bypass host synchronization. */
u32 p32x_sh2_poll_memory8(u32 a, u32 v, SH2 *s) { (void)v; return p32x_sh2_read8(a,s); }
u32 p32x_sh2_poll_memory16(u32 a, u32 v, SH2 *s) { (void)v; return p32x_sh2_read16(a,s); }
u32 p32x_sh2_poll_memory32(u32 a, u32 v, SH2 *s) { (void)v; return p32x_sh2_read32(a,s); }
/* IRQ acknowledges may re-publish a persistent peripheral source. */
static int (*original_ack[2])(SH2 *, int);
int kega_drc_ack(SH2 *s, int level) {
    DRC_SAVE_SR(s);
    int vector = original_ack[!!s->is_slave](s, level);
    DRC_RESTORE_SR(s);
    return vector;
}
void kega_drc_bind_ack(SH2 *s, bool bind) {
    unsigned i = !!s->is_slave;
    if (bind) { original_ack[i] = s->irq_callback; s->irq_callback = kega_drc_ack; }
    else if (s->irq_callback == kega_drc_ack) s->irq_callback = original_ack[i];
}

#endif /* KEGA_SH2_DRC */
