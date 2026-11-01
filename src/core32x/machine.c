/* Kega Refusion: one owned 32X attachment to the existing GPX machine.
 * Address map and byte-lane rules adapted from PicoDrive 07abb005 memory.c;
 * (C) notaz 2009,2010,2013; (C) irixxxx 2019-2024.
 * See third_party/picodrive32x/COPYING. Changed: explicit ownership/callbacks,
 * absolute master-clock timeline and independently allocated CPU contexts. */
#include "machine.h"
#include "state.h"
#include <assert.h>
#include "sh2/soc.h"
#ifdef KEGA_SH2_DRC
#include "sh2/drc.h"
#endif
#include <picodrive32x/cpu/sh2/sh2.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
struct Kega32xMachine {
    Kega32xBus bus;
    Kega32xMemory memory;
    Kega32xPwm pwm;
    KegaSh2 *cpu[2];
    KegaSh2Soc *soc[2];
    unsigned soc_running;
    Kega32xMachineHost host;
    uint8_t bios[2][2048];
    uint64_t epoch_sh2[2], now;
    unsigned running, irq_level[2];
};
static uint64_t sh2_clock(uint64_t t) { return (t/7)*3+(t%7)*3/7; }
static uint64_t master_clock(uint64_t t) { return (t/3)*7+((t%3)*7+2)/3; }
Kega32xBus *kega32x_machine_bus(Kega32xMachine *m) { return m?&m->bus:NULL; }
Kega32xPwm *kega32x_machine_pwm(Kega32xMachine *m) { return m?&m->pwm:NULL; }
KegaSh2 *kega32x_machine_cpu(Kega32xMachine *m, unsigned c) { return m && c<2?m->cpu[c]:NULL; }
uint64_t kega32x_machine_cpu_mclk(const Kega32xMachine *m, unsigned c)
{
    uint64_t local;
    if(!m || c>=2)return 0;
    local=(m->soc_running&(1u<<c))?kega_sh2_soc_now(m->soc[c]):kega_sh2_cycles(m->cpu[c]);
    return master_clock(m->epoch_sh2[c]+local);
}
static unsigned cpu_index(Kega32xMachine *m, KegaSh2 *c) { return c==m->cpu[1]; }
static void bus_irq(void *p,unsigned c,unsigned level,uint64_t t)
{
    Kega32xMachine *m=p; (void)t;
    if (m->irq_level[c]!=level) { m->irq_level[c]=level; kega_sh2_set_irq(m->cpu[c],level); }
}
static void bus_changed(void *p,unsigned c,unsigned flags,uint32_t a,uint64_t t)
{
    Kega32xMachine *m=p;
    unsigned i;
    if (flags&KEGA32X_RESET_RELEASED) {
        for (i=0;i<2;i++) {
            kega_sh2_reset(m->cpu[i]);
            m->epoch_sh2[i]=sh2_clock(t);
            kega_sh2_soc_reset(m->soc[i],0);
            if (m->host.reset_peripherals) m->host.reset_peripherals(m->host.userdata,i,t);
            m->irq_level[i]=0;
        }
    }
    /* An MMIO change can require peers to run earlier than the outer slice. */
    for (i=0;i<2;i++) if (m->running&(1u<<i)) kega_sh2_end_run(m->cpu[i]);
    if (m->host.changed) m->host.changed(m->host.userdata,c,flags,a,t);
}
static void dreq0(void *p,uint64_t t) {
    Kega32xMachine *m=p;
    unsigned i;uint64_t stamp=sh2_clock(t);
    for(i=0;i<2;i++)kega_sh2_soc_dreq_at(m->soc[i],0,stamp>m->epoch_sh2[i]?stamp-m->epoch_sh2[i]:0);
    if(m->host.dreq)m->host.dreq(m->host.userdata,0,t);
}
static void dreq1(void *p,uint64_t t) {
    Kega32xMachine *m=p;
    unsigned i;uint64_t stamp=sh2_clock(t);
    for(i=0;i<2;i++)kega_sh2_soc_dreq_at(m->soc[i],1,stamp>m->epoch_sh2[i]?stamp-m->epoch_sh2[i]:0);
    if(m->host.dreq)m->host.dreq(m->host.userdata,1,t);
}
static void render(void *p,uint64_t t) { Kega32xMachine *m=p; if(m->host.render_sync)m->host.render_sync(m->host.userdata,t); }
static uint16_t pwm_read(void *p,unsigned a,uint64_t t) { return kega32x_pwm_read16(&((Kega32xMachine *)p)->pwm,a,t); }
static void pwm_write(void *p,unsigned a,uint16_t v,uint64_t t) { kega32x_pwm_write16(&((Kega32xMachine *)p)->pwm,a,v,t); }
static void pwm_irq(void *p,uint64_t t) { kega32x_bus_irq(&((Kega32xMachine *)p)->bus,KEGA32X_IRQ_PWM,t); }
static void audio_delta(void *p,uint64_t t,int32_t l,int32_t r)
{
    Kega32xMachine *m=p; if(m->host.audio_delta)m->host.audio_delta(m->host.userdata,t,l,r);
}
static void sync_peer(Kega32xMachine *m,unsigned c,uint64_t t)
{
    unsigned other=c^1;
    uint64_t goal,done;
    if(m->soc_running || !(m->running&(1u<<c)))return;
    if(m->running&(1u<<other)) {kega_sh2_end_run(m->cpu[c]);return;}
    goal=sh2_clock(t);
    done=m->epoch_sh2[other]+kega_sh2_cycles(m->cpu[other]);
    if(goal>done && goal-done<INT_MAX/2) {
        m->running|=1u<<other;
        kega_sh2_run(m->cpu[other],(uint32_t)(goal-done));
        m->running&=~(1u<<other);
    }
}
static uint32_t peripheral_read(Kega32xMachine *m,unsigned c,uint32_t a,unsigned size)
{
    if(m->host.peripheral_read)return m->host.peripheral_read(m->host.userdata,c,a,size);
    if(size==1)return kega_sh2_soc_read8(m->soc[c],a);
    if(size==2)return kega_sh2_soc_read16(m->soc[c],a);
    return kega_sh2_soc_read32(m->soc[c],a);
}
static void peripheral_write(Kega32xMachine *m,unsigned c,uint32_t a,uint32_t v,unsigned size)
{
    if(m->host.peripheral_write) {m->host.peripheral_write(m->host.userdata,c,a,v,size);return;}
    if(size==1)kega_sh2_soc_write8(m->soc[c],a,(uint8_t)v);
    else if(size==2)kega_sh2_soc_write16(m->soc[c],a,(uint16_t)v);
    else kega_sh2_soc_write32(m->soc[c],a,v);
}
static uint8_t read_byte(const uint8_t *p,uint32_t a) { return p[a]; }
static uint32_t read_words(const uint16_t *p,uint32_t a,unsigned size)
{
    if(size==1) return (p[a>>1]>>((a&1)?0:8))&255;
    if(size==2) return p[a>>1];
    return ((uint32_t)p[a>>1]<<16)|p[(a>>1)+1];
}
static void write_words(uint16_t *p,uint32_t a,uint32_t v,unsigned size)
{
    if(size==1) { unsigned shift=(a&1)?0:8; p[a>>1]=(uint16_t)((p[a>>1]&~(255u<<shift))|((v&255)<<shift)); }
    else if(size==2) p[a>>1]=(uint16_t)v;
    else { p[a>>1]=(uint16_t)(v>>16);p[(a>>1)+1]=(uint16_t)v; }
}
uint32_t kega32x_machine_read(Kega32xMachine *m,unsigned c,uint32_t address,unsigned size)
{
    uint32_t a=address>=0xc0000000?address:address&0xc7ffffff,off,v=0;
    uint64_t t;
    if(!m || c>1 || (size!=1 && size!=2 && size!=4))return 0;
    t=kega32x_machine_cpu_mclk(m,c);
    if(a>=0xc0000000 && a<0xe0000000) {
        uint8_t *data=kega_sh2_native(m->cpu[c])->data_array;
        /* Native donor data_array is in 16-bit word-swapped format. */
        return read_words((uint16_t *)(void *)data,(a&0xfff)&~(size-1),size);
    }
    if(a>=0xe0000000) return peripheral_read(m,c,a,size);
    if(a&0xc0000000)return 0;
    switch(a&0x06000000) {
    case 0:
        off=a&0x3ffff;
        if(off<0x4000) {
            size_t limit=c?1024:2048;
            unsigned i;
            for(i=0;i<size;i++) v=(v<<8)|((off+i<limit)?read_byte(m->bios[c],off+i):0);
            return v;
        }
        if ((off>=0x4000 && off<0x4040) || (off>=0x4100 && off<0x4110) || (off>=0x4200 && off<0x4400)) {
            sync_peer(m,c,t);
            kega_sh2_add_wait_cycles(m->cpu[c],2);
            off-=0x4000;
            if(size==1)return kega32x_bus_read8(&m->bus,c,off,t);
            if(size==2)return kega32x_bus_read16(&m->bus,c,off,t);
            return ((uint32_t)kega32x_bus_read16(&m->bus,c,off,t)<<16)|kega32x_bus_read16(&m->bus,c,off+2,t);
        }
        return 0;
    case 0x02000000:
        off=a&0x3fffff;
        if(!m->host.rom_read16)return 0xffff;
        v=m->host.rom_read16(m->host.userdata,off&~1u);
        if(size==1)return (v>>((off&1)?0:8))&255;
        if(size==4)v=(v<<16)|m->host.rom_read16(m->host.userdata,(off+2)&0x3ffffe);
        return v;
    case 0x04000000:
        if(size==1) {v=kega32x_fb_read16(&m->bus,a);return (v>>((a&1)?0:8))&255;}
        v=kega32x_fb_read16(&m->bus,a);
        return size==2?v:(v<<16)|kega32x_fb_read16(&m->bus,a+2);
    case 0x06000000:
        return read_words(m->memory.sdram,(a&0x3ffff)&~(size-1),size);
    }
    return 0;
}
void kega32x_machine_write(Kega32xMachine *m,unsigned c,uint32_t address,uint32_t value,unsigned size)
{
    uint32_t a=address>=0xc0000000?address:address&0xc7ffffff,off;
    uint64_t t;
    if(!m || c>1 || (size!=1 && size!=2 && size!=4))return;
    t=kega32x_machine_cpu_mclk(m,c);
#ifdef KEGA_SH2_DRC
    if(m->soc_running)kega_sh2_drc_invalidate(m->cpu[c],a,size);
#endif
    if(a>=0xc0000000 && a<0xe0000000) {
        write_words((uint16_t *)(void *)kega_sh2_native(m->cpu[c])->data_array,(a&0xfff)&~(size-1),value,size);return;
    }
    if(a>=0xe0000000) {peripheral_write(m,c,a,value,size);return;}
    /* Purge/cache-address-array writes must not alias external chip selects. */
    if(a&0xc0000000)return;
    switch(a&0x06000000) {
    case 0:
        off=a&0x3ffff;
        if(!((off>=0x4000 && off<0x4040)||(off>=0x4100 && off<0x4110)||(off>=0x4200 && off<0x4400)))return;
        sync_peer(m,c,t);
        off-=0x4000;
        if(size==1)kega32x_bus_write8(&m->bus,c,off,(uint8_t)value,t);
        else if(size==2)kega32x_bus_write16(&m->bus,c,off,(uint16_t)value,t);
        else {kega32x_bus_write16(&m->bus,c,off,(uint16_t)(value>>16),t);kega32x_bus_write16(&m->bus,c,off+2,(uint16_t)value,t);}
        break;
    case 0x04000000:
        if(size==1)kega32x_fb_write8(&m->bus,a,(uint8_t)value);
        else if(size==2)kega32x_fb_write16(&m->bus,a,(uint16_t)value);
        else {kega32x_fb_write16(&m->bus,a,(uint16_t)(value>>16));kega32x_fb_write16(&m->bus,a+2,(uint16_t)value);}
        break;
    case 0x06000000: write_words(m->memory.sdram,(a&0x3ffff)&~(size-1),value,size);break;
    default: break;
    }
}
/* Authorize the complete peek span without reading memory or MMIO. Physical
   mirrors are allowed, but a span must not wrap inside its backing storage. */
static bool can_idle_fetch(void *p,KegaSh2 *cpu,uint32_t address,size_t bytes)
{
    Kega32xMachine *m=p;
    const unsigned c=cpu_index(m,cpu);
    const uint32_t a=address>=0xc0000000?address:address&0xc7ffffff;
    uint32_t off;
    if(!bytes || (address&1) || bytes-1>UINT32_MAX-address)return false;
    if(a>=0xc0000000 && a<0xe0000000)return bytes<=0x1000u-(a&0xfff);
    if(a&0xc0000000)return false;
    switch(a&0x06000000) {
    case 0:
        off=a&0x3ffff;
        return off<(c?1024u:2048u) && bytes<=(c?1024u:2048u)-off;
    case 0x02000000:
        off=a&0x3fffff;
        return bytes<=0x400000u-off && m->host.rom_read16 && m->host.rom_fetch_is_pure &&
            m->host.rom_fetch_is_pure(m->host.userdata,off,bytes);
    case 0x06000000: return bytes<=0x40000u-(a&0x3ffff);
    default: return false;
    }
}
/* Instruction reads from plain storage bypass data-access synchronization and
   waits. All handler-backed addresses retain the normal memory access path. */
static uint16_t instruction_fetch16(void *p,KegaSh2 *cpu,uint32_t address)
{
    Kega32xMachine *m=p;
    const unsigned c=cpu_index(m,cpu);
    const uint32_t a=address>=0xc0000000?address:address&0xc7ffffff;
    uint32_t off;
    if(!(address&1)) {
        if(a>=0xc0000000 && a<0xe0000000)
            return ((uint16_t *)(void *)kega_sh2_native(m->cpu[c])->data_array)[(a&0xfff)>>1];
        if(!(a&0xc0000000))switch(a&0x06000000) {
        case 0:
            off=a&0x3ffff;
            if(off+1<(c?1024u:2048u))return (uint16_t)((m->bios[c][off]<<8)|m->bios[c][off+1]);
            break;
        case 0x02000000:
            off=a&0x3fffff;
            if(m->host.rom_read16 && m->host.rom_fetch_is_pure &&
               m->host.rom_fetch_is_pure(m->host.userdata,off,2))
                return m->host.rom_read16(m->host.userdata,off);
            break;
        case 0x06000000: return m->memory.sdram[(a&0x3ffff)>>1];
        default: break;
        }
    }
    return (uint16_t)kega32x_machine_read(m,c,address,2);
}
static uint8_t r8(void *p,KegaSh2 *c,uint32_t a) {Kega32xMachine *m=p;return (uint8_t)kega32x_machine_read(m,cpu_index(m,c),a,1);}
static uint16_t r16(void *p,KegaSh2 *c,uint32_t a) {Kega32xMachine *m=p;return (uint16_t)kega32x_machine_read(m,cpu_index(m,c),a,2);}
static uint32_t r32(void *p,KegaSh2 *c,uint32_t a) {Kega32xMachine *m=p;return kega32x_machine_read(m,cpu_index(m,c),a,4);}
static void w8(void *p,KegaSh2 *c,uint32_t a,uint8_t v) {Kega32xMachine *m=p;kega32x_machine_write(m,cpu_index(m,c),a,v,1);}
static void w16(void *p,KegaSh2 *c,uint32_t a,uint16_t v) {Kega32xMachine *m=p;kega32x_machine_write(m,cpu_index(m,c),a,v,2);}
static void w32(void *p,KegaSh2 *c,uint32_t a,uint32_t v) {Kega32xMachine *m=p;kega32x_machine_write(m,cpu_index(m,c),a,v,4);}
static unsigned fifo_available(void *p,KegaSh2 *c) {(void)c;return kega32x_dreq_available(&((Kega32xMachine *)p)->bus);}
static uint16_t fifo_pop(void *p,KegaSh2 *c) {(void)c;return kega32x_dreq_pop(&((Kega32xMachine *)p)->bus);}
static void schedule_changed(void *p,KegaSh2 *c,uint64_t deadline)
{
    Kega32xMachine *m=p;
    unsigned i=cpu_index(m,c);
    if(m->running&(1u<<i))kega_sh2_end_run(c);
    if(m->host.changed && deadline!=UINT64_MAX)
        m->host.changed(m->host.userdata,i,KEGA32X_COMM_CHANGED,0xfffffe00,master_clock(m->epoch_sh2[i]+deadline));
}
Kega32xMachine *kega32x_machine_create(const Kega32xMachineHost *h,const uint8_t *mb,size_t ml,const uint8_t *sb,size_t sl,int pal,int no_cart)
{
    Kega32xMachine *m;
    KegaSh2Host ch={0}; KegaSh2SocHost sh={0}; Kega32xBusHost bh={0}; Kega32xPwmHost ph={0};
    if(!h || !mb || !sb || ml!=2048 || sl!=1024)return NULL;
    m=calloc(1,sizeof(*m)); if(!m)return NULL;
    m->host=*h;memcpy(m->bios[0],mb,ml);memcpy(m->bios[1],sb,sl);
    ch.user=m;ch.read8=r8;ch.read16=r16;ch.read32=r32;ch.write8=w8;ch.write16=w16;ch.write32=w32;
    ch.fetch16=instruction_fetch16;ch.can_idle_fetch=can_idle_fetch;
    m->cpu[0]=kega_sh2_create(&ch,0);m->cpu[1]=kega_sh2_create(&ch,1);
    if(!m->cpu[0] || !m->cpu[1]) {kega32x_machine_destroy(m);return NULL;}
    kega_sh2_set_peer(m->cpu[0],m->cpu[1]);kega_sh2_set_peer(m->cpu[1],m->cpu[0]);
    sh.user=m;sh.read8=r8;sh.read16=r16;sh.read32=r32;sh.write8=w8;sh.write16=w16;sh.write32=w32;
    sh.schedule_changed=schedule_changed;sh.dreq_available=fifo_available;sh.dreq_pop=fifo_pop;
    m->soc[0]=kega_sh2_soc_create(m->cpu[0],&sh);m->soc[1]=kega_sh2_soc_create(m->cpu[1],&sh);
    if(!m->soc[0] || !m->soc[1]) {kega32x_machine_destroy(m);return NULL;}
    kega_sh2_soc_set_peer(m->soc[0],m->soc[1]);kega_sh2_soc_set_peer(m->soc[1],m->soc[0]);
    bh.userdata=m;bh.changed=bus_changed;bh.irq=bus_irq;bh.dreq=dreq0;bh.render_sync=render;bh.pwm_read=pwm_read;bh.pwm_write=pwm_write;
    ph.userdata=m;ph.delta=audio_delta;ph.irq=pwm_irq;ph.dreq=dreq1;
    kega32x_bus_power(&m->bus,&m->memory,&bh,pal,no_cart);kega32x_pwm_power(&m->pwm,&ph,0);
    {
        const char *reference=getenv("KEGA32X_INTERPRETER");
        const bool fast=!reference || strcmp(reference,"1");
        (void)kega_sh2_set_idle_fastpath(m->cpu[0],fast);
        (void)kega_sh2_set_idle_fastpath(m->cpu[1],fast);
#ifdef KEGA_SH2_DRC
        if(fast)(void)kega_sh2_drc_enable(m->cpu[0],m->cpu[1]);
#endif
    }
    return m;
}
void kega32x_machine_power_reset(Kega32xMachine *m,int pal,int no_cart)
{
    unsigned i;
    Kega32xBusHost bh;
    Kega32xPwmHost ph;
    if(!m || m->running || m->soc_running)return;
#ifdef KEGA_SH2_DRC
    if(kega_sh2_drc_active(m->cpu[0]))kega_sh2_drc_flush();
#endif
    bh=m->bus.host;ph=m->pwm.host;
    m->now=0;m->epoch_sh2[0]=m->epoch_sh2[1]=0;
    kega32x_bus_power(&m->bus,&m->memory,&bh,pal,no_cart);
    kega32x_pwm_power(&m->pwm,&ph,0);
    for(i=0;i<2;i++) {
        memset(kega_sh2_native(m->cpu[i])->data_array,0,0x1000);
        kega_sh2_reset(m->cpu[i]);
        kega_sh2_soc_reset(m->soc[i],0);
        m->irq_level[i]=0;
    }
}
void kega32x_machine_destroy(Kega32xMachine *m)
{
    if(m) {kega_sh2_soc_destroy(m->soc[0]);kega_sh2_soc_destroy(m->soc[1]);kega_sh2_destroy(m->cpu[0]);kega_sh2_destroy(m->cpu[1]);free(m);}
}
uint64_t kega32x_machine_next_event(const Kega32xMachine *m)
{
    uint64_t next=kega32x_pwm_next_deadline(&m->pwm);
    if((m->bus.s.vdp[5]&2) && m->bus.s.fill_end_mclk<next)next=m->bus.s.fill_end_mclk;
    if((m->bus.s.regs[0]&3)==3) {
        unsigned i;
        for(i=0;i<2;i++) {
            uint64_t t=kega_sh2_soc_next_deadline(m->soc[i],kega_sh2_soc_now(m->soc[i]));
            if(t!=UINT64_MAX) {t=master_clock(m->epoch_sh2[i]+t);if(t<next)next=t;}
        }
    }
    return next;
}
static void advance_devices(Kega32xMachine *m,uint64_t t)
{
    unsigned i;
    kega32x_bus_advance(&m->bus,t);kega32x_pwm_advance(&m->pwm,t);
    if((m->bus.s.regs[0]&3)==3) for(i=0;i<2;i++) {
        uint64_t local=sh2_clock(t);
        if(local<m->epoch_sh2[i])continue;
        m->soc_running|=1u<<i;
        kega_sh2_soc_advance(m->soc[i],local-m->epoch_sh2[i]);
        m->soc_running&=~(1u<<i);
    }
}
void kega32x_machine_run_until(Kega32xMachine *m,uint64_t target)
{
    if(!m || m->running || target<=m->now)return;
    while(m->now<target) {
        uint64_t next,event;
        advance_devices(m,m->now);
        next=target;event=kega32x_machine_next_event(m);
        int c;
        /* Bounded reference scheduling; performance tuning follows real boot. */
        if(next-m->now>448)next=m->now+448;
        if(event>m->now && event<next)next=event;
        if((m->bus.s.regs[0]&3)==3) {
            for(c=1;c>=0;c--) {
                uint64_t done=m->epoch_sh2[c]+kega_sh2_cycles(m->cpu[c]),goal=sh2_clock(next);
                if(goal>done) {
                    m->running|=1u<<c;
                    kega_sh2_run(m->cpu[c],(uint32_t)(goal-done));
                    m->running&=~(1u<<c);
                }
            }
        }
        if((m->bus.s.regs[0]&3)==3) {
            uint64_t goal=sh2_clock(next);
            unsigned i;
            for(i=0;i<2;i++) {
                uint64_t done=m->epoch_sh2[i]+kega_sh2_cycles(m->cpu[i]);
                if(done<goal) {
                    uint64_t reached=master_clock(done);
                    if(reached<next)next=reached;
                }
            }
            /* A yielded CPU must catch up before hardware time is committed. */
            if(next<=m->now) return;
        }
        m->now=next;
        advance_devices(m,next);
    }
}

/* State transfer deliberately remains separate from CPU/device hot paths. */
int kega32x_machine_get_state(const Kega32xMachine *m,Kega32xMachineState *state)
{
    Kega32xControlState control={0};
    unsigned i;
    if(!m || !state || m->running || m->soc_running || m->pwm.advancing)return 0;
    control.version=KEGA32X_MACHINE_STATE_VERSION;control.bus=m->bus.s;
    kega32x_pwm_get_state(&m->pwm,&control.pwm);
    for(i=0;i<2;i++) {
        if(!kega_sh2_get_state(m->cpu[i],&control.cpu[i]) ||
           !kega_sh2_soc_get_state(m->soc[i],&control.soc[i]))return 0;
        control.epoch_sh2[i]=m->epoch_sh2[i];
    }
    control.now=m->now;
    if(!kega32x_control_state_validate(&control))return 0;
    state->control=control;state->memory=m->memory;return 1;
}
int kega32x_machine_set_state(Kega32xMachine *m,const Kega32xMachineState *state)
{
    KegaSh2State current_cpu;
    KegaSh2SocState current_soc;
    const Kega32xControlState *s;
    unsigned i;int okay;
    if(!m || !state || m->running || m->soc_running || m->pwm.advancing ||
       !kega32x_state_validate(state))return 0;
    s=&state->control;
    /* Region and cartridge/CD topology belong to this live attachment. */
    if(((s->bus.sh2_control^m->bus.s.sh2_control)&0x100) ||
       ((s->bus.vdp[0]^m->bus.s.vdp[0])&0x8000))return 0;
    for(i=0;i<2;i++)
        if(!kega_sh2_get_state(m->cpu[i],&current_cpu) ||
           !kega_sh2_soc_get_state(m->soc[i],&current_soc))return 0;
    /* No rejection remains after this point. The pure validators above are
       the setters' own preconditions, checked for both CPUs/devices first. */
    m->memory=state->memory;m->bus.s=s->bus;m->now=s->now;
    for(i=0;i<2;i++)m->epoch_sh2[i]=s->epoch_sh2[i];
    for(i=0;i<2;i++) {
        /* SoC restore may refresh the internal IRQ input. Restore CPU last to
           retain its exact pending/test-IRQ architecture at this boundary. */
        okay=kega_sh2_soc_set_state(m->soc[i],&s->soc[i]);assert(okay);
        if(!okay)return 0;
        okay=kega_sh2_set_state(m->cpu[i],&s->cpu[i]);assert(okay);
        if(!okay)return 0;
        m->irq_level[i]=s->cpu[i].pending_irl;
    }
    kega32x_bus_restore(&m->bus,m->now);
    okay=kega32x_pwm_set_state(&m->pwm,&s->pwm);assert(okay);
    return okay;
}
