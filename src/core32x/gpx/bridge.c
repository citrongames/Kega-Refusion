/* Kega Refusion GPX attachment. MD/CD engines and their maps stay in GPX.
 * 32X mapping rules adapted from PicoDrive 07abb005, noncommercial COPYING.
 * This file is compiled only in the experimental GPX fork. */
#include "shared.h"
#include "bridge.h"
#include "bootrom.h"
#include "../machine.h"
#include "../state.h"
#ifdef KEGA_SH2_DRC
#include "../sh2/drc.h"
#endif
#include "../video/compositor.h"
#include "../video/ntsc565.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
/* Diagnostics use the C stream, not libretro virtual file handles. */
#ifdef fprintf
#undef fprintf
#endif
int kegarefusion_32x_attached;
extern uint32_t kega_debug_video_mask;
extern uint8_t *kega_recording_bitmap;
/* GPX may redraw the previous row or render the next one early. Keep three
   tagged rows, never a frame-sized copy or a queue of register writes. */
typedef struct Gpx32xVideoRow {
    uint64_t start, latch_due, pixels_due;
    int logical, output;
    unsigned valid, md_ready, latched, decoded_ready, border, md_width;
    uint16_t control, shift;
    unsigned bank, enabled;
    uint8_t indices[384];
    uint16_t palette[256];
    Kega32xDecodedLine decoded;
} Gpx32xVideoRow;
typedef struct Gpx32x {
    Kega32xMachine *machine;
    cpu_memory_map original[256];
    t_zbank_memory_map original_zbank[256];
    int has_cart_rom;
    uint32_t boot_identity; /* Zero in existing states with three original BIOS. */
    uint8_t boot_page[65536];
    uint64_t frame_base, hint_due;
    unsigned now, main_running, hint_count;
    int line, skip_video;
    unsigned line_start;
    Gpx32xVideoRow video[3];
    uint64_t video_due;
    Kega32xNtsc565 *filter;
    int filter_preset;
    Kega32xMachineState *snapshot;
    struct {
        uint64_t frame_base, hint_due;
        unsigned now, hint_count, hint_vector;
        int line, ready;
    } restore;

} Gpx32x;
static Gpx32x *ctx;
void *kegarefusion_32x_sdram(void) {
    Kega32xBus *bus=ctx && ctx->machine?kega32x_machine_bus(ctx->machine):NULL;
    return bus && bus->memory ? bus->memory->sdram : NULL;
}
/* GPX config.ntsc is an enable flag, not the selected preset. Options are
   received before attachment too; ordinary MD/CD never allocate this table. */
static unsigned requested_filter_preset;
static void video_sync(uint64_t);
static void video_reset(void) {
    unsigned i;
    ctx->video_due=UINT64_MAX;
    for(i=0;i<3;i++)ctx->video[i].valid=0;
    if(ctx->machine)kega32x_machine_bus(ctx->machine)->video_watch_banks=0;
}
static uint64_t video_next_event(void);
static Gpx32xVideoRow *video_row(int logical,uint64_t start) {
    Gpx32xVideoRow *row=&ctx->video[(start/MCYCLES_PER_LINE)%3];
    if(!row->valid || row->start!=start || row->logical!=logical) {
        /* GPX HVC tables place active MD pixels at 988/1000 master clocks.
           H32's 32X layer follows by 13 quarter-pixels (26 clocks).
           Sega figure 3.17 latches registers 216 clocks before XHDISP.
           This maps documented relative timings onto GPX; bus wait timing
           and analogue /YS edge races remain separate approximations. */
        unsigned active=(reg[12]&1)?988u:1026u;
        row->valid=1;row->start=start;row->logical=logical;
        row->latch_due=start+active-216u;row->pixels_due=start+active;
        row->md_ready=row->latched=row->decoded_ready=0;
        ctx->video_due=video_next_event();
    }
    return row;
}
static uint64_t video_next_event(void) {
    uint64_t due=UINT64_MAX;unsigned i;
    for(i=0;i<3;i++) {
        const Gpx32xVideoRow *row=&ctx->video[i];
        uint64_t next;
        if(!row->valid || row->decoded_ready)continue;
        next=row->latched?row->pixels_due:row->latch_due;
        if(next<due)due=next;
    }
    return due;
}
int kegarefusion_32x_prepare_filter(void)
{
    Kega32xNtsc565 *next;
    if(!ctx)return 1;
    if(!requested_filter_preset) {
        kega32x_ntsc565_delete(ctx->filter);ctx->filter=NULL;ctx->filter_preset=0;
        return 1;
    }
    if(ctx->filter) {
        if(kega32x_ntsc565_setup(ctx->filter,(Kega32xNtsc565Preset)requested_filter_preset)!=KEGA32X_NTSC565_OK)
            return 0;
    } else {
        next=kega32x_ntsc565_create((Kega32xNtsc565Preset)requested_filter_preset);
        if(!next)return 0;
        ctx->filter=next;
    }
    ctx->filter_preset=(int)requested_filter_preset;
    return 1;
}
int kegarefusion_32x_set_filter(unsigned preset)
{
    if(preset>4)return 0;
    requested_filter_preset=preset;
    if(kegarefusion_32x_prepare_filter())return 1;
    /* Runtime failure is explicit to the caller. Keep output geometry and
       actual filtering together, and do not retry an 8 MiB allocation per row. */
    requested_filter_preset=0;
    kegarefusion_32x_prepare_filter();
    config.ntsc=0;bitmap.viewport.changed|=9;
    return 0;
}
static void map_apply(void);
static void maps_restore(void);
static void maps_capture(void);
static uint64_t current_time(void) { return ctx->frame_base+m68k.cycles; }
static unsigned io_offset(unsigned a)
{
    if((a&0xffffc0)==0xa15100)return a&0x3f;
    if((a&0xfffff0)==0xa15180)return 0x100|(a&15);
    if((a&0xfffe00)==0xa15200)return 0x200|(a&511);
    return UINT_MAX;
}
static void sync_device(void) { kega32x_machine_run_until(ctx->machine,current_time()); }
static unsigned map_read8(const cpu_memory_map *map,unsigned a)
{ return map->read8?map->read8(a):READ_BYTE(map->base,a&65535); }
static unsigned map_read16(const cpu_memory_map *map,unsigned a)
{ return map->read16?map->read16(a):*(uint16 *)(map->base+(a&65534)); }
static void map_write8(const cpu_memory_map *map,unsigned a,unsigned v)
{ if(map->write8)map->write8(a,v);else WRITE_BYTE(map->base,a&65535,v); }
static void map_write16(const cpu_memory_map *map,unsigned a,unsigned v)
{ if(map->write16)map->write16(a,v);else *(uint16 *)(map->base+(a&65534))=(uint16)v; }
/* Capture only while owned overlays are removed. The saved cart predicate
   remains valid even if system_hw changes before detach/reinitialization. */
static void maps_restore(void)
{
    unsigned i;
    m68k.memory_map[0xa1]=ctx->original[0xa1];
    zbank_memory_map[0xa1]=ctx->original_zbank[0xa1];
    if(ctx->has_cart_rom) {
        m68k.memory_map[0]=ctx->original[0];
        zbank_memory_map[0]=ctx->original_zbank[0];
    }
    for(i=0x84;i<=0x9f;i++) {
        m68k.memory_map[i]=ctx->original[i];
        zbank_memory_map[i]=ctx->original_zbank[i];
    }
}
static void maps_capture(void)
{
    ctx->has_cart_rom=system_hw!=SYSTEM_MCD || scd.cartridge.boot!=0;
    memcpy(ctx->original,m68k.memory_map,sizeof(ctx->original));
    memcpy(ctx->original_zbank,zbank_memory_map,sizeof(ctx->original_zbank));
    /* Keep the G bootstrap/HINT vectors; the rest follows the selected bank. */
    if(ctx->has_cart_rom && ctx->original[0].base)
        memcpy(ctx->boot_page+256,ctx->original[0].base+256,sizeof(ctx->boot_page)-256);
}
static unsigned zmap_read8(unsigned a)
{
    const t_zbank_memory_map *map=&ctx->original_zbank[a>>16];
    return map->read?map->read(a):READ_BYTE(ctx->original[a>>16].base,a&65535);
}
static void zmap_write8(unsigned a,unsigned v)
{
    const t_zbank_memory_map *map=&ctx->original_zbank[a>>16];
    if(map->write)map->write(a,v);else WRITE_BYTE(ctx->original[a>>16].base,a&65535,v);
}
static int mapping_io(unsigned a) { return (a&0xffff00)==0xa13000 || (a&0xffff00)==0xa14100; }
static void legacy_io_write(unsigned a,unsigned v,unsigned size,int z80)
{
    /* TIME mappers inspect/change live GPX maps, including page zero. Give
       them the real maps, then retain their changes underneath our overlays. */
    maps_restore();
    if(z80)zmap_write8(a,v);
    else if(size==1)map_write8(&ctx->original[0xa1],a,v);
    else map_write16(&ctx->original[0xa1],a,v);
    maps_capture();map_apply();
#ifdef KEGA_SH2_DRC
    kega_sh2_drc_flush();
#endif
}
static bool rom_fetch_is_pure(void *user,uint32_t a,size_t bytes)
{
    unsigned page,last;
    (void)user;
    if(!ctx || !ctx->has_cart_rom || !bytes || bytes>0x400000u || a>0x400000u-bytes)return false;
    last=(unsigned)(a+bytes-1)>>16;
    for(page=a>>16;page<=last;page++)
        if(!ctx->original[page].base || ctx->original[page].read16)return false;
    return true;
}
static uint16_t rom_read(void *user,uint32_t a)
{
    (void)user;a&=0x3ffffe;
    if(!ctx->has_cart_rom)return 0xffff;
    /* Keep the cartridge mapper/SRAM behavior selected by GPX. */
    return (uint16_t)map_read16(&ctx->original[a>>16],a);
}
static unsigned io_read8(unsigned a)
{
    unsigned offset=io_offset(a);
    if((a&0xfffffc)==0xa130ec)return (unsigned char)"MARS"[a&3];
    if(offset==UINT_MAX)return map_read8(&ctx->original[0xa1],a);
    sync_device();return kega32x_bus_read8(kega32x_machine_bus(ctx->machine),2,offset,current_time());
}
static unsigned io_read16(unsigned a)
{
    unsigned offset=io_offset(a);
    if((a&0xfffffc)==0xa130ec)return (a&2)?0x5253:0x4d41;
    if(offset==UINT_MAX)return map_read16(&ctx->original[0xa1],a);
    sync_device();return kega32x_bus_read16(kega32x_machine_bus(ctx->machine),2,offset,current_time());
}
static void io_write8(unsigned a,unsigned v)
{
    unsigned offset=io_offset(a);
    if(offset==UINT_MAX) {
        if(mapping_io(a))legacy_io_write(a,v,1,0);else map_write8(&ctx->original[0xa1],a,v);
        return;
    }
    sync_device();kega32x_bus_write8(kega32x_machine_bus(ctx->machine),2,offset,(uint8_t)v,current_time());
}
static void io_write16(unsigned a,unsigned v)
{
    unsigned offset=io_offset(a);
    if(offset==UINT_MAX) {
        if(mapping_io(a))legacy_io_write(a,v,2,0);else map_write16(&ctx->original[0xa1],a,v);
        return;
    }
    sync_device();kega32x_bus_write16(kega32x_machine_bus(ctx->machine),2,offset,(uint16_t)v,current_time());
}
/* Z80.cycles already counts GPX master clocks (its instruction cost is *15).
   Never reuse m68k.cycles: Z80 can access this bus from another time frontier. */
static uint64_t z80_time(void) { return ctx->frame_base+Z80.cycles; }
static unsigned zio_read8(unsigned a)
{
    unsigned offset=io_offset(a);uint64_t t;
    if((a&0xfffffc)==0xa130ec)return (unsigned char)"MARS"[a&3];
    if(offset==UINT_MAX)return zmap_read8(a);
    t=z80_time();kega32x_machine_run_until(ctx->machine,t);
    return kega32x_bus_read8(kega32x_machine_bus(ctx->machine),2,offset,t);
}
static void zio_write8(unsigned a,unsigned v)
{
    unsigned offset=io_offset(a);uint64_t t;
    if(offset==UINT_MAX) {
        if(mapping_io(a))legacy_io_write(a,v,1,1);else zmap_write8(a,v);
        return;
    }
    t=z80_time();kega32x_machine_run_until(ctx->machine,t);
    kega32x_bus_write8(kega32x_machine_bus(ctx->machine),2,offset,(uint8_t)v,t);
}
static unsigned zfb_read8(unsigned a)
{
    unsigned v;kega32x_machine_run_until(ctx->machine,z80_time());
    v=kega32x_fb_read16(kega32x_machine_bus(ctx->machine),a);
    return (v>>((a&1)?0:8))&255;
}
static void zfb_write8(unsigned a,unsigned v)
{
    const uint64_t t=z80_time();Kega32xBus *b;
    kega32x_machine_run_until(ctx->machine,t);
    b=kega32x_machine_bus(ctx->machine);kega32x_fb_sync(b,t);
    kega32x_fb_write8(b,a,(uint8_t)v);
}
static unsigned fb_read8(unsigned a)
{ unsigned v;sync_device();v=kega32x_fb_read16(kega32x_machine_bus(ctx->machine),a);return (v>>((a&1)?0:8))&255; }
static unsigned fb_read16(unsigned a)
{ sync_device();return kega32x_fb_read16(kega32x_machine_bus(ctx->machine),a); }
static void fb_write8(unsigned a,unsigned v)
{
    Kega32xBus *b;sync_device();b=kega32x_machine_bus(ctx->machine);
    kega32x_fb_sync(b,current_time());kega32x_fb_write8(b,a,(uint8_t)v);
}
static void fb_write16(unsigned a,unsigned v)
{
    Kega32xBus *b;sync_device();b=kega32x_machine_bus(ctx->machine);
    kega32x_fb_sync(b,current_time());kega32x_fb_write16(b,a,(uint16_t)v);
}
static unsigned rom_address(unsigned a)
{
    unsigned bank=kega32x_machine_bus(ctx->machine)->s.regs[2];
    return a<0x900000?a&0x7ffff:(bank<<20)|(a&0xfffff);
}
static unsigned rom_window8(unsigned a) {a=rom_address(a);return map_read8(&ctx->original[a>>16],a);}
static unsigned rom_window16(unsigned a) {a=rom_address(a);return map_read16(&ctx->original[a>>16],a);}
static void rom_window_write8(unsigned a,unsigned v) {a=rom_address(a);map_write8(&ctx->original[a>>16],a,v);}
static void rom_window_write16(unsigned a,unsigned v) {a=rom_address(a);map_write16(&ctx->original[a>>16],a,v);}
static unsigned zrom_window8(unsigned a) {return zmap_read8(rom_address(a));}
static void zrom_window_write8(unsigned a,unsigned v) {zmap_write8(rom_address(a),v);}
static void hint_write8(unsigned a,unsigned v) {if((a&0xfffc)==0x70)WRITE_BYTE(ctx->boot_page,a&65535,v);}
static void hint_write16(unsigned a,unsigned v) {if((a&0xfffc)==0x70)*(uint16 *)(ctx->boot_page+(a&65534))=(uint16)v;}
static void map_apply(void)
{
    Kega32xBus *b=kega32x_machine_bus(ctx->machine);
    unsigned i;
    m68k.memory_map[0xa1].read8=io_read8;m68k.memory_map[0xa1].read16=io_read16;
    m68k.memory_map[0xa1].write8=io_write8;m68k.memory_map[0xa1].write16=io_write16;
    zbank_memory_map[0xa1].read=zio_read8;zbank_memory_map[0xa1].write=zio_write8;
    if(!(b->s.regs[0]&1)) {
        for(i=0x84;i<=0x9f;i++) {
            m68k.memory_map[i]=ctx->original[i];zbank_memory_map[i]=ctx->original_zbank[i];
        }
        if(ctx->has_cart_rom) {m68k.memory_map[0]=ctx->original[0];zbank_memory_map[0]=ctx->original_zbank[0];}
        return;
    }
    for(i=0x84;i<0x88;i++) {
        cpu_memory_map *m=&m68k.memory_map[i];
        m->base=(uint8 *)b->memory->framebuffer[(b->s.vdp[5]&1)^1]+((i&1)<<16);
        m->read8=fb_read8;m->read16=fb_read16;m->write8=fb_write8;m->write16=fb_write16;
        zbank_memory_map[i].read=zfb_read8;zbank_memory_map[i].write=zfb_write8;
    }
    if(ctx->has_cart_rom) {
        m68k.memory_map[0].base=ctx->boot_page;
        m68k.memory_map[0].read8=NULL;m68k.memory_map[0].read16=NULL;
        m68k.memory_map[0].write8=hint_write8;m68k.memory_map[0].write16=hint_write16;
        zbank_memory_map[0].read=NULL;zbank_memory_map[0].write=hint_write8;
        for(i=0x88;i<0xa0;i++) {
            cpu_memory_map *m=&m68k.memory_map[i];
            m->base=ctx->original[rom_address(i<<16)>>16].base;
            m->read8=rom_window8;m->read16=rom_window16;m->write8=rom_window_write8;m->write16=rom_window_write16;
            zbank_memory_map[i].read=zrom_window8;zbank_memory_map[i].write=zrom_window_write8;
        }
    }
}
static void changed(void *user,unsigned source,unsigned flags,uint32_t a,uint64_t t)
{
    (void)user;(void)source;(void)a;(void)t;
    if(flags&KEGA32X_MAP_CHANGED)map_apply();
    if(ctx->main_running)kegarefusion_m68k_yield();
}
static void render_sync(void *user,uint64_t t)
{
    (void)user;
    /* Before a video-register/palette write: if an instruction crossed a
       deadline, capture the old values before installing its new value. */
    video_sync(t);
}
static void audio_delta(void *user,uint64_t t,int32_t l,int32_t r)
{
    uint64_t local;
    (void)user;
    if(!snd.blips[0])return;
    local=t>=ctx->frame_base?t-ctx->frame_base:0;
    if(local<=MCYCLES_PER_LINE*313u+4096u)blip_add_delta(snd.blips[0],(unsigned)local,l,r);
}
int kegarefusion_32x_attach(const uint8_t *g,const uint8_t *m,const uint8_t *s)
{
    Kega32xMachineHost host={0};Gpx32x *next;unsigned i;
    uint8_t builtin_g[256], builtin_m[2048], builtin_s[1024];
    unsigned builtin_mask = (g ? 0u : 1u) | (m ? 0u : 2u) | (s ? 0u : 4u);
    if (!g) { builtin_boot_68k(builtin_g); g = builtin_g; }
    if (!m) { builtin_boot_sh2(builtin_m, 0); m = builtin_m; }
    if (!s) { builtin_boot_sh2(builtin_s, 1); s = builtin_s; }
    kegarefusion_32x_detach();
    next=calloc(1,sizeof(*next));if(!next)return 0;
    ctx=next;
    ctx->boot_identity = builtin_mask ? (1u << 8) | builtin_mask : 0; /* replacement boot ROMs v1 */
    maps_capture();
    for(i=0;i<256;i++)WRITE_BYTE(ctx->boot_page,i,g[i]);
    host.rom_read16=rom_read;host.rom_fetch_is_pure=rom_fetch_is_pure;host.changed=changed;host.render_sync=render_sync;host.audio_delta=audio_delta;
    ctx->machine=kega32x_machine_create(&host,m,2048,s,1024,vdp_pal,!ctx->has_cart_rom);
    if(!ctx->machine){free(ctx);ctx=NULL;return 0;}
    ctx->hint_due=UINT64_MAX;ctx->line=-1;
    kegarefusion_32x_attached=1;map_apply();return 1;
}
void kegarefusion_32x_detach(void)
{
    if(ctx) {
        /* Restore only the overlays this attachment owns. Never replace CD RAM. */
        maps_restore();
        kega32x_machine_destroy(ctx->machine);kega32x_ntsc565_delete(ctx->filter);free(ctx->snapshot);free(ctx);ctx=NULL;
    }
    kegarefusion_32x_attached=0;
}
void kegarefusion_32x_reinit(void)
{
    if(!ctx){system_init();system_reset();return;}
    maps_restore();
    system_init();system_reset();
    maps_capture();
    ctx->frame_base=0;ctx->now=0;ctx->hint_due=UINT64_MAX;ctx->hint_count=0;ctx->line=-1;
    ctx->line_start=0;video_reset();
    kega32x_machine_power_reset(ctx->machine,vdp_pal,!ctx->has_cart_rom);
    map_apply();
}

void kegarefusion_32x_soft_reset(void)
{
    if(!ctx){gen_reset(0);return;}
    /* Assert VRES at the old timestamp, before GPX randomizes reset phase. */
    kega32x_bus_irq(kega32x_machine_bus(ctx->machine),KEGA32X_IRQ_RESET,current_time());
    maps_restore();gen_reset(0);maps_capture();map_apply();
#ifdef KEGA_SH2_DRC
    kega_sh2_drc_flush();
#endif
    if(ctx->has_cart_rom && (kega32x_machine_bus(ctx->machine)->s.regs[0]&1)) {
        /* gen_reset also resets cartridge banking. Its reset-vector fetch saw
           those unwrapped maps; the enabled adapter supplies the G BIOS ones. */
        const cpu_memory_map *zero=&m68k.memory_map[0];
        m68k_set_reg(M68K_REG_SP,(map_read16(zero,0)<<16)|map_read16(zero,2));
        m68k_set_reg(M68K_REG_PC,(map_read16(zero,4)<<16)|map_read16(zero,6));
    }
}
static void blank_at(unsigned local)
{
    unsigned phase=local%MCYCLES_PER_LINE;
    int hb=phase>=(unsigned)((reg[12]&1)?228:280) && phase<(unsigned)((reg[12]&1)?872:860);
    int vb=ctx->line>=bitmap.viewport.h && ctx->line<lines_per_frame-1;
    kega32x_bus_blank(kega32x_machine_bus(ctx->machine),vb,hb,ctx->frame_base+local);
}
void kegarefusion_32x_frame_start(int skip)
{
    if(!ctx)return;
    ctx->now=0;ctx->line=bitmap.viewport.h;ctx->line_start=0;
    ctx->skip_video=skip!=0;video_reset();
    /* Normally prepared at load/option change, outside scanline rendering.
       The fallback also covers an attachment whose caller omitted load setup. */
    if(!config.ntsc)requested_filter_preset=0;
    if(!kegarefusion_32x_prepare_filter()) {
        kegarefusion_32x_set_filter(0);
        config.ntsc=0;bitmap.viewport.changed|=9;
        fprintf(stderr,"32X RGB565 filter allocation failed; filter disabled.\n");
    }
    blank_at(0);
}
void kegarefusion_32x_frame_end(unsigned cycles)
{
    if(!ctx)return;
    kegarefusion_32x_run(cycles);video_sync(ctx->frame_base+cycles);
    ctx->frame_base+=cycles;ctx->now=0;
}
void kegarefusion_32x_line(int line,unsigned cycles)
{
    Kega32xBus *b;
    if(!ctx)return;
    video_sync(ctx->frame_base+cycles);
    ctx->line=line;ctx->line_start=cycles;
    video_row(line,ctx->frame_base+cycles);
    b=kega32x_machine_bus(ctx->machine);blank_at(cycles);
    if(line==lines_per_frame-1)ctx->hint_count=b->s.hcount;
    if(line<bitmap.viewport.h || (b->s.sh2_control&0x80)) {
        if(!ctx->hint_count) {ctx->hint_count=b->s.hcount;ctx->hint_due=ctx->frame_base+cycles+((reg[12]&1)?436:540);}
        else --ctx->hint_count;
    }
}
void kegarefusion_32x_run(unsigned target)
{
    if(!ctx){m68k_run(target);return;}
    while(ctx->now<target) {
        unsigned next=target,phase=ctx->now%MCYCLES_PER_LINE,edge;
        uint64_t event,video_event;
        video_sync(ctx->frame_base+ctx->now);
        event=kega32x_machine_next_event(ctx->machine);
        video_event=ctx->video_due;if(video_event<event)event=video_event;
        unsigned hb0=(reg[12]&1)?228:280,hb1=(reg[12]&1)?872:860;
        if(next-ctx->now>448)next=ctx->now+448;
        edge=ctx->now-phase+(phase<hb0?hb0:phase<hb1?hb1:MCYCLES_PER_LINE);
        if(edge<next)next=edge;
        if(ctx->hint_due<event)event=ctx->hint_due;
        if(event>ctx->frame_base+ctx->now && event<ctx->frame_base+next)next=(unsigned)(event-ctx->frame_base);
        if(system_hw==SYSTEM_MCD) {
            unsigned cd=kegarefusion_scd_next_event();
            if(cd>ctx->now && cd<next)next=cd;
        }
        ctx->main_running=1;kegarefusion_m68k_run(next);ctx->main_running=0;
        if(m68k.cycles<next)next=m68k.cycles;
        kega32x_machine_run_until(ctx->machine,ctx->frame_base+next);
        if(system_hw==SYSTEM_MCD) {
            kegarefusion_scd_run(next);
            /* A SUB-CPU register write can wake an idle MAIN-CPU at an
               earlier timestamp. Like GPX scd_update, finish that CPU's
               slice before committing the shared frontier. Otherwise a
               final-line wake leaves MAIN behind the frame subtraction,
               producing a negative cycle count and a permanent stall. */
            /* An old snapshot may already contain the former stall. Do
               not turn that invalid residual clock into an endless retry. */
            if(m68k.cycles<0)return;
            if(m68k.cycles<(int)next)continue;
        }
        if(next<=ctx->now)return;
        ctx->now=next;video_sync(ctx->frame_base+next);blank_at(next);
        if(ctx->hint_due<=ctx->frame_base+next) {
            kega32x_bus_irq(kega32x_machine_bus(ctx->machine),KEGA32X_IRQ_HINT,ctx->hint_due);
            ctx->hint_due=UINT64_MAX;
        }
    }
}
static int video_mix(Gpx32xVideoRow *row)
{
    Kega32xVideoLine line={0};uint8_t ys[320];uint16_t mixed[384];
    unsigned i,total=320+2*row->border;size_t pitch,capacity;
    if(!row->md_ready || !row->decoded_ready || !bitmap.data || bitmap.pitch<=0 ||
       row->output<0 || row->output>=bitmap.height)return 0;
    pitch=(size_t)bitmap.pitch/2;
    if((bitmap.pitch&1) || pitch<total*(config.ntsc?2u:1u) ||
       (size_t)bitmap.height>SIZE_MAX/pitch)return 0;
    capacity=pitch*(size_t)bitmap.height;
    for(i=0;i<row->md_width;i++)ys[i]=(row->indices[row->border+i]&15)==0;
    line.md_indices=row->indices+row->border;line.md_palette_rgb565=row->palette;
    line.md_transparent=ys;line.md_width=row->md_width;
    if(kega32x_mix_rgb565(mixed+row->border,320,&row->decoded,&line)!=KEGA32X_VIDEO_OK)return 0;
    for(i=0;i<row->border;i++)mixed[i]=mixed[row->border+320+i]=row->palette[0x40];
    if(kega_recording_bitmap)
        memcpy(kega_recording_bitmap+(size_t)row->output*bitmap.pitch,mixed,total*sizeof(uint16_t));
    if(config.ntsc) {
        if(!ctx->filter)return 0;
        return kega32x_ntsc565_row(ctx->filter,mixed,total,384,row->palette[0x40],
            (uint16_t *)bitmap.data,capacity,pitch,(size_t)row->output)==KEGA32X_NTSC565_OK;
    }
    memcpy(bitmap.data+(size_t)row->output*bitmap.pitch,mixed,total*sizeof(uint16_t));return 1;
}
static void video_decode(Gpx32xVideoRow *row)
{
    Kega32xBus *b=kega32x_machine_bus(ctx->machine);Kega32xVideoLine line={0};
    line.framebuffer=b->memory->framebuffer[row->bank];line.palette_bgr555=b->s.palette;
    line.line=(unsigned)row->logical&255;
    line.mode=(Kega32xVideoMode)(row->control&3);
    if((kega_debug_video_mask & 32u) || !row->enabled || row->logical<0 || row->logical>=((row->control&0x40)?240:224))
        line.mode=KEGA32X_VIDEO_BLANK;
    line.priority_invert=row->control&0x80;line.pixel_shift=row->shift&1;
    if(kega32x_decode_line(&row->decoded,&line)==KEGA32X_VIDEO_OK)video_mix(row);
}
static void video_sync(uint64_t t)
{
    Kega32xBus *b;unsigned i,watch=0;
    if(!ctx || !ctx->machine || t<ctx->video_due)return;
    b=kega32x_machine_bus(ctx->machine);
    ctx->video_due=UINT64_MAX;
    for(i=0;i<3;i++) {
        Gpx32xVideoRow *row=&ctx->video[i];
        if(!row->valid || row->decoded_ready)continue;
        if(!row->latched && t>=row->latch_due) {
            row->control=b->s.vdp[0];row->shift=b->s.vdp[1];
            row->bank=b->s.vdp[5]&1;row->enabled=b->s.regs[0]&1;row->latched=1;
        }
        if(row->latched && t>=row->pixels_due) {
            row->decoded_ready=1;
            /* Register/event timing is identical in skipped frames; only
               pixel work is omitted. A visible row may get its first MD
               remap late, so decode it even before that remap arrives. */
            if(!ctx->skip_video)video_decode(row);
        }
        if(!row->decoded_ready) {
            uint64_t due=row->latched?row->pixels_due:row->latch_due;
            if(due<ctx->video_due)ctx->video_due=due;
            if(!ctx->skip_video && row->latched)watch|=1u<<row->bank;
        }
    }
    b->video_watch_banks=watch;
}
int kegarefusion_32x_render(int logical,int output,const uint8_t *indices,const uint16_t *palette,unsigned width)
{
    Gpx32xVideoRow *row;unsigned border,active;int delta;uint64_t start,offset;
    if(ctx && ctx->skip_video)return 1;
    if(!ctx || !indices || !palette || !bitmap.data || bitmap.pitch<=0 ||
       output<0 || output>=bitmap.height || bitmap.viewport.x<0)return 0;
    border=(unsigned)bitmap.viewport.x;active=(unsigned)bitmap.viewport.w;
    if(border>32 || (active!=256 && active!=320) || width<active+2*border)return 0;
    delta=logical-ctx->line;
    if(delta>lines_per_frame/2)delta-=lines_per_frame;
    if(delta<-lines_per_frame/2)delta+=lines_per_frame;
    start=ctx->frame_base+ctx->line_start;
    offset=(uint64_t)(delta<0?-delta:delta)*MCYCLES_PER_LINE;
    if(delta<0) {if(start<offset)return 0;start-=offset;} else start+=offset;
    row=video_row(logical,start);
    row->output=output;row->border=border;row->md_width=active;
    memcpy(row->indices,indices,active+2*border);
    memcpy(row->palette,palette,sizeof(row->palette));
    row->md_ready=1;
    /* Most remaps occur in HBlank before the latch. Later MD partial blanking
       must use the frozen 32X row, never newly written next-line registers. */
    if(row->decoded_ready)return video_mix(row);
    video_sync(ctx->frame_base+ctx->now);
    return 1;
}

/* A separate versioned block follows the unchanged GPX state. Snapshot storage
   is allocated on first use and retained for rewind; never on each capture. */
#define GPX32X_STATE_HEADER 56u
static void put32(uint8_t *p,uint32_t v)
{ unsigned i;for(i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
static void put64(uint8_t *p,uint64_t v)
{ put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32)); }
static uint32_t get32(const uint8_t *p)
{ return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint64_t get64(const uint8_t *p)
{ return get32(p)|((uint64_t)get32(p+4)<<32); }
static int snapshot_storage(void)
{
    if(!ctx)return 0;
    if(!ctx->snapshot)ctx->snapshot=malloc(sizeof(*ctx->snapshot));
    return ctx->snapshot!=NULL;
}
size_t kegarefusion_32x_state_size(void)
{ return ctx?GPX32X_STATE_HEADER+kega32x_state_encoded_size():0; }
int kegarefusion_32x_state_save(void *data,size_t size)
{
    uint8_t *p=data;uint32_t hint;
    if(!p || size!=kegarefusion_32x_state_size() || !snapshot_storage() || ctx->main_running)return 0;
    if(!kega32x_machine_get_state(ctx->machine,ctx->snapshot) ||
       !kega32x_state_encode(ctx->snapshot,p+GPX32X_STATE_HEADER,size-GPX32X_STATE_HEADER))return 0;
    memcpy(p,"KR32X001",8);put32(p+8,1);put32(p+12,(uint32_t)size);
    put64(p+16,ctx->frame_base);put64(p+24,ctx->hint_due);
    put32(p+32,ctx->now);put32(p+36,ctx->hint_count);put32(p+40,(uint32_t)(ctx->line+1));
    put32(p+44,(uint32_t)ctx->has_cart_rom);
    hint=((uint32_t)*(uint16_t *)(void *)(ctx->boot_page+0x70)<<16)|*(uint16_t *)(void *)(ctx->boot_page+0x72);
    put32(p+48,hint);put32(p+52,ctx->boot_identity);return 1;
}
int kegarefusion_32x_state_prepare(const void *data,size_t size)
{
    const uint8_t *p=data;uint64_t base,now,limit=MCYCLES_PER_LINE*313u;
    if(ctx)ctx->restore.ready=0;
    if(!p || size!=kegarefusion_32x_state_size() || !snapshot_storage() || ctx->main_running ||
       memcmp(p,"KR32X001",8) || get32(p+8)!=1 || get32(p+12)!=size || get32(p+52)!=ctx->boot_identity ||
       get32(p+32)>limit || get32(p+36)>255 || get32(p+40)>313 ||
       get32(p+44)!=(unsigned)ctx->has_cart_rom)return 0;
    base=get64(p+16);
    if(base>UINT64_MAX-limit*2 ||
       !kega32x_state_decode(ctx->snapshot,p+GPX32X_STATE_HEADER,size-GPX32X_STATE_HEADER))return 0;
    /* Match every machine setter precondition before GPX mutates its state. */
    {
        const Kega32xBusState *live=&kega32x_machine_bus(ctx->machine)->s;
        const Kega32xBusState *saved=&ctx->snapshot->control.bus;
        if(((saved->sh2_control^live->sh2_control)&0x100) ||
           ((saved->vdp[0]^live->vdp[0])&0x8000))return 0;
    }
    now=ctx->snapshot->control.now;
    if(now>base+limit || (base>now && base-now>limit))return 0;
    now=ctx->snapshot->control.pwm.now_mclk;
    if(now>base+limit || (base>now && base-now>limit))return 0;
    if(ctx->snapshot->control.bus.vdp[5]&2) {
        now=ctx->snapshot->control.bus.fill_end_mclk;
        if(now>base+limit || (base>now && base-now>limit))return 0;
    }
    if((ctx->snapshot->control.bus.regs[0]&3)==3) {
        unsigned i,k;
        for(i=0;i<2;i++)for(k=0;k<2;k++) {
            uint64_t native=ctx->snapshot->control.epoch_sh2[i]+(k?
                ctx->snapshot->control.soc[i].now:ctx->snapshot->control.cpu[i].cycles);
            now=(native/3)*7+((native%3)*7+2)/3;
            if(now>base+limit || (base>now && base-now>limit))return 0;
        }
    }
    ctx->restore.frame_base=base;ctx->restore.hint_due=get64(p+24);
    if(ctx->restore.hint_due!=UINT64_MAX &&
       (ctx->restore.hint_due>base+limit || (base>ctx->restore.hint_due && base-ctx->restore.hint_due>limit)))return 0;
    ctx->restore.now=get32(p+32);ctx->restore.hint_count=get32(p+36);
    ctx->restore.line=(int)get32(p+40)-1;ctx->restore.hint_vector=get32(p+48);
    ctx->restore.ready=1;return 1;
}
void kegarefusion_32x_state_begin_load(void)
{ if(ctx && ctx->restore.ready)maps_restore(); }
int kegarefusion_32x_state_end_load(int success)
{
    int okay=0;
    if(!ctx || !ctx->restore.ready)return 0;
    maps_capture();
    if(success) {
        ctx->frame_base=ctx->restore.frame_base;ctx->hint_due=ctx->restore.hint_due;
        ctx->now=ctx->restore.now;ctx->hint_count=ctx->restore.hint_count;ctx->line=ctx->restore.line;
        *(uint16_t *)(void *)(ctx->boot_page+0x70)=(uint16_t)(ctx->restore.hint_vector>>16);
        *(uint16_t *)(void *)(ctx->boot_page+0x72)=(uint16_t)ctx->restore.hint_vector;
        /* GPX state_load has reset/rebased its blips. Restore our held PWM
           amplitude only after the absolute-to-frame timestamp is restored. */
        okay=kega32x_machine_set_state(ctx->machine,ctx->snapshot);
    }
    ctx->restore.ready=0;video_reset();map_apply();bitmap.viewport.changed|=9;return okay;
}

int kegarefusion_32x_save_gpx(void *data)
{
    int saved;
    if(!ctx || !data || ctx->main_running)return 0;
    /* GPX serializes mapper bank numbers from base pointers. Its own maps
       must be visible, rather than our allocated G-BIOS shadow page. */
    maps_restore();saved=state_save(data);map_apply();return saved;
}

void kegarefusion_32x_invalidate_code(void)
{
    /* Keep the 68K cartridge shadow coherent too; its first 256 bytes are
       deliberately the 32X bootstrap/HINT overlay, not cartridge bytes. */
    if(ctx && ctx->has_cart_rom && ctx->original[0].base)
        memcpy(ctx->boot_page+256,ctx->original[0].base+256,sizeof(ctx->boot_page)-256);
#ifdef KEGA_SH2_DRC
    /* Frontend cheat edits run between frames, never inside a translated block. */
    if(ctx) kega_sh2_drc_flush();
#endif
}

unsigned kegarefusion_32x_features(void)
{
    unsigned features=1u|(ctx?2u:0u);
#ifdef KEGA_SH2_DRC
    if(ctx && kega_sh2_drc_active(kega32x_machine_cpu(ctx->machine,0)))features|=4u;
#endif
    return features;
}

int kegarefusion_32x_stats(uint64_t *out,size_t count)
{
#ifdef KEGA_SH2_DRC
    KegaSh2DrcStats stats;unsigned i;
    if(!out || count<14)return 0;
    kega_sh2_drc_get_stats(&stats);
    for(i=0;i<2;i++) {
        out[i]=stats.jit_calls[i];out[2+i]=stats.jit_cycles[i];
        out[4+i]=stats.nested_interpreter_calls[i];out[6+i]=stats.nested_interpreter_cycles[i];
        out[8+i]=stats.fallback_interpreter_calls[i];out[10+i]=stats.fallback_interpreter_cycles[i];
    }
    out[12]=stats.cache_misses;out[13]=stats.cache_evictions;
    if(count>=42) {
        KegaSh2DrcBudgetStats budget;unsigned j;
        kega_sh2_drc_get_budget_stats(&budget);
        for(i=0;i<2;i++) {
            out[14+i]=budget.calls[i];out[16+i]=budget.requested_cycles[i];
            out[18+i]=budget.min_budget[i];out[20+i]=budget.max_budget[i];
            for(j=0;j<10;j++)out[22+i*10+j]=budget.histogram[i][j];
        }
    }
    return 1;
#else
    (void)out;(void)count;return 0;
#endif
}
