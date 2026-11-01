#include "bus.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned irq_level[2], request_count, change_flags;
static void irq(void *p,unsigned cpu,unsigned level,uint64_t t) { (void)p;(void)t;irq_level[cpu]=level; }
static void request(void *p,uint64_t t) { (void)p;(void)t;request_count++; }
static void change(void *p,unsigned c,unsigned f,uint32_t a,uint64_t t) { (void)p;(void)c;(void)a;(void)t;change_flags|=f; }
int main(void)
{
    Kega32xBus b;
    Kega32xMemory *m=calloc(1,sizeof(*m));
    Kega32xBusHost h={0};
    unsigned i;
    assert(m); h.irq=irq; h.dreq=request; h.changed=change;
    kega32x_bus_power(&b,m,&h,0,1);
    assert(kega32x_bus_read16(&b,2,0,0)==0x82);
    assert(kega32x_bus_read16(&b,0,0,0)==0x100);
    kega32x_bus_write16(&b,2,0,3,1);
    assert(change_flags&KEGA32X_RESET_RELEASED);
    assert(kega32x_bus_read16(&b,0,0,1)==0x300);
    /* Independent SH2 masks and command acknowledgement; reset stays latched. */
    kega32x_bus_write8(&b,0,1,2,2);
    kega32x_bus_write16(&b,2,2,3,3);
    assert(irq_level[0]==8 && irq_level[1]==0);
    kega32x_bus_irq(&b,KEGA32X_IRQ_RESET,4);
    assert(irq_level[0]==14 && irq_level[1]==14);
    kega32x_bus_write16(&b,0,0x14,0,5);
    assert(irq_level[0]==8 && irq_level[1]==14);
    kega32x_bus_write16(&b,0,0x1a,0,6);
    assert(irq_level[0]==0 && (b.s.regs[1]&2));
    /* An even-byte H-count access cannot corrupt its low byte. */
    kega32x_bus_write8(&b,0,5,19,7);
    kega32x_bus_write8(&b,0,4,255,7);
    assert(kega32x_bus_read16(&b,0,4,7)==19);
    /* FIFO overflow preserves queued data, and only accepted words count. */
    kega32x_bus_write16(&b,2,0x10,12,10);
    kega32x_bus_write16(&b,2,6,4,10);
    for (i=0;i<8;i++) kega32x_bus_write16(&b,2,0x12,(uint16_t)(0x100+i),11+i);
    kega32x_bus_write16(&b,2,0x12,0xdead,20);
    assert(request_count==2 && b.s.regs[8]==4 && kega32x_dreq_available(&b)==8);
    assert(kega32x_bus_read16(&b,2,6,20)&0x80);
    assert((kega32x_bus_read16(&b,0,6,20)&0xc000)==0x4000);
    assert(kega32x_dreq_pop(&b)==0x100);
    assert(kega32x_bus_read16(&b,2,6,20)&0x80);
    kega32x_bus_write16(&b,2,0x12,0xdead,20);
    assert(b.s.regs[8]==4 && request_count==2);
    for (i=1;i<4;i++) assert(kega32x_dreq_pop(&b)==0x100+i);
    assert(!(kega32x_bus_read16(&b,2,6,21)&0x80));
    for (i=0;i<4;i++) kega32x_bus_write16(&b,2,0x12,(uint16_t)(0x108+i),22+i);
    assert(!(b.s.regs[3]&4) && b.s.regs[8]==0);
    for (i=0;i<8;i++) assert(kega32x_dreq_pop(&b)==0x104+i);
    assert(kega32x_bus_read16(&b,0,6,30)&0x4000);
    /* Writing COMM bytes must not lose the other half or affect the peer mask. */
    kega32x_bus_write16(&b,0,0x20,0x1234,31);
    kega32x_bus_write8(&b,2,0x21,0x56,32);
    assert(kega32x_bus_read16(&b,1,0x20,33)==0x1256);
    /* FM protects video writes. Palette byte lanes are big-endian numeric. */
    kega32x_bus_write16(&b,0,0x200,0xffff,34);
    assert(b.s.palette[0]==0);
    kega32x_bus_write16(&b,2,0x200,0x1234,35);
    kega32x_bus_write8(&b,2,0x201,0x56,35);
    assert(b.s.palette[0]==0x1256);
    /* Fill wraps within a 256-word row; framebuffer swap waits for vblank. */
    kega32x_bus_write16(&b,2,0x100,1,40);
    kega32x_bus_blank(&b,0,0,41);
    assert(!(b.s.vdp[5]&0x2000));
    kega32x_bus_blank(&b,0,1,41);
    assert(b.s.vdp[5]&0x2000);
    kega32x_bus_blank(&b,0,0,41);
    kega32x_bus_write16(&b,2,0x100,2,41);
    assert(b.s.vdp[5]&0x2000);
    kega32x_bus_write16(&b,2,0x100,1,41);
    kega32x_bus_write16(&b,2,0x104,3,42);
    kega32x_bus_write16(&b,2,0x106,0x1fe,42);
    kega32x_bus_write16(&b,2,0x108,0xbeef,42);
    assert(m->framebuffer[1][0x1fe]==0xbeef && m->framebuffer[1][0x100]==0xbeef);
    assert(m->framebuffer[1][0x200]==0 && b.s.vdp[3]==0x102);
    assert(b.s.vdp[5]&2);
    kega32x_bus_advance(&b,91);
    assert(!(b.s.vdp[5]&2));
    kega32x_bus_write16(&b,2,0x10a,1,100);
    assert(!(b.s.vdp[5]&1));
    kega32x_bus_blank(&b,1,0,101);
    assert(b.s.vdp[5]&1);
    /* Normal words can clear pixels; overwrite words and all byte writes skip zero. */
    kega32x_fb_write16(&b,0,0x1234);
    kega32x_fb_write16(&b,0x20000,0x0056);
    kega32x_fb_write8(&b,0,0);
    assert(kega32x_fb_read16(&b,0)==0x1256);
    kega32x_fb_write16(&b,0,0);
    assert(kega32x_fb_read16(&b,0)==0);
    free(m); puts("32X bus: FIFO, IRQ, byte lanes, video ownership, fill and swap passed");
    return 0;
}
