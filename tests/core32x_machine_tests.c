#include "core32x/machine.h"
#include "core32x/gpx/bootrom.h"
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
int main(void)
{
    CHECK(cart_boot_checksum() == 0);
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
