/* Synthetic SH-2 peripheral scenarios, no firmware/game content. */
#include "core32x/sh2/soc.h"
#ifdef NDEBUG
#undef NDEBUG /* These executable checks must run in Release builds too. */
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    KegaSh2Soc *soc;
    unsigned char memory[0x2000];
    uint16_t fifo[8], pwm;
    unsigned available, popped, writes, irq_count, level, vector;
    uint64_t deadline;
} Fixture;
static uint8_t r8(void *u, KegaSh2 *c, uint32_t a) {
    Fixture *f=u; (void)c;
    if(a>=0xfffffe00u)return kega_sh2_soc_read8(f->soc,a);
    assert(a<sizeof(f->memory)); return f->memory[a];
}
static uint16_t r16(void *u, KegaSh2 *c, uint32_t a) {
    return (uint16_t)((uint16_t)r8(u,c,a)<<8 | r8(u,c,a+1));
}
static uint32_t r32(void *u, KegaSh2 *c, uint32_t a) {
    return (uint32_t)r16(u,c,a)<<16 | r16(u,c,a+2);
}
static void w8(void *u, KegaSh2 *c, uint32_t a, uint8_t v) {
    Fixture *f=u; (void)c;
    if(a>=0xfffffe00u){kega_sh2_soc_write8(f->soc,a,v);return;}
    assert(a<sizeof(f->memory)); f->memory[a]=v; ++f->writes;
}
static void w16(void *u, KegaSh2 *c, uint32_t a, uint16_t v) {
    Fixture *f=u; (void)c;
    if(a>=0xfffffe00u){kega_sh2_soc_write16(f->soc,a,v);return;}
    if((a&0xdffffff0u)==0x4030) { f->pwm=v; ++f->writes; return; }
    assert(a+1<sizeof(f->memory)); f->memory[a]=(uint8_t)(v>>8); f->memory[a+1]=(uint8_t)v; ++f->writes;
}
static void w32(void *u, KegaSh2 *c, uint32_t a, uint32_t v) {
    Fixture *f=u; (void)c;
    if(a>=0xfffffe00u){kega_sh2_soc_write32(f->soc,a,v);return;}
    assert(a+3<sizeof(f->memory));
    f->memory[a]=(uint8_t)(v>>24); f->memory[a+1]=(uint8_t)(v>>16);
    f->memory[a+2]=(uint8_t)(v>>8); f->memory[a+3]=(uint8_t)v; ++f->writes;
}
static void irq(void *u, KegaSh2 *c, unsigned level, unsigned vector) {
    Fixture *f=u; ++f->irq_count; f->level=level; f->vector=vector;
    assert(kega_sh2_set_internal_irq(c,level,vector));
}
static void schedule(void *u, KegaSh2 *c, uint64_t deadline) {
    Fixture *f=u; (void)c; f->deadline=deadline;
}
static unsigned available(void *u, KegaSh2 *c) {
    Fixture *f=u; (void)c; return f->available>=4 ? f->available : 0;
}
static uint16_t pop(void *u, KegaSh2 *c) {
    Fixture *f=u; (void)c; assert(f->available && f->popped<8); --f->available; return f->fifo[f->popped++];
}
static KegaSh2Soc *create(Fixture *f, bool slave, KegaSh2 **cpu) {
    KegaSh2Host h={0}; h.user=f; h.read8=r8; h.read16=r16; h.read32=r32; h.write8=w8; h.write16=w16; h.write32=w32;
    *cpu=kega_sh2_create(&h,slave); assert(*cpu);
    KegaSh2SocHost s={f,r8,r16,r32,w8,w16,w32,irq,schedule,available,pop};
    KegaSh2Soc *soc=kega_sh2_soc_create(*cpu,&s); assert(soc); f->soc=soc; return soc;
}
static void reset(KegaSh2Soc *s, Fixture *f) {
    kega_sh2_soc_reset(s,0); f->writes=f->irq_count=f->popped=f->available=0;
}
static void dma(KegaSh2Soc *s, unsigned ch, unsigned source, unsigned destination, unsigned count, unsigned control) {
    kega_sh2_soc_write32(s,0x180+ch*16,source); kega_sh2_soc_write32(s,0x184+ch*16,destination);
    kega_sh2_soc_write32(s,0x188+ch*16,count); kega_sh2_soc_write32(s,0x18c+ch*16,control);
    kega_sh2_soc_write32(s,0x1b0,1);
}
static void frt_bulk_deadlines(void) {
    static const struct { unsigned a,b,start,clock,clear; } cases[]={
        {1,0,0,0,1},{3,5,0,0,1},{3,1,0,1,1},
        {3,65535,65530,0,1},{0,0,65535,0,1},{0,5,65530,2,1},{3,5,0,0,0}
    };
    for(unsigned j=0;j<sizeof(cases)/sizeof(cases[0]);++j) {
        Fixture f={0},g={0};KegaSh2 *fc,*gc;
        KegaSh2Soc *a=create(&f,false,&fc),*b=create(&g,false,&gc);
        KegaSh2Soc *both[]={a,b};
        for(unsigned k=0;k<2;++k) {
            KegaSh2Soc *c=both[k];
            kega_sh2_soc_write8(c,0x16,(uint8_t)cases[j].clock);
            kega_sh2_soc_write16(c,0x14,(uint16_t)cases[j].a);
            kega_sh2_soc_write8(c,0x17,0xf0);
            kega_sh2_soc_write16(c,0x14,(uint16_t)cases[j].b);
            kega_sh2_soc_write8(c,0x17,0xe0);
            kega_sh2_soc_write8(c,0x11,(uint8_t)cases[j].clear);
            kega_sh2_soc_advance(c,3); /* Non-zero sub-clock phase. */
            kega_sh2_soc_write16(c,0x12,(uint16_t)cases[j].start);
            assert(kega_sh2_soc_next_deadline(c,3)==UINT64_MAX);
        }
        kega_sh2_soc_advance(a,800003);
        for(uint64_t t=10;t<800003;t+=7) kega_sh2_soc_advance(b,t);
        kega_sh2_soc_advance(b,800003);
        KegaSh2SocState x,y;assert(kega_sh2_soc_get_state(a,&x));assert(kega_sh2_soc_get_state(b,&y));
        assert(!memcmp(&x,&y,sizeof(x)) && !f.level && !g.level);
        assert(kega_sh2_soc_next_deadline(a,800003)==UINT64_MAX);
        kega_sh2_soc_destroy(a);kega_sh2_soc_destroy(b);kega_sh2_destroy(fc);kega_sh2_destroy(gc);
    }
    Fixture f={0};KegaSh2 *cpu;KegaSh2Soc *c=create(&f,false,&cpu);
    kega_sh2_soc_write8(c,0x60,7);kega_sh2_soc_write8(c,0x67,92);
    kega_sh2_soc_write16(c,0x14,1);kega_sh2_soc_write8(c,0x11,1);
    assert(kega_sh2_soc_next_deadline(c,0)==UINT64_MAX); /* CCLRA only. */
    kega_sh2_soc_advance(c,800000);assert(kega_sh2_soc_read16(c,0x12)==0);
    assert((kega_sh2_soc_read8(c,0x11)&0x0e)==8 && !f.level);
    /* Enabling an already latched flag asserts immediately; no periodic wake. */
    kega_sh2_soc_write8(c,0x10,8);assert(f.level==7 && f.vector==92);
    assert(kega_sh2_soc_next_deadline(c,800000)==UINT64_MAX);
    unsigned published=f.irq_count;kega_sh2_soc_advance(c,1600000);
    assert(f.irq_count==published && f.level==7);
    kega_sh2_soc_write8(c,0x11,1);assert(!f.level);
    assert(kega_sh2_soc_next_deadline(c,1600000)==1600008);
    kega_sh2_soc_advance(c,1600007);assert(!f.level);
    kega_sh2_soc_advance(c,1600008);assert(f.level==7);
    /* Auto-clear A=1 prevents B=5 and overflow from ever becoming pending. */
    kega_sh2_soc_write8(c,0x10,6);kega_sh2_soc_write8(c,0x11,1);
    kega_sh2_soc_write8(c,0x17,0xf0);kega_sh2_soc_write16(c,0x14,5);
    kega_sh2_soc_write8(c,0x17,0xe0);
    assert(kega_sh2_soc_next_deadline(c,1600008)==UINT64_MAX);
    kega_sh2_soc_advance(c,2400008);assert(!f.level && !(kega_sh2_soc_read8(c,0x11)&6));
    kega_sh2_soc_destroy(c);kega_sh2_destroy(cpu);
}
int main(void) {
    frt_bulk_deadlines();
    Fixture f={0}, peer={0}; KegaSh2 *cpu, *peer_cpu;
    KegaSh2Soc *s=create(&f,false,&cpu), *p=create(&peer,true,&peer_cpu);
    kega_sh2_soc_set_peer(s,p); kega_sh2_soc_set_peer(p,s);
    assert(kega_sh2_soc_read8(s,1)==0xff && kega_sh2_soc_read8(s,3)==0xff);
    assert(kega_sh2_soc_read8(s,4)==0x84 && kega_sh2_soc_read8(s,0x10)==1 && kega_sh2_soc_read8(s,0x17)==0xe0);
    kega_sh2_soc_write32(s,0x1e4,0xdeadbeef);
    assert(kega_sh2_soc_read8(s,0x1e4)==0xde && kega_sh2_soc_read16(s,0x1e4)==0xdead);
    assert(kega_sh2_soc_read32(s,0x1e4)==0xdeadbeef);
    KegaSh2State native; assert(kega_sh2_get_state(cpu,&native) && native.peri_regs[0x1e4/4]==0xdeadbeef);

    /* Signed normal, zero, 32-bit and 64-bit overflow cases must not invoke UB. */
    kega_sh2_soc_write32(s,0x100,3); kega_sh2_soc_write32(s,0x104,10);
    assert(kega_sh2_soc_read32(s,0x104)==3 && kega_sh2_soc_read32(s,0x110)==1);
    assert(kega_sh2_soc_read32(s,0x118)==1 && kega_sh2_soc_read32(s,0x11c)==3);
    kega_sh2_soc_write8(s,0xe2,0x60); kega_sh2_soc_write32(s,0x10c,88); kega_sh2_soc_write32(s,0x108,2);
    kega_sh2_soc_write32(s,0x100,0xffffffff); kega_sh2_soc_write32(s,0x104,0x80000000);
    assert(kega_sh2_soc_read32(s,0x104)==0x7fffffff && (kega_sh2_soc_read32(s,0x108)&1));
    assert(f.level==6 && f.vector==88);
    kega_sh2_soc_write32(s,0x100,0); kega_sh2_soc_write32(s,0x104,123);
    assert(kega_sh2_soc_read32(s,0x104)==0x7fffffff);
    kega_sh2_soc_write32(s,0x100,3); kega_sh2_soc_write32(s,0x110,0xffffffff); kega_sh2_soc_write32(s,0x114,0xfffffff6);
    assert(kega_sh2_soc_read32(s,0x114)==0xfffffffd && kega_sh2_soc_read32(s,0x110)==0xffffffff);
    kega_sh2_soc_write32(s,0x100,0xffffffff); kega_sh2_soc_write32(s,0x110,0x80000000); kega_sh2_soc_write32(s,0x114,0);
    assert(kega_sh2_soc_read32(s,0x114)==0x7fffffff && (kega_sh2_soc_read32(s,0x108)&1));

    /* Auto DMA is deferred and bounded even with a huge scheduler jump/TCR=0. */
    reset(s,&f); for(unsigned i=0;i<32;++i) f.memory[0x100+i]=(uint8_t)(i+1);
    kega_sh2_soc_write8(s,0xe2,5); kega_sh2_soc_write32(s,0x1a0,90);
    dma(s,0,0x100,0x200,10,0x5205); assert(f.writes==0 && f.deadline==2);
    kega_sh2_soc_advance(s,1); assert(f.writes==0);
    kega_sh2_soc_advance(s,2); assert(f.writes==1);
    kega_sh2_soc_advance(s,20); assert(f.writes==5 && kega_sh2_soc_read32(s,0x188)==5);
    kega_sh2_soc_advance(s,28); assert(f.writes==9);
    kega_sh2_soc_advance(s,30); assert(f.writes==10 && (kega_sh2_soc_read32(s,0x18c)&2));
    assert(!memcmp(f.memory+0x100,f.memory+0x200,10) && f.irq_count==1 && f.level==5 && f.vector==90);
    reset(s,&f); dma(s,0,0x100,0x300,0,0x5201);
    kega_sh2_soc_advance(s,1000000);
    assert(f.writes==4 && kega_sh2_soc_read32(s,0x188)==0xfffffc);
    assert(kega_sh2_soc_next_deadline(s,1000000)==1000002);
    kega_sh2_soc_write32(s,0x1b0,0); assert(kega_sh2_soc_next_deadline(s,1000000)==UINT64_MAX);

    /* Four-long burst uses ascending addresses inside a descending block. */
    reset(s,&f); dma(s,0,0x100,0x500,4,0x8e01);
    kega_sh2_soc_advance(s,7); assert(f.writes==0);
    kega_sh2_soc_advance(s,8); assert(f.writes==4 && !memcmp(f.memory+0x100,f.memory+0x500,16));
    assert(kega_sh2_soc_read32(s,0x180)==0x110 && kega_sh2_soc_read32(s,0x184)==0x4f0);

    /* DREQ0 cannot consume a partial group; resume only after another full group. */
    reset(s,&f); for(unsigned i=0;i<8;++i) f.fifo[i]=(uint16_t)(0x4100+i);
    f.available=3; dma(s,0,0x20004012,0x600,8,0x4401);
    kega_sh2_soc_dreq(s,0); kega_sh2_soc_advance(s,100); assert(!f.writes && !f.popped);
    f.available=4; kega_sh2_soc_dreq(s,0); kega_sh2_soc_advance(s,107); assert(!f.writes);
    kega_sh2_soc_advance(s,108); assert(f.writes==4 && f.popped==4 && kega_sh2_soc_read32(s,0x188)==4);
    f.available=4; kega_sh2_soc_dreq(s,0); kega_sh2_soc_advance(s,116);
    assert(f.writes==8 && f.popped==8 && (kega_sh2_soc_read32(s,0x18c)&2));
    for(unsigned i=0;i<8;++i) assert(r16(&f,cpu,0x600+i*2)==0x4100+i);
    reset(s,&f); dma(s,1,0x100,0x20004034,1,0x1401);
    assert(!f.writes); kega_sh2_soc_dreq(s,1); kega_sh2_soc_advance(s,2);
    assert(f.writes==1 && f.pwm==0x0102 && (kega_sh2_soc_read32(s,0x19c)&2));

    /* Explicit DREQ event time must not inherit a different CPU's lead. */
    reset(s,&f); dma(s,1,0x100,0x20004034,1,0x1401);
    assert(kega_sh2_get_state(cpu,&native)); native.cycles=1000;
    assert(kega_sh2_set_state(cpu,&native));
    kega_sh2_soc_dreq_at(s,1,2);
    assert(kega_sh2_soc_now(s)==2 && kega_sh2_soc_next_deadline(s,2)==4);
    kega_sh2_soc_advance(s,4); assert(f.writes==1);
    native.cycles=0; assert(kega_sh2_set_state(cpu,&native));

    /* WDT key writes, native-cycle prescaling and interval overflow IRQ. */
    reset(s,&f); kega_sh2_soc_write8(s,0xe3,0x60); kega_sh2_soc_write8(s,0xe4,91);
    kega_sh2_soc_write16(s,0x80,0x5afe); kega_sh2_soc_write16(s,0x80,0xa520);
    assert(f.deadline==4); kega_sh2_soc_advance(s,3);
    assert(kega_sh2_soc_read8(s,0x81)==255 && !f.irq_count);
    kega_sh2_soc_advance(s,4); assert(kega_sh2_soc_read8(s,0x81)==0 && f.irq_count==1 && f.vector==91);
    assert(kega_sh2_soc_read8(s,0x80)&0x80);
    kega_sh2_soc_write16(s,0x80,0xa520); assert(!(kega_sh2_soc_read8(s,0x80)&0x80));

    /* FRT compare A/B, clear-on-A, capture and constant-work large time advance. */
    reset(s,&f); kega_sh2_soc_write8(s,0x60,7); kega_sh2_soc_write8(s,0x67,92);
    kega_sh2_soc_write16(s,0x14,3); kega_sh2_soc_write8(s,0x17,0xf0);
    kega_sh2_soc_write16(s,0x14,5); kega_sh2_soc_write8(s,0x17,0xe0);
    kega_sh2_soc_write8(s,0x10,8); assert(f.deadline==24);
    kega_sh2_soc_advance(s,23); assert(kega_sh2_soc_read16(s,0x12)==2 && !f.irq_count);
    kega_sh2_soc_advance(s,24); assert(kega_sh2_soc_read16(s,0x12)==3 && f.irq_count==1 && f.vector==92);
    kega_sh2_soc_write8(s,0x11,0); assert(!(kega_sh2_soc_read8(s,0x11)&8));
    kega_sh2_soc_capture(s); assert(kega_sh2_soc_read16(s,0x18)==3 && (kega_sh2_soc_read8(s,0x11)&0x80));
    reset(s,&f); kega_sh2_soc_write16(s,0x14,3); kega_sh2_soc_write8(s,0x11,1);
    kega_sh2_soc_advance(s,800000); assert(kega_sh2_soc_read16(s,0x12)==1);

    /* Equal-level DMA0 precedes DMA1 even when its programmed vector is larger. */
    reset(s,&f); kega_sh2_soc_write8(s,0xe2,5);
    kega_sh2_soc_write32(s,0x1a0,100); kega_sh2_soc_write32(s,0x1a8,1);
    dma(s,0,0x100,0x700,1,0x5205); dma(s,1,0x100,0x710,1,0x5205);
    kega_sh2_soc_advance(s,2); assert(f.level==5 && f.vector==100);

    /* FRT7 holds over later DMA5 and simultaneous WDT3. External8 still wins.
     * Acknowledge alone must not clear flags/release lower requests; real ISR
     * MMIO clears each source and RTE exposes the next highest active request. */
    reset(s,&f); assert(kega_sh2_reset(cpu));
    kega_sh2_soc_write8(s,0x60,7); kega_sh2_soc_write8(s,0x67,92);
    kega_sh2_soc_write8(s,0xe2,5); kega_sh2_soc_write32(s,0x1a0,90);
    kega_sh2_soc_write8(s,0xe3,0x30); kega_sh2_soc_write8(s,0xe4,91);
    kega_sh2_soc_write16(s,0x14,1); kega_sh2_soc_write8(s,0x10,8);
    kega_sh2_soc_write16(s,0x80,0x5afc); kega_sh2_soc_write16(s,0x80,0xa520);
    kega_sh2_soc_advance(s,8); assert(f.level==7 && f.vector==92);
    dma(s,0,0x100,0x700,1,0x5205); unsigned published=f.irq_count;
    kega_sh2_soc_advance(s,10); assert(f.level==7 && f.vector==92 && f.irq_count==published);
    KegaSh2SocState pending; assert(kega_sh2_soc_get_state(s,&pending));
    assert((pending.irq_pending&(1u<<KEGA_SH2_SOC_FRT_COMPARE)) &&
           (pending.irq_pending&(1u<<KEGA_SH2_SOC_DMA0)) && (pending.irq_pending&(1u<<KEGA_SH2_SOC_WDT)));
    w32(&f,cpu,92*4,0x900); w32(&f,cpu,90*4,0x800); w32(&f,cpu,91*4,0xa00); w32(&f,cpu,68*4,0xb00); w32(&f,cpu,67*4,0xb00);
    w16(&f,cpu,0x900,0x002b); w16(&f,cpu,0x902,0x0009); /* First ISR deliberately does not clear flag. */
    w16(&f,cpu,0xb00,0x002b); w16(&f,cpu,0xb02,0x0009);
    assert(kega_sh2_get_state(cpu,&native)); native.pc=0xc00; native.sr=0; native.r[15]=0x1f00;
    native.r[0]=0; native.r[1]=0xfffffe11; native.r[2]=0xffffff8c; native.r[3]=0xfffffe80; native.r[4]=0xa520;
    assert(kega_sh2_set_state(cpu,&native)); assert(kega_sh2_set_irq(cpu,8));
    assert(kega_sh2_run(cpu,1)==13); assert(kega_sh2_get_state(cpu,&native));
    assert(native.pc==0xb00 && native.pending_int_irq==7); /* External IRQ did not acknowledge SoC. */
    assert(kega_sh2_set_irq(cpu,7)); assert(kega_sh2_run(cpu,1)==18);
    assert(kega_sh2_get_state(cpu,&native) && native.pc==0xb00 && native.pending_int_irq==7);
    assert(kega_sh2_set_irq(cpu,0)); assert(kega_sh2_run(cpu,1)==18);
    assert(kega_sh2_get_state(cpu,&native) && native.pc==0x900 && native.pending_int_irq==7);
    assert(kega_sh2_run(cpu,1)==18); /* Uncleared FRT level re-enters after RTE. */
    assert(kega_sh2_get_state(cpu,&native) && native.pc==0x900 && native.pending_int_irq==7);
    w16(&f,cpu,0x900,0x2100); w16(&f,cpu,0x902,0x002b); w16(&f,cpu,0x904,0x0009);
    assert(kega_sh2_run(cpu,1)==1); /* Clear FRT flag, retaining DMA/WDT. */
    assert(kega_sh2_get_state(cpu,&native) && native.pending_int_irq==5);
    assert(kega_sh2_run(cpu,1)==18); assert(kega_sh2_get_state(cpu,&native) && native.pc==0x800);
    w16(&f,cpu,0x800,0x2202); w16(&f,cpu,0x802,0x002b); w16(&f,cpu,0x804,0x0009);
    assert(kega_sh2_run(cpu,1)==1); assert(kega_sh2_get_state(cpu,&native) && native.pending_int_irq==3);
    assert(kega_sh2_run(cpu,1)==18); assert(kega_sh2_get_state(cpu,&native) && native.pc==0xa00);
    w16(&f,cpu,0xa00,0x2341); w16(&f,cpu,0xa02,0x002b); w16(&f,cpu,0xa04,0x0009);
    assert(kega_sh2_run(cpu,1)==1); assert(kega_sh2_get_state(cpu,&native) && native.pending_int_irq==0);
    assert(kega_sh2_run(cpu,1)==5); assert(kega_sh2_get_state(cpu,&native) && native.pc==0xc00);
    assert(kega_sh2_reset(cpu));

    /* Linked SCI uses this CPU's TX priority and the peer's RX priority. */
    reset(s,&f); reset(p,&peer); kega_sh2_soc_write8(s,0x60,0x80); kega_sh2_soc_write8(s,0x64,93);
    kega_sh2_soc_write8(p,0x60,0x50); kega_sh2_soc_write8(p,0x63,94);
    kega_sh2_soc_write8(s,2,0xa0); kega_sh2_soc_write8(p,2,0x50);
    kega_sh2_soc_write8(s,3,0x5a); kega_sh2_soc_write8(s,4,4);
    assert(kega_sh2_soc_read8(p,5)==0x5a && (kega_sh2_soc_read8(p,4)&0x40));
    assert(f.level==8 && f.vector==93 && peer.level==5 && peer.vector==94);
    KegaSh2SocState saved, again; assert(kega_sh2_soc_get_state(s,&saved));
    kega_sh2_soc_write8(s,3,0xa6); assert(kega_sh2_soc_set_state(s,&saved));
    assert(kega_sh2_soc_get_state(s,&again) && !memcmp(&saved,&again,sizeof(saved)));
    saved.version=0; assert(!kega_sh2_soc_set_state(s,&saved));
    kega_sh2_soc_destroy(s); kega_sh2_soc_destroy(p); kega_sh2_destroy(cpu); kega_sh2_destroy(peer_cpu);
    puts("SH2 SoC: bounded auto/FIFO/PWM DMA, timers, SCI, division extremes and state passed"); return 0;
}
