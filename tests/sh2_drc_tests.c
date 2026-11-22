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
    uint32_t deny_address;
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
    Fixture *f=u;(void)c;
    return !f->deny_code && (f->deny_address<a || f->deny_address-a>=bytes);
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
    /* BRA-self/NOP must finish exactly the same iteration as the
       interpreter even when the budget ends inside its delay slot. */
    const unsigned idle_budgets[]={1,2,3,4,190,191,192,193,4096};
    for(unsigned i=0;i<sizeof(idle_budgets)/sizeof(idle_budgets[0]);i++) {
        unsigned budget=idle_budgets[i];
        assert(kega_sh2_run(a->cpu,budget)==kega_sh2_run(b->cpu,budget));
        compare(a,b);
    }
    assert(kega_sh2_get_state(b->cpu,&state)); assert(state.r[0]==7);
    /* Unsupported executable region has ordinary host data here. Translation
     * failure must flush registers and spend its budget on reference execution. */
    state.pc=0x04000304u;assert(kega_sh2_set_state(a->cpu,&state));assert(kega_sh2_set_state(b->cpu,&state));
    assert(kega_sh2_run(a->cpu,1)==kega_sh2_run(b->cpu,1));compare(a,b);
    assert(kega_sh2_drc_active(b->cpu));
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
    assert(kega_sh2_drc_active(b->cpu));
    a->deny_code=b->deny_code=false;
    a->fetch_sideeffect=b->fetch_sideeffect=false;
    for(unsigned i=0;i<5;++i) {
        assert(kega_sh2_run(a->cpu,3)==kega_sh2_run(b->cpu,3));
        compare(a,b);
    }
    KegaSh2DrcStats stats;
    kega_sh2_drc_get_stats(&stats);
    assert(stats.fallback_interpreter_calls[0]==1 && stats.jit_cycles[0]>0);

    /* A pure branch can still have a handler-backed delay slot. Keep the
       branch and slot atomic without asking the emitter to fetch the slot. */
    KegaSh2State state;
    assert(kega_sh2_get_state(a->cpu,&state));
    state.pc=BASE+0x500;
    for(unsigned i=0;i<2;++i) {
        Fixture *f=i?b:a;
        put(f,0x500,0xa002); /* BRA 508 */
        put(f,0x502,0x7401); /* ADD #1,R4 (delay) */
        put(f,0x508,0xaffe); /* BRA 508 */
        put(f,0x50a,0x0009);
        f->deny_address=BASE+0x502;
        assert(kega_sh2_set_state(f->cpu,&state));
    }
    assert(kega_sh2_run(a->cpu,1)==kega_sh2_run(b->cpu,1));compare(a,b);
    assert(kega_sh2_drc_active(b->cpu));
    assert(kega_sh2_run(a->cpu,3)==kega_sh2_run(b->cpu,3));compare(a,b);
    kega_sh2_drc_get_stats(&stats);
    assert(stats.fallback_interpreter_calls[0]==2);
    kega_sh2_drc_disable();kega_sh2_destroy(a->cpu);kega_sh2_destroy(b->cpu);kega_sh2_destroy(peer->cpu);
    free(a);free(b);free(peer);
}
static void emit_short_block(void) {
    Fixture *a=malloc(sizeof(*a)),*b=malloc(sizeof(*b)),*peer=malloc(sizeof(*peer));
    assert(a&&b&&peer);setup(a,false);setup(b,false);setup(peer,true);
    /* The final SDRAM word forms a one-instruction block. Its MAC, writeback,
       and dispatcher stubs need more than 128 bytes of x86-64 code. */
    for(unsigned i=0;i<2;++i) {
        Fixture *f=i?b:a;
        put(f,0x3fffe,0x021f); /* MAC.L @R1+,@R2+ */
        w32(f,NULL,BASE+0x810,0x1234);
        w32(f,NULL,BASE+0x814,0xfffffffb);
        KegaSh2State state;
        assert(kega_sh2_get_state(f->cpu,&state));
        state.pc=BASE+0x3fffe;state.r[1]=BASE+0x810;state.r[2]=BASE+0x814;
        assert(kega_sh2_set_state(f->cpu,&state));
    }
    assert(kega_sh2_drc_enable(b->cpu,peer->cpu));
    assert(kega_sh2_run(a->cpu,1)==kega_sh2_run(b->cpu,1));compare(a,b);
    KegaSh2DrcStats stats;
    kega_sh2_drc_get_stats(&stats);
    assert(!stats.fallback_interpreter_calls[0] && stats.jit_calls[0]==1);
    assert(kega_sh2_drc_active(b->cpu));
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
/* Exercise the shared interpreter/DRC data entry points without translating
 * code. ROM words are native numeric uint16 values; callbacks are the oracle. */
extern uint32_t kega_sh2_host_read(struct SH2_ *, uint32_t, unsigned);
typedef struct RomReadFixture {
    const uint16_t *const *pages;
    unsigned reads[3];
    uint32_t address[3];
} RomReadFixture;
static uint16_t rom_word(RomReadFixture *f, uint32_t a) {
    const uint16_t *page=f->pages[(a>>16)&63];
    return page ? page[(a&0xffff)>>1] : 0x6bad;
}
static uint8_t rom_r8(void *u,KegaSh2 *c,uint32_t a) {
    RomReadFixture *f=u;(void)c;++f->reads[0];f->address[0]=a;
    return (uint8_t)(rom_word(f,a)>>((a&1)?0:8));
}
static uint16_t rom_r16(void *u,KegaSh2 *c,uint32_t a) {
    RomReadFixture *f=u;(void)c;++f->reads[1];f->address[1]=a;return rom_word(f,a);
}
static uint32_t rom_r32(void *u,KegaSh2 *c,uint32_t a) {
    RomReadFixture *f=u;(void)c;++f->reads[2];f->address[2]=a;
    return ((uint32_t)rom_word(f,a)<<16)|rom_word(f,a+2);
}
static void rom_w8(void *u,KegaSh2 *c,uint32_t a,uint8_t v) { (void)u;(void)c;(void)a;(void)v;assert(0); }
static void rom_w16(void *u,KegaSh2 *c,uint32_t a,uint16_t v) { (void)u;(void)c;(void)a;(void)v;assert(0); }
static void rom_w32(void *u,KegaSh2 *c,uint32_t a,uint32_t v) { (void)u;(void)c;(void)a;(void)v;assert(0); }
static void rom_read_case(KegaSh2 *reference,KegaSh2 *fast,RomReadFixture *r,RomReadFixture *f,
                          uint32_t address,unsigned width,uint32_t expected,bool direct) {
    unsigned selected=width==8?0:width==16?1:2;
    memset(r->reads,0,sizeof(r->reads));memset(f->reads,0,sizeof(f->reads));
    assert(kega_sh2_host_read(kega_sh2_native(reference),address,width)==expected);
    assert(kega_sh2_host_read(kega_sh2_native(fast),address,width)==expected);
    for(unsigned i=0;i<3;++i) {
        assert(r->reads[i]==(i==selected));
        assert(f->reads[i]==(i==selected && !direct));
    }
    assert(r->address[selected]==address);
    if(!direct)assert(f->address[selected]==address);
}
static void rom_page_reads(void) {
    uint16_t (*words)[0x8000]=calloc(3,sizeof(*words));assert(words);
    words[0][0]=0x81f2;words[0][1]=0x3456;words[0][2]=0x789a;
    words[0][0x7ffe]=0xb1c2;words[0][0x7fff]=0xd3e4;
    words[1][0]=0xf567;words[1][1]=0x89ab;words[1][0x7fff]=0xab12;
    words[2][0]=0xdead;words[2][1]=0xbeef;
    const uint16_t *source[64]={0},*pages[64]={0};
    source[0]=pages[0]=words[0];source[1]=pages[1]=words[1];
    source[2]=words[1]; /* Deliberately handled by the host despite ROM data. */
    source[63]=pages[63]=words[1];
    RomReadFixture r={0},f={0};r.pages=f.pages=source;
    KegaSh2Host host={0};host.user=&r;
    host.read8=rom_r8;host.read16=rom_r16;host.read32=rom_r32;
    host.write8=rom_w8;host.write16=rom_w16;host.write32=rom_w32;
    KegaSh2 *reference=kega_sh2_create(&host,false);assert(reference);
    host.user=&f;host.rom_pages=pages;
    KegaSh2 *fast=kega_sh2_create(&host,true);assert(fast);
    static const struct { uint32_t address,expected;unsigned width;bool direct; } cases[]={
        {0x02000000,0x81,8,true},{0x02000001,0xf2,8,true},
        {0x02000000,0x81f2,16,true},{0x02000001,0x81f2,16,true},
        {0x02000000,0x81f23456,32,true},{0x02000001,0x81f23456,32,true},
        /* Longwords align to two bytes, rather than four, on the ROM bus. */
        {0x02000002,0x3456789a,32,true},{0x02000003,0x3456789a,32,true},
        {0x0200ffff,0xe4,8,true},{0x0200ffff,0xd3e4,16,true},
        {0x0200fffd,0xb1c2d3e4,32,true},
        {0x0200fffe,0xd3e4f567,32,false},{0x0200ffff,0xd3e4f567,32,false},
        {0x02010000,0xf56789ab,32,true},
        {0x02020001,0x67,8,false},{0x02020001,0xf567,16,false},
        {0x02020001,0xf56789ab,32,false},{0x02030000,0x6bad6bad,32,false},
        {0x02400001,0xf2,8,true},{0x22000001,0x81f2,16,true},
        {0x22400003,0x3456789a,32,true},
        {0x023ffffe,0xab1281f2,32,false},{0x223fffff,0xab1281f2,32,false},
        {0x06000001,0xf2,8,false},{0x42000001,0x81f2,16,false}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i)
        rom_read_case(reference,fast,&r,&f,cases[i].address,cases[i].width,cases[i].expected,cases[i].direct);
    /* The live table is reread after bank replacement, including aliases. */
    source[0]=pages[0]=words[2];
    rom_read_case(reference,fast,&r,&f,0x02000001,32,0xdeadbeef,true);
    rom_read_case(reference,fast,&r,&f,0x22400000,16,0xdead,true);
    words[2][0]=0x7654;
    rom_read_case(reference,fast,&r,&f,0x22000000,16,0x7654,true);
    pages[0]=NULL;
    rom_read_case(reference,fast,&r,&f,0x02400001,8,0x54,false);
    rom_read_case(reference,fast,&r,&f,0x02000000,32,0x7654beef,false);
    pages[0]=source[0]=words[0];
    rom_read_case(reference,fast,&r,&f,0x02000000,32,0x81f23456,true);
    kega_sh2_destroy(reference);kega_sh2_destroy(fast);free(words);
}
int main(void) {
    rom_page_reads();
    assert(kega_sh2_drc_cache_capacity()==4u*1024*1024);
    for(unsigned mode=0;mode<8;++mode) parity(mode);
    smc_and_nested();
    cache_pressure();
    emit_short_block();
    denied_code_fetch();
    puts("SH2 DRC: registers/cycles, waits, stop, IRQ/RTE, nested peer, SMC, fallback and cache pressure passed");
    return 0;
}
