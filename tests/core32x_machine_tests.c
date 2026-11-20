#include "core32x/machine.h"
#include "core32x/gpx/bootrom.h"
#ifdef KEGA_SH2_DRC
#include "core32x/sh2/drc.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"machine fixture: %s line %d\n",#c,__LINE__);return 1;} }while(0)
static void put16(uint8_t *p,unsigned a,uint16_t v) {p[a]=(uint8_t)(v>>8);p[a+1]=(uint8_t)v;}
static void put32(uint8_t *p,unsigned a,uint32_t v) {put16(p,a,(uint16_t)(v>>16));put16(p,a+2,(uint16_t)v);}
static void program(uint8_t *p,uint32_t port,uint8_t value)
{
    put32(p,0,0x100);put32(p,4,0x0603f000);
    put16(p,0x100,0xd102);put16(p,0x102,(uint16_t)(0xe000|value));
    put16(p,0x104,0x2101);put16(p,0x106,0xaffe);put16(p,0x108,0x0009);
    put32(p,0x10c,port);
}
static uint32_t peripheral(void *p,unsigned cpu,uint32_t address,unsigned size)
{ (void)p;(void)cpu;(void)size;return address==0xffffff00?0xface:0; }
static uint16_t cart_word(void *data, uint32_t address)
{
    const uint8_t *rom = data;
    return address < 0x400 ? (uint16_t)((rom[address] << 8) | rom[address + 1]) : 0;
}

static int cart_boot_checksum(void)
{
    uint8_t cart[0x400] = {0}, master[2048], slave[1024];
    put16(cart, 0x18e, 1); /* Intentionally different from the computed sum. */
    put32(cart, 0x1a4, sizeof(cart) - 1);
    put32(cart, 0x3d4, 0x200);
    put32(cart, 0x3dc, 4);
    put32(cart, 0x200, 0xfffe8123);
    uint16_t sum = 0;
    for (unsigned i = 0x200; i < sizeof(cart); i += 2)
        sum += cart_word(cart, i);
    builtin_boot_sh2(master, 0, 1);
    builtin_boot_sh2(slave, 1, 1);
    Kega32xMachineHost host = {.userdata = cart, .rom_read16 = cart_word};
    Kega32xMachine *m = kega32x_machine_create(&host, master, sizeof(master), slave, sizeof(slave), 0, 0);
    CHECK(m);
    Kega32xBus *bus = kega32x_machine_bus(m);
    KegaSh2 *cpu = kega32x_machine_cpu(m, 0);
    kega32x_bus_write16(bus, KEGA32X_M68K, 0, 3, 0);
    for (int i = 0; i < 4000 && !bus->s.regs[0x28/2]; ++i)
        kega_sh2_run(cpu, 1);
    CHECK(bus->s.regs[0x28/2] == sum);
    CHECK(kega32x_machine_read(m, 0, 0x06000000, 4) == 0);
    kega32x_bus_write16(bus, KEGA32X_M68K, 0x28, 0, kega32x_machine_cpu_mclk(m, 0));
    kega_sh2_run(cpu, 250);
    CHECK(kega32x_machine_read(m, 0, 0x06000000, 4) == 0xfffe8123);
    CHECK(bus->s.regs[0x28/2] == 0); /* The IPL must not republish after acknowledgement. */
    kega32x_machine_destroy(m);
    return 0;
}
static unsigned run_until_budget(KegaSh2 *cpu,unsigned budget)
{
    unsigned elapsed=0;
    while (elapsed<budget) {
        unsigned step=kega_sh2_run(cpu,budget-elapsed);
        if (!step) break;
        elapsed+=step;
    }
    return elapsed;
}

enum { FRAMEBUFFER_WATCH_OFFSET = 0x10 };
typedef struct {
    Kega32xBus *bus;
    uint64_t time;
    uint32_t before;
    unsigned calls;
} FramebufferWatch;

static void watch_framebuffer(void *data,uint64_t time)
{
    FramebufferWatch *watch=data;
    watch->time=time;
    watch->before=((uint32_t)kega32x_fb_read16(watch->bus,FRAMEBUFFER_WATCH_OFFSET)<<16)|
        kega32x_fb_read16(watch->bus,FRAMEBUFFER_WATCH_OFFSET+2);
    watch->calls++;
}

static int test_framebuffer_ownership(bool drc)
{
    uint8_t mb[2048]={0},sb[1024]={0};
    const uint16_t stores[]={
        0x2100,0x2201,0x2302, /* MOV.B/W/L R0,@R1/R2/R3 (normal) */
        0x2400,0x2501,0x2602, /* MOV.B/W/L R0,@R4/R5/R6 (overwrite) */
        0x7701,0xaffe,0x0009  /* ADD #1,R7; loop */
    };
    const uint16_t owned_words[]={
        0xb234,0x00b2,0xa100,0x00b2,0x12b2,0x12b2,0xa134,0x12b2
    };
    for (unsigned i=0;i<sizeof(stores)/sizeof(stores[0]);i++) {
        put16(mb,0x100+2*i,stores[i]);
        put16(sb,0x100+2*i,stores[i]);
    }
    FramebufferWatch watch={0};
    Kega32xMachineHost host={.userdata=&watch,.render_sync=watch_framebuffer};
    Kega32xMachine *m=kega32x_machine_create(&host,mb,sizeof(mb),sb,sizeof(sb),0,0);
    CHECK(m);
    Kega32xBus *b=kega32x_machine_bus(m);
    watch.bus=b;
    kega32x_bus_write16(b,KEGA32X_M68K,0,3,0);
    watch.calls=0; /* Begin after the adapter-enable notification. */
#ifdef KEGA_SH2_DRC
    if(drc) CHECK(kega_sh2_drc_enable(kega32x_machine_cpu(m,KEGA32X_MASTER),
                                     kega32x_machine_cpu(m,KEGA32X_SLAVE)));
    else kega_sh2_drc_disable();
#else
    CHECK(!drc);
#endif
    for (unsigned c=KEGA32X_MASTER;c<=KEGA32X_SLAVE;c++) {
        KegaSh2 *cpu=kega32x_machine_cpu(m,c);
        for (unsigned own=0;own<2;own++) {
            KegaSh2State state;
            kega32x_bus_write16(b,KEGA32X_M68K,0,own?0x8003:3,kega32x_machine_cpu_mclk(m,c));
            for (unsigned i=0;i<sizeof(owned_words)/sizeof(owned_words[0]);i++)
                kega32x_fb_write16(b,2*i,0x1234);

            CHECK(kega_sh2_get_state(cpu,&state));
            state.pc=0x100;state.delay=0;
            state.r[0]=0xa10000b2;state.r[1]=0x24000000;state.r[2]=0x24000002;
            state.r[3]=0x24000004;state.r[4]=0x24020009;state.r[5]=0x2402000a;
            state.r[6]=0x2402000c;state.r[7]=0;
            CHECK(kega_sh2_set_state(cpu,&state));
            CHECK(run_until_budget(cpu,32)>=32);
            CHECK(kega_sh2_get_state(cpu,&state) && state.r[7]==1);
            for (unsigned i=0;i<sizeof(owned_words)/sizeof(owned_words[0]);i++)
                CHECK(kega32x_fb_read16(b,2*i)==(own?owned_words[i]:0x1234));
        }
    }

    /* The writable bank is 1. A watched bank 0 cannot observe these stores. */
    const uint32_t address=0x24000000+FRAMEBUFFER_WATCH_OFFSET;
    const uint64_t time=kega32x_machine_cpu_mclk(m,KEGA32X_SLAVE);
    CHECK(time>0);
    b->video_watch_banks=1;
    kega32x_machine_write(m,KEGA32X_SLAVE,address,0x43218765,4);
    CHECK(watch.calls==0);
    b->video_watch_banks=2;
    kega32x_machine_write(m,KEGA32X_SLAVE,address,0xabcdfedc,4);
    CHECK(watch.calls==1 && watch.time==time && watch.before==0x43218765);
    CHECK(kega32x_fb_read16(b,FRAMEBUFFER_WATCH_OFFSET)==0xabcd);
    CHECK(kega32x_fb_read16(b,FRAMEBUFFER_WATCH_OFFSET+2)==0xfedc);
#ifdef KEGA_SH2_DRC
    if(drc) {
        KegaSh2DrcStats stats;
        kega_sh2_drc_get_stats(&stats);
        CHECK(stats.jit_calls[KEGA32X_MASTER]>0 && stats.jit_calls[KEGA32X_SLAVE]>0);
        kega_sh2_drc_disable();
    }
#endif
    kega32x_machine_destroy(m);
    return 0;
}

static int test_bus_access_timing(bool drc)
{
    uint8_t mb[2048]={0},sb[1024]={0};
    enum { SYSTEM_REGS, FRAMEBUFFER, SDRAM, LOCAL_RAM, REGION_COUNT };
    const uint32_t sources[]={0x20004020,0x24000000,0x06001000,0xc0000100};
    unsigned read_cost[REGION_COUNT][3],write_cost[REGION_COUNT][3];
    program(mb,0x20004026,0x12);
    program(sb,0x20004026,0x34);
    put32(sb,0x110,0x180); /* IRL8 vector */
    put16(sb,0x180,0x6211); /* MOV.W @R1,R2 (framebuffer count) */
    put16(sb,0x182,0x7201); /* ADD #1,R2 */
    put16(sb,0x184,0x2121); /* MOV.W R2,@R1 */
    put16(sb,0x186,0x2431); /* MOV.W R3,@R4 (clear CMD) */
    put16(sb,0x188,0x002b); /* RTE */
    put16(sb,0x18a,0x0009);
    for (unsigned i=0;i<3;i++) {
        /* MOV.B/W/L @R1,R2 and MOV.B/W/L R2,@R3 */
        put16(sb,0x1a0+8*i,(uint16_t)(0x6210+i));
        put16(sb,0x1a2+8*i,(uint16_t)(0x2320+i));
        put16(sb,0x1a4+8*i,0xaffe);
        put16(sb,0x1a6+8*i,0x0009);
    }
    Kega32xMachineHost host={0};
    Kega32xMachine *m=kega32x_machine_create(&host,mb,sizeof(mb),sb,sizeof(sb),0,0);
    CHECK(m);
    Kega32xBus *b=kega32x_machine_bus(m);
    KegaSh2 *cpu=kega32x_machine_cpu(m,KEGA32X_SLAVE);
    kega32x_bus_write16(b,KEGA32X_M68K,0,0x8003,0); /* SH-2 owns the framebuffer. */
#ifdef KEGA_SH2_DRC
    if(drc) CHECK(kega_sh2_drc_enable(kega32x_machine_cpu(m,KEGA32X_MASTER),cpu));
    else kega_sh2_drc_disable();
#else
    CHECK(!drc);
#endif
    KegaSh2State state;
    const uint32_t stack_tops[]={0xc0000200,0x0603f000};
    const unsigned irq_read_cycles[]={20,26};
    kega32x_bus_write8(b,KEGA32X_SLAVE,1,2,0);
    for (unsigned i=0;i<2;i++) {
        const uint32_t stack=stack_tops[i];
        CHECK(kega_sh2_get_state(cpu,&state));
        state.pc=0x106;state.sr=1;state.r[1]=sources[FRAMEBUFFER];
        state.r[3]=0;state.r[4]=0x2000401a;state.r[15]=stack;
        CHECK(kega_sh2_set_state(cpu,&state));
        kega32x_fb_write16(b,0,0);
        kega32x_bus_write16(b,KEGA32X_M68K,2,2,0);

        /* The SDRAM pushes add six clocks over a local stack. The count and
           CMD latch remain untouched through entry and the framebuffer read. */
        CHECK(run_until_budget(cpu,irq_read_cycles[i])==irq_read_cycles[i]);
        CHECK(kega32x_machine_read(m,KEGA32X_SLAVE,stack-8,4)==0x106);
        CHECK(kega32x_machine_read(m,KEGA32X_SLAVE,stack-4,4)==1);
        CHECK(kega32x_fb_read16(b,0)==0 && b->s.regs[1]==2);
        CHECK(run_until_budget(cpu,2)==2);
        CHECK(kega32x_fb_read16(b,0)==1 && b->s.regs[1]==2);
        CHECK(kega_sh2_run(cpu,1)>1 && b->s.regs[1]==0);
    }

    for (unsigned region=0;region<REGION_COUNT;region++) {
        kega32x_machine_write(m,KEGA32X_SLAVE,sources[region],0xa1b2c3d4,4);
        for (unsigned i=0;i<3;i++) {
            CHECK(kega_sh2_get_state(cpu,&state));
            state.pc=0x1a0+8*i;state.delay=0;
            state.r[1]=sources[region];state.r[3]=sources[region]+4;
            CHECK(kega_sh2_set_state(cpu,&state));
            read_cost[region][i]=kega_sh2_run(cpu,1);
            write_cost[region][i]=kega_sh2_run(cpu,1);
            const unsigned size=1u<<i;
            CHECK(kega32x_machine_read(m,KEGA32X_SLAVE,state.r[3],size)==
                  kega32x_machine_read(m,KEGA32X_SLAVE,state.r[1],size));
        }
        CHECK(read_cost[region][0]==read_cost[region][1]);
        CHECK(write_cost[region][0]==write_cost[region][1]);
        if (region<=FRAMEBUFFER)
            CHECK(read_cost[region][2]==2*read_cost[region][1]);
        if (region==SYSTEM_REGS) {
            CHECK(write_cost[region][2]==2*write_cost[region][1]);
        } else {
            /* Ordinary stores keep their one-cycle issue cost at every width. */
            CHECK(write_cost[region][1]==1 && write_cost[region][2]==1);
        }
    }
    CHECK(read_cost[FRAMEBUFFER][1]>read_cost[SYSTEM_REGS][1]);
    CHECK(read_cost[FRAMEBUFFER][1]>write_cost[FRAMEBUFFER][1]);
#ifdef KEGA_SH2_DRC
    if(drc) {
        KegaSh2DrcStats stats;
        kega_sh2_drc_get_stats(&stats);
        CHECK(kega_sh2_drc_active(cpu) && stats.jit_calls[KEGA32X_SLAVE]>0);
        kega_sh2_drc_disable();
    }
#endif
    kega32x_machine_destroy(m);
    return 0;
}

int main(void)
{
    CHECK(cart_boot_checksum() == 0);
    CHECK(test_framebuffer_ownership(false)==0);
    CHECK(test_bus_access_timing(false)==0);
#ifdef KEGA_SH2_DRC
    CHECK(test_framebuffer_ownership(true)==0);
    CHECK(test_bus_access_timing(true)==0);
#endif
    uint8_t mb[2048]={0},sb[1024]={0};
    Kega32xMachineHost h={0};Kega32xMachine *m;Kega32xBus *b;
    program(mb,0x20004020,0x42);program(sb,0x20004022,0x24);
    /* Read one unaligned address at the end of each plain memory mirror with the
       three SH-2 load widths, including byte/word sign extension. */
    put16(mb,0x140,0x6010); /* MOV.B @R1,R0 */
    put16(mb,0x142,0x6211); /* MOV.W @R1,R2 */
    put16(mb,0x144,0x6312); /* MOV.L @R1,R3 */
    put16(mb,0x146,0xaffe); put16(mb,0x148,0x0009);
    put16(sb,0x160,0x2122); /* MOV.L R2,@R1 */
    put16(sb,0x162,0xaffe); put16(sb,0x164,0x0009);
    h.peripheral_read=peripheral;
    CHECK(!kega32x_machine_create(&h,mb,2047,sb,sizeof(sb),0,0));
    m=kega32x_machine_create(&h,mb,sizeof(mb),sb,sizeof(sb),0,0);CHECK(m);
    b=kega32x_machine_bus(m);
    kega32x_machine_run_until(m,100);
    CHECK(kega_sh2_cycles(kega32x_machine_cpu(m,0))==0);
    kega32x_bus_write16(b,KEGA32X_M68K,0,3,100);
    kega32x_machine_run_until(m,1000);
    CHECK(b->s.regs[0x20/2]==0x42 && b->s.regs[0x22/2]==0x24);
    CHECK(kega32x_machine_cpu_mclk(m,0)>=998 && kega32x_machine_cpu_mclk(m,1)>=998);
    kega32x_machine_write(m,0,0x26000100,0x12345678,4);
    CHECK(kega32x_machine_read(m,1,0x06000100,4)==0x12345678);
    CHECK(kega32x_machine_read(m,1,0x06000101,1)==0x34);
    kega32x_machine_write(m,0,0xc0000100,0xbeef,2);
    CHECK(kega32x_machine_read(m,0,0xc0000100,2)==0xbeef);
    CHECK(kega32x_machine_read(m,1,0xc0000100,2)==0);
    CHECK(kega32x_machine_read(m,0,0xffffff00,4)==0xface);
    kega32x_machine_write(m,0,0x46000100,0,4);
    CHECK(kega32x_machine_read(m,0,0x06000100,4)==0x12345678);
    KegaSh2 *master=kega32x_machine_cpu(m,0);
    KegaSh2State state;
    CHECK(kega_sh2_get_state(master,&state));
    const uint32_t read_mirror[]={0x27ffffff,0xdfffffff};
    const uint32_t write_address[]={0x0603fffc,0xc0000ffc};
    for(unsigned i=0;i<2;i++) {
        state.pc=0x140;state.delay=0;state.r[1]=read_mirror[i];
        state.r[0]=state.r[2]=state.r[3]=0;
        CHECK(kega_sh2_set_state(master,&state));
        kega32x_machine_write(m,0,write_address[i],0x1234abcd,4);
        CHECK(kega_sh2_run(master,32)>=32);
        CHECK(kega_sh2_get_state(master,&state));
        CHECK(state.r[0]==0xffffffcd && state.r[2]==0xffffabcd && state.r[3]==0x1234abcd);
    }
    /* Warm the master's translated idle loop. A store from the other SH-2
       through an unaligned SDRAM mirror replaces both instructions. */
    KegaSh2 *slave=kega32x_machine_cpu(m,1);
    CHECK(kega_sh2_get_state(slave,&state));
    state.pc=0x160;state.delay=0;state.r[1]=0x27fc1003;state.r[2]=0xaffe7701;
    CHECK(kega_sh2_set_state(slave,&state));
    kega32x_machine_write(m,0,0x06001000,0xaffe0009,4);
    CHECK(kega_sh2_get_state(master,&state));
    state.pc=0x06001000;state.delay=0;state.r[7]=0;
    CHECK(kega_sh2_set_state(master,&state));
    for(int i=0;i<4;i++) CHECK(kega_sh2_run(master,12)==12);
    CHECK(kega_sh2_run(slave,1)>=1);
    CHECK(kega_sh2_run(master,12)==12);
    CHECK(kega_sh2_get_state(master,&state));
    CHECK(state.r[7]==4); /* ADD #1,R7 in the new delay slot */
    kega32x_machine_destroy(m);
    puts("32X assembly: two synthetic BIOS programs, held reset, shared/private memory passed");
    return 0;
}
