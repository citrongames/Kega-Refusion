#ifndef KEGA_PAPRIUM_BRIDGE_H
#define KEGA_PAPRIUM_BRIDGE_H
#include <stdint.h>
#include <stddef.h>
/* PCM positions/counts are native-rate stereo frames. A zero-count request returns
 * the track length for position 0, or sample rate for position 1. Nonzero
 * requests fill at most that many interleaved frames. */
typedef uint64_t (*paprium_music_callback)(void *, unsigned, uint64_t, int16_t *, size_t);
void paprium_set_music_callback(paprium_music_callback callback, void *user);
int paprium_active(void);
int paprium_cd_active(void);
size_t paprium_extra_state_size(void);
void paprium_shutdown(void);
void punchium_audio(int output_samples);
#endif
