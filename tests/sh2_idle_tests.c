#ifdef NDEBUG
#undef NDEBUG
#endif
#include <core32x/sh2/sh2_interpreter.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define BASE 0x06000000u
#define RAM 0x4000u
typedef struct IdleFixture { unsigned char mem[RAM]; unsigned fetches, ack, irq_on_delay; bool allow; KegaSh2 *cpu; } IdleFixture;
static uint8_t rb(void *u,KegaSh2 *c,uint32_t a) { (void)c;return ((IdleFixture *)u)->mem[a&(RAM-1)]; }
static uint16_t rw(void *u,KegaSh2 *c,uint32_t a) { return (uint16_t)((rb(u,c,a)<<8)|rb(u,c,a+1)); }
static uint32_t rl(void *u,KegaSh2 *c,uint32_t a) { return ((uint32_t)rw(u,c,a)<<16)|rw(u,c,a+2); }
static void wb(void *u,KegaSh2 *c,uint32_t a,uint8_t v) { (void)c;((IdleFixture *)u)->mem[a&(RAM-1)]=v; }
static void ww(void *u,KegaSh2 *c,uint32_t a,uint16_t v) { wb(u,c,a,(uint8_t)(v>>8));wb(u,c,a+1,(uint8_t)v); }
static void wl(void *u,KegaSh2 *c,uint32_t a,uint32_t v) { ww(u,c,a,(uint16_t)(v>>16));ww(u,c,a+2,(uint16_t)v); }
static uint16_t fetch(void *u,KegaSh2 *c,uint32_t a) {
    IdleFixture *f=u;++f->fetches;
    if(a==BASE+0x102 && f->irq_on_delay) { f->irq_on_delay=0;assert(kega_sh2_set_irq(c,6)); }
    return rw(u,c,a);
}
static bool pure(void *u,KegaSh2 *c,uint32_t a,size_t bytes) { (void)c;return ((IdleFixture *)u)->allow && a==BASE+0x100 && bytes==4; }
static unsigned ack(void *u,KegaSh2 *c,unsigned level) { IdleFixture *f=u;++f->ack;assert(level==6);assert(kega_sh2_set_irq(c,0));return 67; }
static void setup(IdleFixture *f,bool fast,unsigned mode) {
    memset(f,0,sizeof(*f));f->allow=!(mode&8);
    KegaSh2Host h={0};h.user=f;h.read8=rb;h.read16=rw;h.read32=rl;h.write8=wb;h.write16=ww;h.write32=wl;h.fetch16=fetch;h.irq_ack=ack;
    if(!(mode&16)) h.can_idle_fetch=pure;
    f->cpu=kega_sh2_create(&h,false);assert(f->cpu);
    wl(f,NULL,0,BASE+0x100);wl(f,NULL,4,BASE+0x1000);
    ww(f,NULL,BASE+0x100,0xaffe);ww(f,NULL,BASE+0x102,(mode&4)?0x7001:0x0009);
    wl(f,NULL,BASE+0x2000+67*4,BASE+0x400);wl(f,NULL,BASE+0x2000+80*4,BASE+0x400);
    ww(f,NULL,BASE+0x400,0x7101);ww(f,NULL,BASE+0x402,0x002b);ww(f,NULL,BASE+0x404,0x0009);
    assert(kega_sh2_reset(f->cpu));
    KegaSh2State s;assert(kega_sh2_get_state(f->cpu,&s));s.vbr=BASE+0x2000;s.sr=(mode&2)?0xf0:0;
    if(mode&32) s.delay=BASE+0x102;
    assert(kega_sh2_set_state(f->cpu,&s));assert(kega_sh2_set_idle_fastpath(f->cpu,fast));
    if(mode&1) assert(kega_sh2_set_irq(f->cpu,6));
    if(mode&64) assert(kega_sh2_set_internal_irq(f->cpu,7,80));
    if(mode&128) f->irq_on_delay=1;
}
int main(void) {
    static const unsigned modes[]={0,1,2,3,4,5,8,16,32,33,64,66,128|8,128|16};
    for(unsigned m=0;m<sizeof(modes)/sizeof(modes[0]);++m) for(unsigned budget=1;budget<=17;++budget) {
        IdleFixture a,b;setup(&a,false,modes[m]);setup(&b,true,modes[m]);
        unsigned done_a=kega_sh2_run(a.cpu,budget),done_b=kega_sh2_run(b.cpu,budget);assert(done_a==done_b);
        KegaSh2State x,y;assert(kega_sh2_get_state(a.cpu,&x));assert(kega_sh2_get_state(b.cpu,&y));
        if(memcmp(&x,&y,sizeof(x))) { fprintf(stderr,"idle mismatch mode %u budget %u PC %x/%x ppc %x/%x ea %x/%x cycles %llu/%llu\n",modes[m],budget,x.pc,y.pc,x.ppc,y.ppc,x.ea,y.ea,(unsigned long long)x.cycles,(unsigned long long)y.cycles);abort(); }
        assert(!memcmp(a.mem,b.mem,RAM) && a.ack==b.ack);
        if(modes[m]&(8|16)) assert(a.fetches==b.fetches); /* denied span: no extra peek */
        if(modes[m]==0) {assert(done_b==((budget+2)/3)*3);assert(b.fetches==2);assert(x.pc==BASE+0x100 && x.ppc==BASE+0x102 && x.ea==BASE+0x100 && !x.delay);}
        kega_sh2_destroy(a.cpu);kega_sh2_destroy(b.cpu);
    }
    puts("SH2 idle: full-state parity budgets 1..17, delay, changed code, IRQ and denied/MMIO peek passed");
    return 0;
}
