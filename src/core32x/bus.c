/*
 * Kega Refusion 32X device bus.
 * Derived from PicoDrive pico/32x/memory.c and 32x.c, revision
 * 07abb00550528f7bf53087d932dd3d747cf2d0df.
 * (C) notaz, 2009,2010,2013; (C) irixxxx, 2019-2024.
 * MAME noncommercial terms: third_party/picodrive32x/COPYING.
 * Modified for Kega Refusion: explicit state/host API, master-clock timestamps,
 * bounded two-bank DREQ FIFO, real blanking signals instead of read-count
 * toggles, no donor MD/CD execution or global Pico state.
 */
#include "bus.h"
#include <string.h>

enum { FM=0x8000, REN=0x80, NRES=2, ADEN=1, FULL=0x80, S68=4,
       VBLANK=0x8000, HBLANK=0x4000, PEN=0x2000, FEN=2 };
static void changed(Kega32xBus *b, unsigned cpu, unsigned flags, unsigned addr, uint64_t t)
{
    if (b->host.changed) b->host.changed(b->host.userdata,cpu,flags,addr,t);
}
static void render_sync(Kega32xBus *b, uint64_t t)
{
    if (b->host.render_sync) b->host.render_sync(b->host.userdata,t);
}
static void irqs(Kega32xBus *b, uint64_t t)
{
    unsigned cpu;
    for (cpu=0;cpu<2;cpu++) {
        unsigned p=b->s.irq_pending[cpu], level=0;
        if ((b->s.regs[0] & (NRES|ADEN)) == (NRES|ADEN))
            while (p >>= 1) level+=2;
        if (b->host.irq) b->host.irq(b->host.userdata,cpu,level,t);
    }
}
static void command_irq(Kega32xBus *b, uint64_t t)
{
    unsigned cpu;
    for (cpu=0;cpu<2;cpu++) {
        b->s.irq_pending[cpu] &= (uint8_t)~KEGA32X_IRQ_CMD;
        if ((b->s.irq_mask[cpu]&2) && (b->s.regs[1]&(1u<<cpu)))
            b->s.irq_pending[cpu] |= KEGA32X_IRQ_CMD;
    }
    irqs(b,t);
}
void kega32x_bus_irq(Kega32xBus *b, unsigned mask, uint64_t t)
{
    unsigned cpu;
    for (cpu=0;cpu<2;cpu++)
        b->s.irq_pending[cpu] |= (uint8_t)(mask & ((b->s.irq_mask[cpu]<<3)|KEGA32X_IRQ_RESET));
    irqs(b,t);
}
void kega32x_bus_power(Kega32xBus *b, Kega32xMemory *m, const Kega32xBusHost *h, int pal, int no_cart)
{
    memset(b,0,sizeof(*b));
    b->memory=m;
    if (h) b->host=*h;
    b->dram_refresh_start=UINT64_MAX;
    b->s.regs[0]=REN|NRES;
    b->s.regs[8]=0xffff;
    b->s.sh2_control=(uint16_t)(no_cart ? 0x100:0);
    b->s.vdp[0]=(uint16_t)(pal ? 0:0x8000);
    b->s.vdp[5]=VBLANK|PEN;
    if (m) memset(m,0,sizeof(*m));
}
void kega32x_bus_advance(Kega32xBus *b, uint64_t t)
{
    if ((b->s.vdp[5]&FEN) && t>=b->s.fill_end_mclk) {
        b->s.vdp[5] &= (uint16_t)~FEN;
        b->s.fill_end_mclk=0;
        changed(b,KEGA32X_M68K,KEGA32X_COMM_CHANGED,0x10a,t);
    }
}
void kega32x_bus_blank(Kega32xBus *b, int vb, int hb, uint64_t t)
{
    unsigned old=b->s.vdp[5];
    kega32x_bus_advance(b,t);
    b->s.vdp[5] &= (uint16_t)~(VBLANK|HBLANK|PEN);
    if (vb) b->s.vdp[5]|=VBLANK;
    if (hb) b->s.vdp[5]|=HBLANK;
    if (vb || hb || !(b->s.vdp[0]&1)) b->s.vdp[5]|=PEN;
    if (vb && !(old&VBLANK)) {
        b->s.vdp[5]=(uint16_t)((b->s.vdp[5]&~1)|b->s.pending_fb);
        kega32x_bus_irq(b,KEGA32X_IRQ_VINT,t);
    }
    if (old!=b->s.vdp[5]) changed(b,KEGA32X_M68K,KEGA32X_COMM_CHANGED|(((old^b->s.vdp[5])&1)?KEGA32X_MAP_CHANGED:0),0x10a,t);
}
void kega32x_bus_restore(Kega32xBus *b, uint64_t t)
{
    /* Caller validates serialized ranges before installing state. */
    b->video_watch_banks=0;
    command_irq(b,t);
    changed(b,KEGA32X_M68K,KEGA32X_MAP_CHANGED,0,t);
}
static void fifo_full(Kega32xBus *b)
{
    b->s.regs[3]&=(uint16_t)~FULL;
    if (b->s.fifo_ready & (1u<<(b->s.fifo_write>>2))) b->s.regs[3]|=FULL;
}
unsigned kega32x_dreq_available(const Kega32xBus *b)
{
    unsigned bank=b->s.fifo_read>>2;
    if (!(b->s.fifo_ready&(1u<<bank))) return 0;
    return 4u-(b->s.fifo_read&3u)+((b->s.fifo_ready&(1u<<(bank^1)))?4u:0u);
}
uint16_t kega32x_dreq_pop(Kega32xBus *b)
{
    uint16_t v=b->s.regs[9];
    unsigned bank=b->s.fifo_read>>2;
    if (b->s.fifo_ready&(1u<<bank)) {
        v=b->s.dreq_fifo[b->s.fifo_read];
        b->s.fifo_read=(uint8_t)((b->s.fifo_read+1)&7);
        b->s.fifo_count--;
        if (!(b->s.fifo_read&3)) b->s.fifo_ready&=(uint8_t)~(1u<<bank);
        fifo_full(b);
        b->s.regs[9]=v;
    }
    return v;
}
static void dreq_push(Kega32xBus *b, uint16_t v, uint64_t t)
{
    unsigned bank=b->s.fifo_write>>2;
    if (!(b->s.regs[0]&ADEN) || !(b->s.regs[3]&S68) || (b->s.fifo_ready&(1u<<bank))) return;
    b->s.dreq_fifo[b->s.fifo_write]=v;
    b->s.fifo_write=(uint8_t)((b->s.fifo_write+1)&7);
    b->s.fifo_count++;
    if (--b->s.regs[8]==0) b->s.regs[3]&=(uint16_t)~S68;
    /* Two four-word banks. A partially read bank is not writable yet, and
       partially written data is not visible to the SH2/DMA side. */
    if (!(b->s.fifo_write&3)) {
        b->s.fifo_ready|=(uint8_t)(1u<<bank);
        fifo_full(b);
        if (b->host.dreq) b->host.dreq(b->host.userdata,t);
    }
}
static int owns_video(const Kega32xBus *b, unsigned cpu)
{
    return !!(b->s.regs[0]&FM) == (cpu!=KEGA32X_M68K);
}
static uint16_t sys_read(Kega32xBus *b, unsigned cpu, unsigned a, uint64_t t)
{
    if (a>=0x30) return b->host.pwm_read ? b->host.pwm_read(b->host.userdata,a,t):0;
    if (cpu==KEGA32X_M68K) return b->s.regs[a/2];
    switch (a) {
    case 0: return (uint16_t)((b->s.regs[0]&FM)|b->s.sh2_control|b->s.irq_mask[cpu]);
    case 4: return b->s.hcount;
    /* SH2 FULL/EMPTY describe the framebuffer write cache, NOT DREQ.
       Writes are committed synchronously here, so its cache is empty. */
    case 6: return (uint16_t)((b->s.regs[3]&7)|0x4000);
    case 0x12: return kega32x_dreq_pop(b);
    case 2: case 0x14: case 0x16: case 0x18: case 0x1a: case 0x1c: case 0x1e: return 0;
    default: return b->s.regs[a/2];
    }
}
uint16_t kega32x_bus_read16(Kega32xBus *b, unsigned cpu, uint32_t a, uint64_t t)
{
    if (cpu>KEGA32X_M68K) return 0;
    a &= ~1u;
    if (!(b->s.regs[0]&ADEN) && a>=0x30) return 0;
    if (a<0x40) return sys_read(b,cpu,a,t);
    if (a>=0x100 && a<0x110) {
        kega32x_bus_advance(b,t);
        uint16_t value=b->s.vdp[(a&15)/2];
        /* FEN is busy both during an autofill and while the framebuffer DRAM
           refreshes (~40 SH-2 clocks per line). Only sample the latter on a
           control-register read; it has no DMA completion event of its own. */
        if (a==0x10a && t>=b->dram_refresh_start &&
            t-b->dram_refresh_start < 40u*7/3)
            value|=FEN;
        return value;
    }
    if (a>=0x200 && a<0x400) return b->s.palette[(a&511)/2];
    return 0;
}
uint8_t kega32x_bus_read8(Kega32xBus *b, unsigned cpu, uint32_t a, uint64_t t)
{
    uint16_t v=kega32x_bus_read16(b,cpu,a,t);
    return (uint8_t)(v>>((a&1)?0:8));
}
static void adapter_write(Kega32xBus *b, unsigned cpu, uint16_t v, uint64_t t)
{
    unsigned old=b->s.regs[0], flags=0;
    if (cpu==KEGA32X_M68K && ((old^v)&ADEN)) render_sync(b,t);
    if (cpu==KEGA32X_M68K) {
        if ((old&ADEN) && !(v&ADEN)) v|=NRES;
        b->s.regs[0]=(uint16_t)((old&REN)|(v&(FM|NRES|ADEN)));
        if ((old^v)&ADEN) {
            flags|=KEGA32X_ENABLE_CHANGED|KEGA32X_MAP_CHANGED;
            b->s.sh2_control=(uint16_t)((b->s.sh2_control&~0x200)|((v&ADEN)<<9));
        }
        if (((old^v)&NRES) || (!(old&ADEN) && (v&ADEN) && (v&NRES)))
            flags|=(v&NRES)?KEGA32X_RESET_RELEASED:KEGA32X_RESET_ASSERTED;
    } else {
        b->s.regs[0]=(uint16_t)((old&~FM)|(v&FM));
        b->s.irq_mask[cpu]=(uint8_t)(v&15);
        b->s.sh2_control=(uint16_t)((b->s.sh2_control&~0x80)|(v&0x80));
        flags|=KEGA32X_HINT_CHANGED;
    }
    if ((old^v)&FM) flags|=KEGA32X_MAP_CHANGED;
    if (flags) changed(b,cpu,flags,0,t);
    command_irq(b,t);
}
static void vdp_write(Kega32xBus *b, unsigned cpu, unsigned a, uint16_t v, uint64_t t)
{
    unsigned old;
    if (!owns_video(b,cpu)) return;
    switch (a) {
    case 0:
        render_sync(b,t);
        b->s.vdp[0]=(uint16_t)((b->s.vdp[0]&0x8000)|(v&255));
        if (!(v&1) || (b->s.vdp[5]&(VBLANK|HBLANK))) b->s.vdp[5]|=PEN;
        else b->s.vdp[5]&=(uint16_t)~PEN;
        break;
    case 2: render_sync(b,t); b->s.vdp[1]=v&1; break;
    case 4: b->s.vdp[2]=v&255; break;
    case 6: b->s.vdp[3]=v; break;
    case 8:
        if (b->memory) {
            unsigned count=b->s.vdp[2]+1, address=b->s.vdp[3], i;
            uint16_t *fb;
            kega32x_fb_sync(b,t);
            fb=b->memory->framebuffer[(b->s.vdp[5]&1)^1];
            for (i=0;i<count;i++) {
                fb[address]=v;
                address=(address&0xff00)|((address+1)&255);
            }
            b->s.vdp[3]=(uint16_t)address;
            b->s.vdp[4]=v;
            b->s.vdp[5]|=FEN;
            /* Donor latency (3+length) 68000 clocks -> GPX master clocks. */
            b->s.fill_end_mclk=t+7u*(3u+count);
        }
        break;
    case 10:
        b->s.pending_fb=v&1;
        old=b->s.vdp[5];
        if ((old&VBLANK) || !(b->s.vdp[0]&3)) {
            render_sync(b,t);
            b->s.vdp[5]=(uint16_t)((old&~1)|(v&1));
            if((old^v)&1)changed(b,cpu,KEGA32X_MAP_CHANGED,0x10a,t);
        }
        break;
    default: break;
    }
}
void kega32x_bus_write16(Kega32xBus *b, unsigned cpu, uint32_t a, uint16_t v, uint64_t t)
{
    uint16_t *r=b->s.regs;
    if (cpu>KEGA32X_M68K) return;
    a&=~1u;
    if (!(b->s.regs[0]&ADEN) && a>=0x30) return;
    if (a>=0x200 && a<0x400) {
        if (owns_video(b,cpu)) {
            render_sync(b,t);
            b->s.palette[(a&511)/2]=v;
        }
        return;
    }
    if (a>=0x100 && a<0x110) { vdp_write(b,cpu,a&15,v,t); return; }
    if (a>=0x40) return;
    if (a==0) { adapter_write(b,cpu,v,t); return; }
    if (a>=0x20 && a<0x30) {
        if (r[a/2]!=v) { r[a/2]=v; changed(b,cpu,KEGA32X_COMM_CHANGED,a,t); }
        return;
    }
    if (a>=0x30) {
        if (a==0x30) v=(uint16_t)(cpu==KEGA32X_M68K ? (r[a/2]&~15)|(v&15) : v&0xf8f);
        if (a==0x32) v&=0xfff;
        if (a<=0x38) {
            r[a/2]=v;
            if (b->host.pwm_write) b->host.pwm_write(b->host.userdata,a,v,t);
        }
        return;
    }
    if (cpu!=KEGA32X_M68K) {
        if (a==4) { b->s.hcount=(uint8_t)v; changed(b,cpu,KEGA32X_HINT_CHANGED,a,t); }
        else if (a==0x1a) { r[1]&=(uint16_t)~(1u<<cpu); command_irq(b,t); }
        else if (a>=0x14 && a<=0x1c) {
            unsigned bit=KEGA32X_IRQ_RESET>>((a-0x14)/2);
            b->s.irq_pending[cpu]&=(uint8_t)~bit;
            irqs(b,t);
        }
        return;
    }
    switch (a) {
    case 2: r[1]=v&3; command_irq(b,t); break;
    case 4:
        if (r[2]!=(v&3)) { r[2]=v&3; changed(b,cpu,KEGA32X_MAP_CHANGED,a,t); }
        break;
    case 6:
        if (!(v&S68)) { b->s.fifo_count=b->s.fifo_read=b->s.fifo_write=b->s.fifo_ready=0; r[3]&=(uint16_t)~FULL; }
        r[3]=(uint16_t)((r[3]&FULL)|(v&7));
        changed(b,cpu,KEGA32X_MAP_CHANGED,a,t);
        break;
    case 8: case 12: r[a/2]=v&255; break;
    case 10: r[5]=v&0xfffe; break;
    case 14: r[7]=v; break;
    case 16: r[8]=v&0xfffc; break;
    case 18: dreq_push(b,v,t); break;
    case 26: r[13]=v&0x101; break;
    default: break;
    }
}
void kega32x_bus_write8(Kega32xBus *b, unsigned cpu, uint32_t a, uint8_t v, uint64_t t)
{
    uint16_t old, combined;
    unsigned shift=(a&1)?0:8;
    if (cpu>KEGA32X_M68K) return;
    if (a>=0x200 && a<0x400) {
        old=b->s.palette[(a&511)/2];
        combined=(uint16_t)((old&~(0xffu<<shift))|((uint16_t)v<<shift));
        kega32x_bus_write16(b,cpu,a,combined,t); return;
    }
    if (a>=0x400) return;
    if (a>=0x100) {
        if (a>=0x110 || !(a&1) || a==0x107 || a==0x109) return;
        kega32x_bus_write16(b,cpu,a,v,t); return;
    }
    if (a>=0x40) return;
    if (a==2 || a==4 || a==6) return;
    old=b->s.regs[a/2];
    if (a<2 && cpu!=KEGA32X_M68K) old=(uint16_t)((old&FM)|b->s.irq_mask[cpu]|(b->s.sh2_control&0x80));
    combined=(uint16_t)((old&~(0xffu<<shift))|((uint16_t)v<<shift));
    if (a==0x12 || a==0x34 || a==0x36 || a==0x38) { b->s.regs[a/2]=combined; return; }
    if (a>=0x14 && a<0x20 && cpu!=KEGA32X_M68K) return;
    kega32x_bus_write16(b,cpu,a,combined,t);
    if (a==0x13 || a==0x35 || a==0x37 || a==0x39) b->s.regs[a/2]=0;
}
uint16_t kega32x_fb_read16(const Kega32xBus *b, uint32_t a)
{
    return b->memory ? b->memory->framebuffer[(b->s.vdp[5]&1)^1][(a&0x1ffff)>>1]:0;
}
void kega32x_fb_write16(Kega32xBus *b, uint32_t a, uint16_t v)
{
    uint16_t *p;
    if (!b->memory) return;
    p=&b->memory->framebuffer[(b->s.vdp[5]&1)^1][(a&0x1ffff)>>1];
    if (a&0x20000) {
        if (!(v&255)) v|=*p&255;
        if (!(v&0xff00)) v|=*p&0xff00;
    }
    *p=v;
}
void kega32x_fb_write8(Kega32xBus *b, uint32_t a, uint8_t v)
{
    uint16_t *p;
    unsigned shift=(a&1)?0:8;
    if (!b->memory || !v) return;
    p=&b->memory->framebuffer[(b->s.vdp[5]&1)^1][(a&0x1ffff)>>1];
    *p=(uint16_t)((*p&~(255u<<shift))|((uint16_t)v<<shift));
}
