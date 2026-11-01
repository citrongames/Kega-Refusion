/* Original synthetic 32X machine/state fixture; no ROM/BIOS files needed. */
#include "core32x/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"32X state fixture: %s line %d\n",#c,__LINE__);return 1;} }while(0)
static void be16(uint8_t *p,unsigned a,uint16_t v) {p[a]=(uint8_t)(v>>8);p[a+1]=(uint8_t)v;}
static void be32(uint8_t *p,unsigned a,uint32_t v) {be16(p,a,(uint16_t)(v>>16));be16(p,a+2,(uint16_t)v);}
static void program(uint8_t *p,uint32_t address)
{
    be32(p,0,0x100);be32(p,4,0x0603f000);
    be16(p,0x100,0xd102);be16(p,0x102,0xe000);be16(p,0x104,0x7001);
    be16(p,0x106,0x2102);be16(p,0x108,0xaffc);be16(p,0x10a,0x0009);
    be32(p,0x10c,address);
}
typedef struct Event {uint64_t t;int32_t l,r;} Event;
typedef struct Trace {Event events[128];unsigned count;} Trace;
static void audio(void *user,uint64_t t,int32_t l,int32_t r)
{
    Trace *trace=user;
    if(trace->count<128)trace->events[trace->count++]=(Event){t,l,r};
}
static void future(Kega32xMachine *m)
{
    Kega32xBus *b=kega32x_machine_bus(m);unsigned i;
    kega32x_bus_write16(b,KEGA32X_M68K,0x12,0x4444,1000);
    for(i=0;i<8;i++) {
        const uint64_t t=1040+i*43;
        kega32x_machine_run_until(m,t);
        kega32x_bus_write16(b,KEGA32X_M68K,0x34,(uint16_t)(3+i),t);
        kega32x_bus_write16(b,KEGA32X_M68K,0x36,(uint16_t)(10-i),t);
    }
    kega32x_bus_blank(b,1,1,1400);kega32x_machine_run_until(m,1600);
}
static int decode_rejects(Kega32xMachineState *dest,const uint8_t *wire,size_t size,
                          const Kega32xMachineState *before)
{
    return !kega32x_state_decode(dest,wire,size) && !memcmp(dest,before,sizeof(*dest));
}
int main(void)
{
    uint8_t master[2048]={0},slave[1024]={0};
    Kega32xMachineHost host={0};Kega32xMachine *m;Kega32xBus *bus;
    Kega32xMachineState *saved,*decoded,*before;uint8_t *wire,*other,*expected;
    const size_t bytes=kega32x_state_encoded_size();
    Trace actual={0},baseline;unsigned i;
    saved=malloc(sizeof(*saved));decoded=malloc(sizeof(*decoded));before=malloc(sizeof(*before));
    wire=malloc(bytes);other=malloc(bytes);expected=malloc(bytes);
    CHECK(saved && decoded && before && wire && other && expected && bytes==535743);
    program(master,0x06000100);program(slave,0x06000104);
    host.userdata=&actual;host.audio_delta=audio;
    m=kega32x_machine_create(&host,master,sizeof(master),slave,sizeof(slave),0,0);CHECK(m);
    bus=kega32x_machine_bus(m);
    kega32x_machine_run_until(m,100);
    kega32x_bus_write16(bus,KEGA32X_M68K,0,3,100);
    kega32x_machine_run_until(m,1000);
    CHECK(kega32x_machine_read(m,0,0x06000100,4)>0 && kega32x_machine_read(m,1,0x06000104,4)>0);
    kega32x_machine_write(m,0,0xc0000100,0xbeef,2);
    bus->memory->framebuffer[1][65535]=0xc3d4;bus->memory->sdram[131071]=0xa1b2;
    kega32x_bus_write16(bus,KEGA32X_M68K,0x100,2,1000);
    kega32x_bus_blank(bus,0,0,1000);
    kega32x_bus_write16(bus,KEGA32X_M68K,0x10a,1,1000);
    kega32x_bus_write16(bus,KEGA32X_M68K,0x104,3,1000);
    kega32x_bus_write16(bus,KEGA32X_M68K,0x106,0x10fe,1000);
    kega32x_bus_write16(bus,KEGA32X_M68K,0x108,0x1234,1000);
    kega32x_bus_write16(bus,KEGA32X_M68K,6,4,1000);
    kega32x_bus_write16(bus,KEGA32X_M68K,0x10,8,1000);
    for(i=0;i<3;i++)kega32x_bus_write16(bus,KEGA32X_M68K,0x12,(uint16_t)(0x1111*(i+1)),1000);
    kega32x_bus_write16(bus,KEGA32X_M68K,0x30,0x0105,1000);
    kega32x_bus_write16(bus,KEGA32X_M68K,0x32,10,1000);
    kega32x_bus_write16(bus,KEGA32X_M68K,0x34,4,1000);
    kega32x_bus_write16(bus,KEGA32X_M68K,0x36,8,1000);
    CHECK(kega32x_machine_get_state(m,saved));CHECK(kega32x_state_validate(saved));
    CHECK(kega32x_state_encode(saved,wire,bytes));
    CHECK(!memcmp(wire,"K32XST01",8) && wire[8]==1 && !wire[9] && !wire[10] && !wire[11]);
    CHECK(wire[bytes-2]==0xb2 && wire[bytes-1]==0xa1);
    CHECK(wire[24+11431+262144-2]==0xd4 && wire[24+11431+262144-1]==0xc3);
    CHECK(kega32x_state_decode(decoded,wire,bytes));
    CHECK(kega32x_state_encode(decoded,other,bytes) && !memcmp(wire,other,bytes));
    memset(before,0x5a,sizeof(*before));memcpy(decoded,before,sizeof(*decoded));
    CHECK(decode_rejects(decoded,wire,bytes-1,before));CHECK(decode_rejects(decoded,wire,bytes+1,before));
    memcpy(other,wire,bytes);other[0]^=1;CHECK(decode_rejects(decoded,other,bytes,before));
    memcpy(other,wire,bytes);other[8]=2;CHECK(decode_rejects(decoded,other,bytes,before));
    memcpy(other,wire,bytes);other[16]=1;CHECK(decode_rejects(decoded,other,bytes,before));
    memcpy(other,wire,bytes);other[24+614]=9;CHECK(decode_rejects(decoded,other,bytes,before));
    memcpy(other,wire,bytes);other[24+628+34]=4;CHECK(decode_rejects(decoded,other,bytes,before));
    memcpy(other,wire,bytes);other[24+628+45]=0;CHECK(decode_rejects(decoded,other,bytes,before));
    memcpy(other,wire,bytes);other[24+628+45+4256]^=1;CHECK(decode_rejects(decoded,other,bytes,before));
    memset(other,0x77,bytes);memcpy(expected,other,bytes);
    CHECK(!kega32x_state_encode(saved,other,bytes-1) && !memcmp(other,expected,bytes));
    CHECK(!kega32x_state_encode(saved,saved,sizeof(*saved)));
    /* Entire bundle validates before any live memory/CPU state/callback changes. */
    CHECK(kega32x_machine_get_state(m,before));CHECK(kega32x_state_encode(before,expected,bytes));
    memcpy(decoded,saved,sizeof(*decoded));decoded->control.bus.fifo_count=9;
    i=actual.count;CHECK(!kega32x_machine_set_state(m,decoded) && actual.count==i);
    CHECK(kega32x_machine_get_state(m,before) && kega32x_state_encode(before,other,bytes) && !memcmp(other,expected,bytes));
    memcpy(decoded,saved,sizeof(*decoded));decoded->control.bus.vdp[0]^=0x8000;
    CHECK(!kega32x_machine_set_state(m,decoded) && actual.count==i);
    CHECK(kega32x_machine_get_state(m,before) && kega32x_state_encode(before,other,bytes) && !memcmp(other,expected,bytes));
    CHECK(kega32x_state_decode(decoded,wire,bytes));
    actual.count=0;future(m);baseline=actual;
    CHECK(kega32x_machine_get_state(m,before) && kega32x_state_encode(before,expected,bytes));
    CHECK(kega32x_machine_set_state(m,decoded));
    CHECK(kega32x_machine_read(m,0,0xc0000100,2)==0xbeef);
    actual.count=0;future(m);
    CHECK(kega32x_machine_get_state(m,before) && kega32x_state_encode(before,other,bytes));
    CHECK(!memcmp(expected,other,bytes));CHECK(actual.count==baseline.count && actual.count>0);
    for(i=0;i<actual.count;i++)CHECK(actual.events[i].t==baseline.events[i].t &&
        actual.events[i].l==baseline.events[i].l && actual.events[i].r==baseline.events[i].r);
    kega32x_machine_destroy(m);free(saved);free(decoded);free(before);free(wire);free(other);free(expected);
    puts("32X states: LE roundtrip, atomic rejection and replayed dual-CPU/RAM/FIFO/PWM timeline passed");
    return 0;
}
