/* GPX-only attachment boundary. All entry points run on its emulation thread. */
#ifndef KEGA32X_GPX_BRIDGE_H
#define KEGA32X_GPX_BRIDGE_H
#include <stdint.h>
#include <stddef.h>
extern int kegarefusion_32x_attached;
unsigned kegarefusion_32x_features(void);
void kegarefusion_32x_invalidate_code(void);
int kegarefusion_32x_stats(uint64_t *, size_t);
/* Direct SDRAM pointer is valid only while the 32X attachment is loaded. */
void *kegarefusion_32x_sdram(void);
/* Called after GPX's normal system_init/reset and after resolving BIOS.
 * A null image selects the matching built-in 32X bootstrap.
   Returns zero on failure, no partial attachment is exposed. */
int kegarefusion_32x_attach(const uint8_t *bios_g, const uint8_t *bios_m, const uint8_t *bios_s);
void kegarefusion_32x_detach(void);
void kegarefusion_32x_soft_reset(void);
/* Reinitializes GPX and its attachment together, retaining 32X allocations. */
void kegarefusion_32x_reinit(void);
/* Selection: 0=disabled, 1=monochrome, 2=composite, 3=S-Video, 4=RGB.
   Call on GPX option updates even before attachment; no unattached allocation.
   Runtime failure disables filtering and marks geometry dirty; caller reports it. */
int kegarefusion_32x_set_filter(unsigned preset);
/* Call after successful attach, before load succeeds, to surface allocation
   failure as load failure. Returns zero without changing the requested preset. */
int kegarefusion_32x_prepare_filter(void);
void kegarefusion_32x_run(unsigned int target_mclk);
void kegarefusion_32x_frame_start(int skip_video);
void kegarefusion_32x_frame_end(unsigned int master_clocks);
void kegarefusion_32x_line(int vdp_line, unsigned int master_clock);
/* Called in remap_line before loss of MD indexed /YS metadata. Returns 1 if
   output was rendered. md_pixels is the native RGB565 palette, indices include
   both horizontal borders, logical_line remains the original VDP line. */
int kegarefusion_32x_render(int logical_line, int output_line, const uint8_t *indices,
                           const uint16_t *md_pixels, unsigned int width);
size_t kegarefusion_32x_state_size(void);
int kegarefusion_32x_state_save(void *, size_t);
int kegarefusion_32x_save_gpx(void *);
int kegarefusion_32x_state_prepare(const void *, size_t);
void kegarefusion_32x_state_begin_load(void);
int kegarefusion_32x_state_end_load(int success);
void kegarefusion_m68k_run(unsigned int cycles);
void kegarefusion_s68k_run(unsigned int cycles);
void kegarefusion_m68k_yield(void);
void kegarefusion_s68k_yield(void);
/* CD helper functions retain GPX scanline clock ratio and advance only the
   existing CD sub-CPU/devices. Main CPU/SH2 scheduling is owned by bridge.c. */
unsigned int kegarefusion_scd_next_event(void);
void kegarefusion_scd_run(unsigned int target_mclk);
#endif
