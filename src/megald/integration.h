/* Sega PAC glue in the integrated Genesis Plus GX module. */
#ifndef KR_MEGALD_INTEGRATION_H
#define KR_MEGALD_INTEGRATION_H
#include <stdint.h>

extern int kr_megald_active;
struct kr_inspect_info;
void kr_inspect_megald(struct kr_inspect_info *info, unsigned parts);
int kr_megald_mount(const char *path, char *header);
void kr_megald_close(void);
int kr_megald_japanese(void);
unsigned kr_megald_sides(void);
unsigned kr_megald_side(void);
int kr_megald_select_side(unsigned index);
void kr_megald_map_bus(void);
void kr_megald_reset(int hard);
void kr_megald_sector(uint8_t *dst);
void kr_megald_seek_complete(void);
void kr_megald_advance(void);
void kr_megald_audio(unsigned samples, int *previous, int left_gain, int right_gain);
void kr_megald_begin_field(void);
int kr_megald_scanline(int y, int output_y, const uint8_t *indices, const uint16_t *palette, int width);
/* Optional native RGB565 row includes borders; original indices retain the
 * foreground/background fade selection. Consumed before returning. */
int kr_megald_scanline_rgb(int y, int output_y, const uint8_t *indices, const uint16_t *palette, int width, const uint16_t *rgb);
unsigned kr_megald_save(uint8_t *out);
unsigned kr_megald_restore(const uint8_t *in);
#endif
