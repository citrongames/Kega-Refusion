/* Read-only, bounded MegaLD MMI streaming reader. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KEGA_MEGALD_MEDIA_H
#define KEGA_MEGALD_MEDIA_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MEGALD_MEDIA_MAX_SIDES 16
#define MEGALD_MEDIA_MAX_TRACKS 99
#define MEGALD_MEDIA_TEXT 256

typedef struct megald_media megald_media;
typedef struct megald_media_track {
  int number;                 /* 1..99 */
  int is_data;                /* MODE1/2352 or AUDIO */
  int64_t index00;             /* file LBA, -1 if absent */
  int64_t index01;             /* file LBA: first data track starts at 0 */
  int64_t end_lba;             /* exclusive, next INDEX00/01 or image end */
} megald_media_track;

typedef struct megald_media_side {
  char name[MEGALD_MEDIA_TEXT];
  char format[32];             /* NTSC-CAV or NTSC-CLV */
  int sequence_no, volume_no, side_no;
  uint32_t width, height, frame_count;
  uint32_t frame_duration_us;  /* 0 in current NTSC images; use 30000/1001 Hz */
  uint32_t lead_in_frames, active_frames, lead_out_frames;
  int has_video, has_analog_audio, has_digital_audio;
  uint64_t analog_sample_frames; /* interleaved stereo signed little endian PCM, 44100 Hz */
  uint64_t digital_sectors;      /* 2352 raw bytes per sector */
  int track_count;
  megald_media_track tracks[MEGALD_MEDIA_MAX_TRACKS];
} megald_media_side;

typedef struct megald_media_info {
  char name[MEGALD_MEDIA_TEXT];
  char system[32];             /* MegaLD or LDROM2 */
  char region[16];             /* J, U */
  char catalog_id[64];
  int side_count;              /* sorted by sequenceNo, stable 0-based API indices */
} megald_media_info;

/* UTF-8 original path; never extracts or modifies images. Error text is optional.
   Open reads bounded ZIP/JSON/CUE/QON headers, not the video frame index or payload.
   Each handle owns independent video, analog and digital file positions.
   One video caller, one analog caller and one digital caller may run concurrently;
   callers must stop before close. Handles have no shared mutable global state. */
megald_media *megald_media_open(const char *path_utf8, char *error, size_t error_size);
void megald_media_close(megald_media *media);
const megald_media_info *megald_media_get_info(const megald_media *media);
const megald_media_side *megald_media_get_side(const megald_media *media, unsigned side);

/* 1 on success, 0 for invalid/out-of-range/truncated/corrupt media.
   read_sector has no implicit 150-sector pregap adjustment. */
int megald_media_read_sector(megald_media *media, unsigned side, uint64_t lba,
                             unsigned char sector[2352]);
/* Absolute QON frame (0-based, INCLUDING lead-in). RGB24, caller-owned output.
   row_stride must be >= width*3, output_size >= (height-1)*stride+width*3.
   Decoder scratch storage is reused, bounded to three frames. */
int megald_media_read_video(megald_media *media, unsigned side, uint32_t frame,
                            unsigned char *rgb, size_t output_size, size_t row_stride);
/* Reads up to count stereo sample frames at 44100 Hz into interleaved host-endian
   int16_t samples; returns actual frame count, 0 at EOF/error. No padding. */
size_t megald_media_read_pcm(megald_media *media, unsigned side, uint64_t first_sample,
                             int16_t *stereo, size_t count);
#ifdef __cplusplus
}
#endif
#endif
