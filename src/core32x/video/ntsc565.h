/*
 * Optional full-RGB565 Blargg MD filter for Kega Refusion, 2026-11-01.
 * Derived from md_ntsc 0.1.2, Copyright (C) 2006-2007 Shay Green.
 * LGPL 2.1 or later; see third_party/blargg/PROVENANCE.txt and COPYING.LGPL-2.1.
 */
#ifndef KEGA32X_VIDEO_NTSC565_H
#define KEGA32X_VIDEO_NTSC565_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEGA32X_NTSC565_TABLE_BYTES (65536u * 32u * 4u)
#define KEGA32X_NTSC565_MAX_INPUT_WIDTH 512u

typedef struct Kega32xNtsc565 Kega32xNtsc565;
/* Values match the existing user-facing Blargg selection; disabled creates no
 * instance. These presets retain GPX's current coefficients. */
typedef enum Kega32xNtsc565Preset {
    KEGA32X_NTSC565_MONOCHROME = 1,
    KEGA32X_NTSC565_COMPOSITE = 2,
    KEGA32X_NTSC565_SVIDEO = 3,
    KEGA32X_NTSC565_RGB = 4
} Kega32xNtsc565Preset;

typedef enum Kega32xNtsc565Result {
    KEGA32X_NTSC565_OK = 0,
    KEGA32X_NTSC565_INVALID_ARGUMENT = -1
} Kega32xNtsc565Result;

/* One allocation: the fixed 8 MiB kernel plus small instance metadata. Kernel
 * generation happens here or on a changed setup only, never while filtering.
 * NULL indicates invalid preset or allocation failure. Delete accepts NULL. */
Kega32xNtsc565 *kega32x_ntsc565_create(Kega32xNtsc565Preset preset);
Kega32xNtsc565Result kega32x_ntsc565_setup(Kega32xNtsc565 *, Kega32xNtsc565Preset preset);
void kega32x_ntsc565_delete(Kega32xNtsc565 *);

/* The 32X uses MD/H40 chroma timing: 4 input -> 8 output pixels. Width must
 * be a multiple of 4 in [4,512]; otherwise zero is returned. Thus 320->640,
 * or 348->696 when the caller explicitly supplies MD overscan borders.
 * Native display aspect/width metadata must continue to describe the input
 * raster, not the horizontally oversampled filter output. H32 resampling is
 * the compositor/caller's policy, not performed by this filter. */
size_t kega32x_ntsc565_output_width(size_t input_width);

/* Filter one RGB565 row into a caller-owned pitched RGB565 image. All capacity
 * and pitch values are in uint16_t pixels, never bytes. Image/row and input
 * capacity are validated before any write; input and this output row must not
 * overlap. Exactly output_width pixels are written, padding is preserved.
 * Border is the caller's RGB565 edge/backdrop color. Input capacity is the
 * number of readable input pixels; output capacity covers the complete image.
 * No row allocation, initialization, I/O or mutation of the filter occurs.
 * Distinct instances are independent. Do not setup/delete concurrently with a
 * row call on the same instance. This component changes no GPX settings. */
Kega32xNtsc565Result kega32x_ntsc565_row(const Kega32xNtsc565 *,
    const uint16_t *input, size_t input_width, size_t input_capacity,
    uint16_t border, uint16_t *output_image, size_t output_capacity,
    size_t pitch_pixels, size_t output_row);

#ifdef __cplusplus
}
#endif
#endif
