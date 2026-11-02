/* Read-only host observer: all intercepted calls forward original arguments/results. */
#include <stdint.h>
#include <stddef.h>
#include "shared.h"
#define CAP 4096
#define TABLE 8192
#define TRACE 256
/* categories: cart, expansion, time, a0 exceptional, a1 exceptional, control */
typedef struct { uint64_t calls; uintptr_t handler; uint32_t address,first_value,last_value,min_value,max_value,first_pc,last_pc,first_frame,last_frame,category,kind; } Row;
typedef struct { uint32_t frame,pc,address,value,category,kind; uintptr_t handler; } Event;
static Row rows[CAP]; static unsigned slots[TABLE],used; static uint64_t dropped, totals[12][4];
typedef struct {uintptr_t handler;unsigned limit,rows;uint64_t dropped;} HandlerLimit;
static HandlerLimit limits[16];static unsigned nlimits;
static Event events[TRACE]; static unsigned nevents;
static m68ki_cpu_core *cpu; static md_cart_t *cp; static Z80_Regs *zp; static uint8_t *zs;
static cpu_memory_map original[256]; static t_zbank_memory_map *zm,zo[256]; static unsigned current_frame, installed;
static unsigned (*tr)(unsigned); static void (*tw)(unsigned,unsigned);
static uintptr_t known_bus8, known_bus16, known_unused8, known_unused16;
static unsigned is_ours(void *p);
static void refresh(void); static void restore(void);
static unsigned cat(unsigned a){
 if(a<0x800000) return 0; if(a<0xA00000) return 1;
 if((a&0xFFFF00)==0xA13000) return 2;
 if((a>>16)==0xA0){ if((a&0xFFFF)>=0x4000 || (zs && *zs!=3)) return 3; return 5; }
 if((a>>16)==0xA1){ unsigned l=a&0xFFFF; if(l<0x20 || (l>=0x1100&&l<0x1102) || (l>=0x1200&&l<0x1202) || (l>=0x4000&&l<0x4004) || l==0x4101) return 5; return 4; }
 return 5;
}
static void record(unsigned a,unsigned v,unsigned k,uintptr_t h,unsigned c){
 totals[c][k]++; if(c==5||c==11) return; unsigned pc=c>=6?zp->pc.w.l:cpu->pc;
 unsigned idx=(unsigned)((a*2654435761u) ^ (k*97531u) ^ (c*131u) ^ (h>>4))&(TABLE-1);
 for(unsigned n=0;n<TABLE;n++,idx=(idx+1)&(TABLE-1)){
  Row *r; if(!slots[idx]){ HandlerLimit *lim=NULL;for(unsigned q=0;q<nlimits;q++)if(limits[q].handler==h){lim=&limits[q];break;}if(lim&&lim->rows>=lim->limit){lim->dropped++;return;}if(lim)lim->rows++; if(used==CAP){dropped++;return;} unsigned at=used++;slots[idx]=at+1;r=&rows[at];r->address=a;r->kind=k;r->category=c;r->handler=h;r->min_value=v;r->max_value=v;r->first_value=v;r->first_pc=pc;r->first_frame=current_frame;
   if(nevents<TRACE) events[nevents++]=(Event){current_frame,pc,a,v,c,k,h};
  }else {r=&rows[slots[idx]-1];if(r->address!=a||r->kind!=k||r->category!=c||r->handler!=h)continue;}
  r->calls++;r->last_value=v;if(v<r->min_value)r->min_value=v;if(v>r->max_value)r->max_value=v;r->last_pc=pc;r->last_frame=current_frame;return;
 }
 dropped++;
}
static unsigned r8(unsigned a){unsigned p=(a>>16)&255;unsigned (*f)(unsigned)=original[p].read8;unsigned v=f(a);record(a,v,0,(uintptr_t)f,cat(a));return v;}
static unsigned r16(unsigned a){unsigned p=(a>>16)&255;unsigned (*f)(unsigned)=original[p].read16;unsigned v=f(a);record(a,v,1,(uintptr_t)f,cat(a));return v;}
static void w8(unsigned a,unsigned v){unsigned p=(a>>16)&255;void (*f)(unsigned,unsigned)=original[p].write8;unsigned c=cat(a);record(a,v,2,(uintptr_t)f,c);if(p<0xA0||c==2)restore();f(a,v);refresh();}
static void w16(unsigned a,unsigned v){unsigned p=(a>>16)&255;void (*f)(unsigned,unsigned)=original[p].write16;unsigned c=cat(a);record(a,v,3,(uintptr_t)f,c);if(p<0xA0||c==2)restore();f(a,v);refresh();}
/* TIME native handler observed separately; its enclosing A1 access is counted once. */
static unsigned timer(unsigned a){return tr(a);}
static void timew(unsigned a,unsigned v){tw(a,v);}
static unsigned zr(unsigned a){unsigned(*f)(unsigned)=zo[(a>>16)&255].read;unsigned v=f(a);record(a,v,0,(uintptr_t)f,cat(a)+6);return v;}
static void zw(unsigned a,unsigned v){void(*f)(unsigned,unsigned)=zo[(a>>16)&255].write;unsigned c=cat(a);record(a,v,2,(uintptr_t)f,c+6);if(a<0xA00000||c==2)restore();f(a,v);refresh();}
static unsigned is_ours(void *p){return p==(void*)r8||p==(void*)r16||p==(void*)w8||p==(void*)w16;}
static void refresh(void){
 for(unsigned p=0;p<0xC0;p++){
  cpu_memory_map *m=&cpu->memory_map[p];
  if(m->read8!=(void*)r8){original[p].read8=m->read8;if(m->read8)m->read8=r8;}
  if(m->read16!=(void*)r16){original[p].read16=m->read16;if(m->read16)m->read16=r16;}
  if(m->write8!=(void*)w8){original[p].write8=m->write8;if(m->write8)m->write8=w8;}
  if(m->write16!=(void*)w16){original[p].write16=m->write16;if(m->write16)m->write16=w16;}
  original[p].base=m->base;
  if(zm[p].read!=zr){zo[p].read=zm[p].read;if(zm[p].read)zm[p].read=zr;}
  if(zm[p].write!=zw){zo[p].write=zm[p].write;if(zm[p].write)zm[p].write=zw;}
 }
 /* Do not wrap cart.hw pointers: pointer identity is semantically inspected by core. */
 tr=cp->hw.time_r;tw=cp->hw.time_w;
}
void obs_init(uintptr_t c,uintptr_t cartridge,uintptr_t z,uintptr_t state,uintptr_t zmap){zm=(void*)zmap;cpu=(void*)c;cp=(void*)cartridge;zp=(void*)z;zs=(void*)state;}
void obs_known(uintptr_t r,uintptr_t rw,uintptr_t w,uintptr_t ww){known_bus8=r;known_bus16=rw;known_unused8=w;known_unused16=ww;}
void obs_frame(unsigned f){current_frame=f;if(installed)refresh();}
void obs_install(void){installed=1;refresh();}
static void restore(void){for(unsigned p=0;p<0xC0;p++){cpu_memory_map *m=&cpu->memory_map[p];if(m->read8==(void*)r8)m->read8=original[p].read8;if(m->read16==(void*)r16)m->read16=original[p].read16;if(m->write8==(void*)w8)m->write8=original[p].write8;if(m->write16==(void*)w16)m->write16=original[p].write16;if(zm[p].read==zr)zm[p].read=zo[p].read;if(zm[p].write==zw)zm[p].write=zo[p].write;}}
void obs_remove(void){restore();installed=0;}
unsigned obs_count(void){return used;} Row *obs_rows(void){return rows;} Event *obs_events(void){return events;} unsigned obs_event_count(void){return nevents;} uint64_t obs_dropped(void){return dropped;} uint64_t obs_total(unsigned c,unsigned k){return totals[c][k];}
unsigned obs_pc(void){return cpu->pc;} unsigned obs_zpc(void){return zp->pc.w.l;} unsigned obs_stopped(void){return cpu->stopped;}
unsigned obs_zstate(void){return zs?*zs:0;} unsigned obs_m68k_size(void){return sizeof(m68ki_cpu_core);} unsigned obs_pc_offset(void){return offsetof(m68ki_cpu_core,pc);} unsigned obs_map_size(void){return sizeof(cpu_memory_map);} unsigned obs_cart_size(void){return sizeof(md_cart_t);} unsigned obs_cart_hw_offset(void){return offsetof(md_cart_t,hw);}
uintptr_t obs_time_r(void){return (uintptr_t)tr;} uintptr_t obs_time_w(void){return (uintptr_t)tw;} uintptr_t obs_regs_r(void){return (uintptr_t)cp->hw.regs_r;} uintptr_t obs_regs_w(void){return (uintptr_t)cp->hw.regs_w;}
unsigned obs_realtec(void){return cp->hw.realtec;} unsigned obs_bankshift(void){return cp->hw.bankshift;} unsigned obs_special(void){return cp->special;} unsigned obs_romsize(void){return cp->romsize;}

void obs_limit_handler(uintptr_t h,unsigned limit){if(h&&nlimits<16)limits[nlimits++]=(HandlerLimit){h,limit,0,0};}
unsigned obs_limits_count(void){return nlimits;} uintptr_t obs_limit_ptr(unsigned i){return limits[i].handler;} unsigned obs_limit_rows(unsigned i){return limits[i].rows;} unsigned obs_limit_cap(unsigned i){return limits[i].limit;} uint64_t obs_limit_dropped(unsigned i){return limits[i].dropped;}
