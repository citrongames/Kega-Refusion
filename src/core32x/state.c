/* Kega Refusion: explicit, bounded little-endian 32X state format, 2026.
 * Device/CPU API structures come from the existing PicoDrive-derived modules.
 * This codec never copies C structure padding into the wire representation. */
#include "state.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

enum { WIRE_HEADER = 24, BUS_BYTES = 628, PWM_BYTES = 45,
       CPU_BYTES = 4776, SOC_BYTES = 591,
       CONTROL_BYTES = BUS_BYTES + PWM_BYTES + CPU_BYTES*2 + SOC_BYTES*2 + 24,
       MEMORY_BYTES = 512*1024, WIRE_BYTES = WIRE_HEADER + CONTROL_BYTES + MEMORY_BYTES };
static const uint8_t magic[8] = {'K','3','2','X','S','T','0','1'};
static unsigned irq_level(const Kega32xBusState *s,unsigned cpu)
{
    unsigned p,level=0;
    if((s->regs[0]&3)!=3)return 0;
    p=s->irq_pending[cpu];
    while(p>>=1)level+=2;
    return level;
}
static int valid_bus(const Kega32xBusState *s)
{
    unsigned available,partial,bank,i;
    if((s->regs[0]&~0x8083u) || (s->regs[1]&~3u) || (s->regs[2]&~3u) ||
       (s->regs[3]&~0x87u) || (s->regs[4]&~255u) || (s->regs[5]&1u) ||
       (s->regs[6]&~255u) || (s->regs[13]&~0x101u) ||
       (s->sh2_control&~0x380u) || ((s->sh2_control>>9)&1)!=(s->regs[0]&1) ||
       (s->vdp[0]&~0x80ffu) || s->vdp[1]>1 || s->vdp[2]>255 ||
       (s->vdp[5]&~0xe003u) || s->vdp[6] || s->vdp[7] ||
       s->fifo_read>7 || s->fifo_write>7 || s->fifo_ready>3 || s->fifo_count>8 ||
       s->pending_fb>1 || (!(s->vdp[5]&2) && s->fill_end_mclk))return 0;
    for(i=0;i<2;i++) {
        unsigned command=(s->regs[1]&(1u<<i)) && (s->irq_mask[i]&2);
        if((s->irq_mask[i]&~15u) || (s->irq_pending[i]&~0xf8u) ||
           !!(s->irq_pending[i]&KEGA32X_IRQ_CMD)!=!!command)return 0;
    }
    bank=s->fifo_read>>2;
    if(!(s->fifo_ready&(1u<<bank)) && ((s->fifo_read&3) || s->fifo_ready))return 0;
    available=(s->fifo_ready&(1u<<bank)) ? 4u-(s->fifo_read&3u)+
        ((s->fifo_ready&(1u<<(bank^1)))?4u:0u):0u;
    if((s->fifo_ready&(1u<<(s->fifo_write>>2))) && (s->fifo_write&3))return 0;
    partial=(s->fifo_ready&(1u<<(s->fifo_write>>2)))?0u:s->fifo_write&3u;
    if(s->fifo_count!=available+partial || ((s->fifo_read+s->fifo_count)&7)!=s->fifo_write ||
       !!(s->regs[3]&0x80)!=!!(s->fifo_ready&(1u<<(s->fifo_write>>2))))return 0;
    return 1;
}
int kega32x_control_state_validate(const Kega32xControlState *s)
{
    Kega32xPwm test={0};
    const uint64_t max_sh2=(UINT64_MAX/7)*3+(UINT64_MAX%7)*3/7;
    unsigned i;
    if(!s || s->version!=KEGA32X_MACHINE_STATE_VERSION || !valid_bus(&s->bus) ||
       !kega32x_pwm_set_state(&test,&s->pwm))return 0;
    for(i=0;i<2;i++) {
        if(!kega_sh2_validate_state(&s->cpu[i]) || !kega_sh2_soc_validate_state(&s->soc[i]) ||
           memcmp(s->cpu[i].peri_regs,s->soc[i].regs,sizeof(s->soc[i].regs)) ||
           s->epoch_sh2[i]>max_sh2 || s->cpu[i].cycles>max_sh2-s->epoch_sh2[i] ||
           s->soc[i].now>max_sh2-s->epoch_sh2[i])return 0;
        if(s->cpu[i].pending_irl!=irq_level(&s->bus,i))return 0;
    }
    return 1;
}
int kega32x_state_validate(const Kega32xMachineState *s)
{ return s && kega32x_control_state_validate(&s->control); }
size_t kega32x_state_encoded_size(void) { return WIRE_BYTES; }

typedef struct Wire {
    uint8_t *output;
    const uint8_t *input;
    size_t position,limit;
    int read;
} Wire;
static void u8(Wire *w,uint8_t *v)
{
    assert(w->position<w->limit);
    if(w->read)*v=w->input[w->position];else w->output[w->position]=*v;
    ++w->position;
}
static void u16(Wire *w,uint16_t *v)
{
    unsigned i;uint16_t n=*v;
    assert(w->position<=w->limit && w->limit-w->position>=2);
    if(w->read) {n=0;for(i=0;i<2;i++)n|=(uint16_t)w->input[w->position+i]<<(8*i);*v=n;}
    else for(i=0;i<2;i++)w->output[w->position+i]=(uint8_t)(n>>(8*i));
    w->position+=2;
}
static void u32(Wire *w,uint32_t *v)
{
    unsigned i;uint32_t n=*v;
    assert(w->position<=w->limit && w->limit-w->position>=4);
    if(w->read) {n=0;for(i=0;i<4;i++)n|=(uint32_t)w->input[w->position+i]<<(8*i);*v=n;}
    else for(i=0;i<4;i++)w->output[w->position+i]=(uint8_t)(n>>(8*i));
    w->position+=4;
}
static void u64(Wire *w,uint64_t *v)
{
    unsigned i;uint64_t n=*v;
    assert(w->position<=w->limit && w->limit-w->position>=8);
    if(w->read) {n=0;for(i=0;i<8;i++)n|=(uint64_t)w->input[w->position+i]<<(8*i);*v=n;}
    else for(i=0;i<8;i++)w->output[w->position+i]=(uint8_t)(n>>(8*i));
    w->position+=8;
}
static void signed32(Wire *w,int32_t *v)
{
    uint32_t n=(uint32_t)*v;u32(w,&n);
    if(w->read)*v=n<=INT32_MAX?(int32_t)n:-1-(int32_t)(UINT32_MAX-n);
}
static void bus_wire(Wire *w,Kega32xBusState *s)
{
    unsigned i;
    for(i=0;i<32;i++)u16(w,&s->regs[i]);
    u16(w,&s->sh2_control);
    for(i=0;i<8;i++)u16(w,&s->vdp[i]);
    for(i=0;i<256;i++)u16(w,&s->palette[i]);
    for(i=0;i<8;i++)u16(w,&s->dreq_fifo[i]);
    for(i=0;i<2;i++)u8(w,&s->irq_mask[i]);
    for(i=0;i<2;i++)u8(w,&s->irq_pending[i]);
    u8(w,&s->fifo_read);u8(w,&s->fifo_write);u8(w,&s->fifo_ready);u8(w,&s->fifo_count);
    u8(w,&s->pending_fb);u8(w,&s->hcount);u64(w,&s->fill_end_mclk);
}
static void pwm_wire(Wire *w,Kega32xPwmState *s)
{
    unsigned i,j;
    u64(w,&s->now_mclk);u64(w,&s->cursor_sh2);u16(w,&s->control);u16(w,&s->cycle);
    for(i=0;i<2;i++)for(j=0;j<3;j++)u16(w,&s->fifo[i][j]);
    for(i=0;i<2;i++)u8(w,&s->read[i]);
    for(i=0;i<2;i++)u8(w,&s->count[i]);
    u8(w,&s->irq_count);for(i=0;i<2;i++)signed32(w,&s->current[i]);
}
#define CPU_FIELDS(X) X(version) X(pc) X(ppc) X(pr) X(sr) X(gbr) X(vbr) X(mach) X(macl) \
    X(ea) X(delay) X(test_irq) X(pending_level) X(pending_irl) X(pending_int_irq) \
    X(pending_int_vector) X(state) X(poll_addr) X(poll_cycles) X(poll_cnt) X(no_polling) \
    X(m68krcycles_done) X(mult_m68k_to_sh2) X(mult_sh2_to_m68k)
static void cpu_wire(Wire *w,KegaSh2State *s)
{
    unsigned i;
#define FIELD(n) u32(w,&s->n);
    CPU_FIELDS(FIELD)
#undef FIELD
    for(i=0;i<16;i++)u32(w,&s->r[i]);
    /* Native donor data-array bytes are word-swapped. Encode numeric words,
       not host byte layout, just as the machine's data-array accesses do. */
    for(i=0;i<sizeof(s->data_array);i+=2) {
        uint16_t n=0;if(!w->read)memcpy(&n,s->data_array+i,2);
        u16(w,&n);if(w->read)memcpy(s->data_array+i,&n,2);
    }
    for(i=0;i<128;i++)u32(w,&s->peri_regs[i]);
    u64(w,&s->cycles);
}
static void soc_wire(Wire *w,KegaSh2SocState *s)
{
    unsigned i;
    u32(w,&s->version);for(i=0;i<128;i++)u32(w,&s->regs[i]);
    u64(w,&s->now);u64(w,&s->frt_last);u64(w,&s->wdt_last);
    for(i=0;i<2;i++)u64(w,&s->dma_due[i]);
    u32(w,&s->frt_phase);u32(w,&s->wdt_phase);
    for(i=0;i<2;i++)u32(w,&s->dma_remaining[i]);
    u32(w,&s->irq_pending);u32(w,&s->irq_selected);u16(w,&s->ocra);u16(w,&s->ocrb);
    for(i=0;i<2;i++)u8(w,&s->dma_active[i]);
    for(i=0;i<2;i++)u8(w,&s->dreq_tokens[i]);
    u8(w,&s->wdt_key);u8(w,&s->rst_key);u8(w,&s->dma_turn);
}
static void control_wire(Wire *w,Kega32xControlState *s)
{
    unsigned i;
    bus_wire(w,&s->bus);pwm_wire(w,&s->pwm);
    for(i=0;i<2;i++)cpu_wire(w,&s->cpu[i]);
    for(i=0;i<2;i++)soc_wire(w,&s->soc[i]);
    for(i=0;i<2;i++)u64(w,&s->epoch_sh2[i]);
    u64(w,&s->now);
    assert(w->position==WIRE_HEADER+CONTROL_BYTES);
}
static int overlap(const void *a,size_t as,const void *b,size_t bs)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y?y-x<as:x-y<bs;
}
static int little_endian(void) { const uint16_t n=1;return *(const uint8_t *)&n; }
static void memory_write(Wire *w,const Kega32xMemory *m)
{
    unsigned i,j;
    if(little_endian()) {
        for(i=0;i<2;i++) {memcpy(w->output+w->position,m->framebuffer[i],131072);w->position+=131072;}
        memcpy(w->output+w->position,m->sdram,262144);w->position+=262144;
    } else {
        for(i=0;i<2;i++)for(j=0;j<65536;j++) {uint16_t n=m->framebuffer[i][j];u16(w,&n);}
        for(i=0;i<131072;i++) {uint16_t n=m->sdram[i];u16(w,&n);}
    }
}
static void memory_read(Wire *w,Kega32xMemory *m)
{
    unsigned i,j;
    if(little_endian()) {
        for(i=0;i<2;i++) {memcpy(m->framebuffer[i],w->input+w->position,131072);w->position+=131072;}
        memcpy(m->sdram,w->input+w->position,262144);w->position+=262144;
    } else {
        for(i=0;i<2;i++)for(j=0;j<65536;j++)u16(w,&m->framebuffer[i][j]);
        for(i=0;i<131072;i++)u16(w,&m->sdram[i]);
    }
}
int kega32x_state_encode(const Kega32xMachineState *s,void *output,size_t capacity)
{
    Kega32xControlState control;Wire w;uint32_t version=1,length=WIRE_BYTES-WIRE_HEADER,zero=0;
    if(!s || !output || capacity<WIRE_BYTES || !kega32x_control_state_validate(&s->control) ||
       overlap(s,sizeof(*s),output,WIRE_BYTES))return 0;
    control=s->control;memcpy(output,magic,8);
    w=(Wire){output,NULL,8,WIRE_BYTES,0};u32(&w,&version);u32(&w,&length);u32(&w,&zero);u32(&w,&zero);
    control_wire(&w,&control);memory_write(&w,&s->memory);assert(w.position==WIRE_BYTES);return 1;
}
int kega32x_state_decode(Kega32xMachineState *s,const void *input,size_t size)
{
    Kega32xControlState control={0};Wire w;uint32_t version=0,length=0,zero1=0,zero2=0;
    if(!s || !input || size!=WIRE_BYTES || memcmp(input,magic,8) || overlap(s,sizeof(*s),input,size))return 0;
    w=(Wire){NULL,input,8,size,1};u32(&w,&version);u32(&w,&length);u32(&w,&zero1);u32(&w,&zero2);
    if(version!=1 || length!=WIRE_BYTES-WIRE_HEADER || zero1 || zero2)return 0;
    control.version=KEGA32X_MACHINE_STATE_VERSION;control_wire(&w,&control);
    if(!kega32x_control_state_validate(&control))return 0;
    /* All rejection paths precede the first destination write. */
    s->control=control;memory_read(&w,&s->memory);assert(w.position==WIRE_BYTES);return 1;
}
