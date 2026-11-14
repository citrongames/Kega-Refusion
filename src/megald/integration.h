/* Sega PAC glue in the integrated Genesis Plus GX module. */
#ifndef KR_MEGALD_INTEGRATION_H
#define KR_MEGALD_INTEGRATION_H
#include <stdint.h>

extern int kr_megald_active;
int kr_megald_mount(const char *path, char *header);
void kr_megald_close(void);
int kr_megald_japanese(void);
void kr_megald_map_bus(void);
void kr_megald_reset(int hard);
void kr_megald_sector(uint8_t *dst);
void kr_megald_seek_complete(void);
void kr_megald_advance(void);
void kr_megald_audio(unsigned samples, int *previous, int left_gain, int right_gain);
void kr_megald_begin_field(void);
int kr_megald_scanline(int y, int output_y, const uint8_t *indices, const uint16_t *palette, int width);
unsigned kr_megald_save(uint8_t *out);
unsigned kr_megald_restore(const uint8_t *in);
#endif
