/* SMS-only observer. Forward native Z80 memory/port writes without changing
 * arguments, return behavior, mapped bytes, CPU cycles or mapper implementation. */
#include <stdint.h>
#include <stddef.h>
#include "shared.h"
#include "sms_observer_types.h" /* exact local types extracted from sms_cart.c */
#define CAP 2048
#define TABLE 4096
#define EVENTS 256
typedef struct {uint64_t calls,changed,values[4];uintptr_t handler;uint32_t address,kind,first_value,last_value,first_pc,last_pc,first_frame,last_frame,first_before_fcr,last_after_fcr,priority;} SmsRow;
typedef struct {uintptr_t handler;uint32_t frame,pc,address,value,kind,before_fcr,after_fcr;uintptr_t before_map[6],after_map[6];} SmsEvent;
static SmsRow rows[CAP];static SmsEvent events[EVENTS];static unsigned slots[TABLE],used,nevents,noisy_rows;
static uint64_t calls,ram_calls,cart_calls,control_calls,map_changes,dropped,noisy_dropped,port_calls;
static uint64_t port_counts[256],port_values[256][4];
static romhw_t *cr,*br;static sms_audit_slot_t *sl;static Z80_Regs *zr;static external_t *ex;
static void(**write_ptr)(unsigned,unsigned char),(**port_ptr)(unsigned,unsigned char);static void(*write_native)(unsigned,unsigned char),(*port_native)(unsigned,unsigned char);
static unsigned char **readmap,**writemap;static unsigned frame,enabled;
static uint32_t fcr(void){uint32_t v=0; if(sl->fcr)for(unsigned i=0;i<4;i++)v|=(uint32_t)sl->fcr[i]<<(8*i);return v;}
static void maps(uintptr_t *out){for(unsigned i=0;i<6;i++)out[i]=(uintptr_t)readmap[i*8];}
static int candidate(unsigned a){return a<=3||a>=0xfff0||a==0x2000||a==0x3ffe||a==0x4000||a==0x6000||a==0x7fff||a==0x8000||a==0xa000||a==0xbfff;}
static void refresh(void);
static void record(unsigned address,unsigned value,unsigned kind,uintptr_t handler,unsigned before,unsigned after,uintptr_t *bm,uintptr_t *am,unsigned priority,unsigned change,unsigned pc){
 unsigned idx=(address*2654435761u ^ kind*151u ^ (unsigned)(handler>>4))&(TABLE-1);
 for(unsigned i=0;i<TABLE;i++,idx=(idx+1)&(TABLE-1)){
  SmsRow *r;
  if(!slots[idx]){
   if(!priority && noisy_rows>=256){noisy_dropped++;return;}
   if(used==CAP){dropped++;return;}
   unsigned at=used++;slots[idx]=at+1;r=&rows[at];r->handler=handler;r->address=address;r->kind=kind;r->first_value=value;r->first_pc=pc;r->first_frame=frame;r->first_before_fcr=before;if(!priority)noisy_rows++;
   if(nevents<EVENTS){SmsEvent *e=&events[nevents++];e->handler=handler;e->frame=frame;e->pc=pc;e->address=address;e->value=value;e->kind=kind;e->before_fcr=before;e->after_fcr=after;for(unsigned k=0;k<6;k++){e->before_map[k]=bm[k];e->after_map[k]=am[k];}}
  }else {r=&rows[slots[idx]-1];if(r->address!=address||r->kind!=kind||r->handler!=handler)continue;}
  r->calls++;r->changed+=change;r->values[value>>6]|=(uint64_t)1<<(value&63);r->last_value=value;r->last_pc=pc;r->last_frame=frame;r->last_after_fcr=after;r->priority|=priority;return;
 }
 dropped++;
}
static void audit_write(unsigned a,unsigned char v){
 unsigned pc=zr->pc.w.l;calls++;unsigned watch=a<0xc000||candidate(a);if(a<0xc000)cart_calls++;else ram_calls++;
 uintptr_t bm[6],am[6];unsigned before=0;if(watch){before=fcr();maps(bm);}
 void(*fn)(unsigned,unsigned char)=write_native;
 *write_ptr=fn;fn(a,v);refresh();
 if(watch){unsigned after=fcr(),change=before!=after;maps(am);for(unsigned i=0;i<6;i++)change|=bm[i]!=am[i];if(change)map_changes++;unsigned priority=candidate(a)||change;if(priority)control_calls++;record(a,v,0,(uintptr_t)fn,before,after,bm,am,priority,change,pc);}
}
static void audit_port(unsigned a,unsigned char v){
 unsigned pc=zr->pc.w.l;port_calls++;port_counts[a&255]++;port_values[a&255][v>>6]|=(uint64_t)1<<(v&63);unsigned watch=(a&0xff)==0x3e||(a&0xff)==0x3f;uintptr_t bm[6],am[6];unsigned before=0;if(watch){before=fcr();maps(bm);}
 void(*fn)(unsigned,unsigned char)=port_native;*port_ptr=fn;fn(a,v);refresh();
 if(watch){unsigned after=fcr(),change=before!=after;maps(am);for(unsigned i=0;i<6;i++)change|=bm[i]!=am[i];record(a,v,1,(uintptr_t)fn,before,after,bm,am,1,change,pc);}
}
static void refresh(void){
 if(*write_ptr!=audit_write){write_native=*write_ptr;if(enabled)*write_ptr=audit_write;}
 if(*port_ptr!=audit_port){port_native=*port_ptr;if(enabled)*port_ptr=audit_port;}
}
void sms_init(uintptr_t cart_hw,uintptr_t slot_hw,uintptr_t bios_hw,uintptr_t z80,uintptr_t write_cb,uintptr_t port_cb,uintptr_t reads,uintptr_t writes,uintptr_t ext_hw){cr=(void*)cart_hw;sl=(void*)slot_hw;br=(void*)bios_hw;zr=(void*)z80;write_ptr=(void*)write_cb;port_ptr=(void*)port_cb;readmap=(void*)reads;writemap=(void*)writes;ex=(void*)ext_hw;refresh();}
void sms_install(void){enabled=1;refresh();}void sms_remove(void){enabled=0;if(*write_ptr==audit_write)*write_ptr=write_native;if(*port_ptr==audit_port)*port_ptr=port_native;}
void sms_frame(unsigned f){frame=f;refresh();}
unsigned sms_mapper(void){return cr->mapper;}unsigned sms_pages(void){return cr->pages;}unsigned sms_bios_mapper(void){return br->mapper;}unsigned sms_bios_pages(void){return br->pages;}unsigned sms_slot_mapper(void){return sl->mapper;}unsigned sms_slot_pages(void){return sl->pages;}
unsigned sms_fcr(unsigned i){return cr->fcr[i&3];}unsigned sms_slot_fcr(unsigned i){return sl->fcr?sl->fcr[i&3]:0;}unsigned sms_zpc(void){return zr->pc.w.l;}unsigned sms_zcycles(void){return zr->cycles;}unsigned sms_halted(void){return zr->halt;}
uintptr_t sms_readmap(unsigned i){return (uintptr_t)readmap[i&63];}uintptr_t sms_writemap(unsigned i){return (uintptr_t)writemap[i&63];}uintptr_t sms_write_handler(void){return (uintptr_t)write_native;}uintptr_t sms_port_handler(void){return (uintptr_t)port_native;}uintptr_t sms_slot_rom(void){return (uintptr_t)sl->rom;}uintptr_t sms_rom_base(void){return (uintptr_t)ex->md_cart.rom;}
unsigned sms_rom_capacity(void){return sizeof(ex->md_cart.rom);}unsigned sms_sram_offset(void){return offsetof(T_SRAM,sram);}unsigned sms_romhw_size(void){return sizeof(romhw_t);}unsigned sms_slot_size(void){return sizeof(sms_audit_slot_t);}unsigned sms_fcr_offset(void){return offsetof(romhw_t,fcr);}unsigned sms_mapper_offset(void){return offsetof(romhw_t,mapper);}unsigned sms_pages_offset(void){return offsetof(romhw_t,pages);}unsigned sms_z80_size(void){return sizeof(Z80_Regs);}
unsigned sms_count(void){return used;}SmsRow *sms_rows(void){return rows;}unsigned sms_event_count(void){return nevents;}SmsEvent *sms_events(void){return events;}
uint64_t sms_total(unsigned i){switch(i){case 0:return calls;case 1:return ram_calls;case 2:return cart_calls;case 3:return control_calls;case 4:return map_changes;case 5:return dropped;case 6:return noisy_dropped;case 7:return port_calls;}return 0;}

uint64_t sms_port_count(unsigned p){return port_counts[p&255];}uint64_t sms_port_values(unsigned p,unsigned word){return port_values[p&255][word&3];}

unsigned sms_fm_config_offset(void){return offsetof(t_config,ym2413);}
