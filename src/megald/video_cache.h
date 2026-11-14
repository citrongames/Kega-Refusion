#ifndef KR_LD_VIDEO_CACHE_H
#define KR_LD_VIDEO_CACHE_H
#include "media/media.h"
typedef struct kr_ld_cache kr_ld_cache;
kr_ld_cache *kr_ld_cache_open(megald_media *m, unsigned side, uint32_t width, uint32_t height);
void kr_ld_cache_select_side(kr_ld_cache *v, unsigned side);
void kr_ld_cache_close(kr_ld_cache *v);
/* Returned RGB pixels stay valid until the next get or close. */
const unsigned char *kr_ld_cache_get(kr_ld_cache *v, int frame);
void kr_ld_cache_prefetch(kr_ld_cache *v, int frame);
#endif
