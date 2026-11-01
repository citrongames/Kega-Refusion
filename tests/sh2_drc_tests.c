#ifdef NDEBUG
#undef NDEBUG
#endif
#include <core32x/sh2/drc.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BASE 0x06000000u
#define MEM_SIZE 0x40000u
typedef struct Fixture {
    unsigned char mem[MEM_SIZE];
    KegaSh2 *cpu, *peer;
    unsigned writes, wait, stop, irq, nested, fetches;
    bool deny_code, fetch_sideeffect;
    uint64_t last_write;
} Fixture;
static unsigned off(uint32_t a) { return a & (MEM_SIZE-1); }
static uint8_t r8(void *u,KegaSh2 *c,uint32_t a) { (void)c; return ((Fixture *)u)->mem[off(a)]; }
static uint16_t r16(void *u,KegaSh2 *c,uint32_t a) { return (uint16_t)((r8(u,c,a)<<8)|r8(u,c,a+1)); }
static uint32_t r32(void *u,KegaSh2 *c,uint32_t a) { return ((uint32_t)r16(u,c,a)<<16)|r16(u,c,a+2); }
static void w8(void *u,KegaSh2 *c,uint32_t a,uint8_t v) { (void)c; ((Fixture *)u)->mem[off(a)]=v; }
static void w16(void *u,KegaSh2 *c,uint32_t a,uint16_t v) { w8(u,c,a,(uint8_t)(v>>8)); w8(u,c,a+1,(uint8_t)v); }
static void w32(void *u,KegaSh2 *c,uint32_t a,uint32_t v) {
    Fixture *f=u; w16(u,c,a,(uint16_t)(v>>16));w16(u,c,a+2,(uint16_t)v);
    if(a==BASE+0x800) {
        ++f->writes; f->last_write=kega_sh2_cycles(c);
        if(f->wait) { assert(kega_sh2_add_wait_cycles(c,f->wait)); f->wait=0; }
        if(f->stop) { kega_sh2_end_run(c); f->stop=0; }
        if(f->irq) { assert(kega_sh2_set_irq(c,6)); f->irq=0; }
        if(f->nested) { assert(kega_sh2_run(f->peer,5)>=5); f->nested=0; }
    }
}
static unsigned ack(void *u,KegaSh2 *c,unsigned level) { (void)u; assert(level==6); assert(kega_sh2_set_irq(c,0)); return 67; }
static void put(Fixture *f,unsigned a,unsigned op) { w16(f,NULL,BASE+a,(uint16_t)op); }
static uint16_t code_fetch(void *u,KegaSh2 *c,uint32_t address) {
    Fixture *f=u;++f->fetches;
    if(f->fetch_sideeffect)++f->mem[0x900];
    return r16(u,c,address);
}
static bool code_is_pure(void *u,KegaSh2 *c,uint32_t a,size_t bytes) {
    (void)c;(void)a;(void)bytes;return !((Fixture *)u)->deny_code;
}
static void setup(Fixture *f,bool slave) {
    memset(f,0,sizeof(*f));
    KegaSh2Host host={f,r8,r16,r32,w8,w16,w32,ack,NULL,code_fetch,code_is_pure};
    f->cpu=kega_sh2_create(&host,slave); assert(f->cpu);
    w32(f,NULL,0,BASE+0x100); w32(f,NULL,4,BASE+0x1000);
    w32(f,NULL,BASE+0x2000+67*4,BASE+0x400);
    put(f,0x100,0xe005); /* MOV #5,R0 */
    put(f,0x102,0xe100); /* MOV #0,R1 */
    put(f,0x104,0x310c); /* ADD R0,R1 */
    put(f,0x106,0x4010); /* DT R0 */
    put(f,0x108,0x8bfc); /* BF 104 */
    put(f,0x10a,0x2212); /* MOV.L R1,@R2 */
    put(f,0x10c,0x6322); /* MOV.L @R2,R3 */
    put(f,0x10e,0xa001); /* BRA 114 */
    put(f,0x110,0x7302); /* ADD #2,R3 (delay) */
    put(f,0x112,0xe37f); /* skipped */
    put(f,0x114,0xafff); /* BRA 116 */
    put(f,0x116,0x0009); /* NOP */
    put(f,0x118,0xaffd); /* BRA 116 */
    put(f,0x11a,0x0009);
    put(f,0x400,0x7401); /* ADD #1,R4 */
    put(f,0x402,0x002b); /* RTE */
    put(f,0x404,0x0009);
    assert(kega_sh2_reset(f->cpu));
    KegaSh2State s; assert(kega_sh2_get_state(f->cpu,&s));
    s.r[2]=BASE+0x800; s.vbr=BASE+0x2000; s.sr=0; assert(kega_sh2_set_state(f->cpu,&s));
}
static void compare(Fixture *a,Fixture *b) {
    KegaSh2State x,y; assert(kega_sh2_get_state(a->cpu,&x)); assert(kega_sh2_get_state(b->cpu,&y));
    if(x.pc!=y.pc || x.sr!=y.sr || x.cycles!=y.cycles || memcmp(x.r,y.r,sizeof(x.r))) {
        fprintf(stderr,"parity: PC %08x/%08x SR %x/%x cycles %llu/%llu R0 %x/%x R1 %x/%x R3 %x/%x\n",x.pc,y.pc,x.sr,y.sr,(unsigned long long)x.cycles,(unsigned long long)y.cycles,x.r[0],y.r[0],x.r[1],y.r[1],x.r[3],y.r[3]);
        abort();
    }
    assert(x.pr==y.pr && x.gbr==y.gbr && x.vbr==y.vbr && x.mach==y.mach && x.macl==y.macl);
    assert(!memcmp(a->mem,b->mem,sizeof(a->mem)));
}
static void parity(unsigned mode) {
    Fixture *a=malloc(sizeof(*a)),*b=malloc(sizeof(*b)),*peer=malloc(sizeof(*peer));
    assert(a&&b&&peer);setup(a,false);setup(b,false);setup(peer,true);
    b->peer=peer->cpu;
    assert(kega_sh2_drc_enable(b->cpu,peer->cpu));
    assert(kega_sh2_drc_active(b->cpu));
    a->wait=b->wait=(mode&1)?7:0; a->stop=b->stop=(mode&2)!=0; a->irq=b->irq=(mode&4)!=0;
    for(unsigned i=0;i<60;++i) {
        unsigned budget=1+i%7;
        unsigned x=kega_sh2_run(a->cpu,budget),y=kega_sh2_run(b->cpu,budget);
        if(x!=y) {fprintf(stderr,"budget %u used %u/%u mode %u step %u\n",budget,x,y,mode,i);abort();}
        compare(a,b); assert(kega_sh2_drc_active(b->cpu));
    }
    assert(a->writes && a->last_write==b->last_write);
    KegaSh2State s; assert(kega_sh2_get_state(b->cpu,&s));
    assert(s.r[1]==15 && s.r[3]==17 && s.r[4]==((mode&4)?1:0));
    KegaSh2DrcStats stats;KegaSh2DrcBudgetStats budgets;
    kega_sh2_drc_get_stats(&stats);kega_sh2_drc_get_budget_stats(&budgets);
    assert(stats.jit_calls[0]==60 && !stats.nested_interpreter_calls[0] && !stats.fallback_interpreter_calls[0]);
    assert(stats.jit_cycles[0]==kega_sh2_cycles(b->cpu));
    assert(budgets.calls[0]==60 && budgets.min_budget[0]==1 && budgets.max_budget[0]==7);
    uint64_t samples=0;for(unsigned j=0;j<10;++j)samples+=budgets.histogram[0][j];assert(samples==60);
    kega_sh2_drc_disable(); kega_sh2_destroy(a->cpu);kega_sh2_destroy(b->cpu);kega_sh2_destroy(peer->cpu);
    free(a);free(b);free(peer);
}
static void smc_and_nested(void) {
    Fixture *a=malloc(sizeof(*a)),*b=malloc(sizeof(*b)),*peer=malloc(sizeof(*peer)),*peer_ref=malloc(sizeof(*peer_ref));
    assert(a&&b&&peer&&peer_ref);setup(a,false);setup(b,false);setup(peer,true);setup(peer_ref,true);
    a->peer=peer_ref->cpu; b->peer=peer->cpu; a->nested=b->nested=1;
    assert(kega_sh2_drc_enable(b->cpu,peer->cpu));
    for(unsigned i=0;i<50;++i) { assert(kega_sh2_run(a->cpu,5)==kega_sh2_run(b->cpu,5)); compare(a,b); }
    compare(peer_ref,peer);
    /* External write-through alias modifies already translated code. */
    kega_sh2_drc_invalidate(b->cpu,0x26000116u,2);
    put(a,0x116,0x7503); put(b,0x116,0x7503); /* ADD #3,R5 */
    for(unsigned i=0;i<20;++i) { assert(kega_sh2_run(a->cpu,3)==kega_sh2_run(b->cpu,3)); compare(a,b); }
    KegaSh2State state; assert(kega_sh2_get_state(b->cpu,&state)); assert(state.r[5]>0);
    assert(kega_sh2_drc_active(b->cpu));
    /* A CPU store overwrites a following instruction in its own live block. */
    assert(kega_sh2_get_state(a->cpu,&state));state.pc=BASE+0x300;state.r[1]=0xe0070009u;state.r[2]=BASE+0x304;
    assert(kega_sh2_set_state(a->cpu,&state)); assert(kega_sh2_set_state(b->cpu,&state));
    for(unsigned j=0;j<2;++j) {Fixture *f=j?b:a;put(f,0x300,0x2212);put(f,0x302,0x0009);put(f,0x304,0xe001);put(f,0x306,0x0009);put(f,0x308,0xaffe);put(f,0x30a,0x0009);}
    for(unsigned i=0;i<10;++i) {
        unsigned x=kega_sh2_run(a->cpu,1),y=kega_sh2_run(b->cpu,1);
        assert(x==y); compare(a,b);
    }
    assert(kega_sh2_get_state(b->cpu,&state)); assert(state.r[0]==7);
    /* Unsupported executable region has ordinary host data here. Translation
     * failure must flush registers and spend its budget on reference execution. */
    state.pc=0x04000304u;assert(kega_sh2_set_state(a->cpu,&state));assert(kega_sh2_set_state(b->cpu,&state));
    assert(kega_sh2_run(a->cpu,1)==kega_sh2_run(b->cpu,1));compare(a,b);
    assert(!kega_sh2_drc_active(b->cpu));
    kega_sh2_drc_disable();
    kega_sh2_destroy(a->cpu);kega_sh2_destroy(b->cpu);kega_sh2_destroy(peer->cpu);kega_sh2_destroy(peer_ref->cpu);
    free(a);free(b);free(peer);free(peer_ref);
}
static void denied_code_fetch(void) {
    Fixture *a=malloc(sizeof(*a)),*b=malloc(sizeof(*b)),*peer=malloc(sizeof(*peer));
    assert(a&&b&&peer);setup(a,false);setup(b,false);setup(peer,true);
    a->deny_code=b->deny_code=true;a->fetch_sideeffect=b->fetch_sideeffect=true;
    assert(kega_sh2_drc_enable(b->cpu,peer->cpu));
    assert(kega_sh2_run(a->cpu,1)==kega_sh2_run(b->cpu,1));compare(a,b);
    assert(a->fetches==1 && b->fetches==1 && b->mem[0x900]==1);
    assert(!kega_sh2_drc_active(b->cpu));
    kega_sh2_drc_disable();kega_sh2_destroy(a->cpu);kega_sh2_destroy(b->cpu);kega_sh2_destroy(peer->cpu);
    free(a);free(b);free(peer);
}
static void cache_pressure(void) {
    Fixture *a=malloc(sizeof(*a)),*b=malloc(sizeof(*b)),*peer=malloc(sizeof(*peer));
    assert(a&&b&&peer);setup(a,false);setup(b,false);setup(peer,true);
    for(unsigned j=0;j<2;++j) {
        Fixture *f=j?b:a;
        for(unsigned i=0x100;i<0x3fff8;i+=2) put(f,i,0x7001); /* ADD #1,R0 */
        put(f,0x3fff8,0x4d2b); /* JMP @R13 */ put(f,0x3fffa,0x0009);
        KegaSh2State state;assert(kega_sh2_get_state(f->cpu,&state));
        state.r[13]=BASE+0x100;assert(kega_sh2_set_state(f->cpu,&state));
    }
    assert(kega_sh2_drc_enable(b->cpu,peer->cpu));
    for(unsigned i=0;i<1100;++i) {
        assert(kega_sh2_run(a->cpu,512)==kega_sh2_run(b->cpu,512));
        assert(kega_sh2_drc_active(b->cpu));
    }
    compare(a,b);
    kega_sh2_drc_disable();kega_sh2_destroy(a->cpu);kega_sh2_destroy(b->cpu);kega_sh2_destroy(peer->cpu);
    free(a);free(b);free(peer);
}
int main(void) {
    assert(kega_sh2_drc_cache_capacity()==4u*1024*1024);
    for(unsigned mode=0;mode<8;++mode) parity(mode);
    smc_and_nested();
    cache_pressure();
    denied_code_fetch();
    puts("SH2 DRC: registers/cycles, waits, stop, IRQ/RTE, nested peer, SMC, fallback and cache pressure passed");
    return 0;
}
