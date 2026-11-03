/*
 * Optional RGB565 Blargg filter: Kega Refusion adaptation, 2026-11-01.
 * Derived from md_ntsc 0.1.2. Copyright (C) 2006-2007 Shay Green.
 * GPX donor identifies its adaptations as "Modified ... -- EkeEke".
 *
 * This module is free software; you can redistribute it and/or modify it
 * under the GNU Lesser General Public License, version 2.1 or later.
 * It is provided WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * third_party/blargg/COPYING.LGPL-2.1 and PROVENANCE.txt.
 *
 * Local changes: full RGB565 kernels, fixed uint32_t words, independent
 * instances, bounded caller-owned pitched output, no GPX bitmap dependency.
 */
#include "ntsc565.h"
#include <stdlib.h>

/* Private compatibility names keep the small Blargg kernel readable. */
typedef struct md_ntsc_setup_t {
    double hue, saturation, contrast, brightness, sharpness;
    double gamma, resolution, artifacts, fringing, bleed;
    const float *decoder_matrix;
} md_ntsc_setup_t;
typedef uint32_t md_ntsc_rgb_t;
enum { md_ntsc_entry_size = 32 };
#define alignment_count 2
#define burst_count 1
#define rescale_in 1
#define rescale_out 1
#define artifacts_mid 0.40f
#define fringing_mid 0.30f
#define std_decoder_hue 0
#define gamma_size 64
#define artifacts_max 1.00f
#define LUMA_CUTOFF 0.1974
#define md_ntsc_rgb_builder ((UINT32_C(1) << 21) | (UINT32_C(1) << 11) | (UINT32_C(1) << 1))
#define md_ntsc_clamp_mask (md_ntsc_rgb_builder * 3u / 2u)
#define md_ntsc_clamp_add (md_ntsc_rgb_builder * 0x101u)
#define MD_NTSC_CLAMP_(io, shift) do { \
    const uint32_t sub = (io) >> (9 - (shift)) & md_ntsc_clamp_mask; \
    uint32_t clamp = md_ntsc_clamp_add - sub; \
    (io) |= clamp; clamp -= sub; (io) &= clamp; \
} while (0)
#include "../../../third_party/blargg/md_ntsc_kernel_impl.h"

struct Kega32xNtsc565 {
    uint32_t table[65536][32];
    Kega32xNtsc565Preset preset;
    int initialized;
};
typedef char kernel_table_must_be_eight_mib[
    sizeof(((Kega32xNtsc565 *)0)->table) == KEGA32X_NTSC565_TABLE_BYTES ? 1 : -1];

/* Same values and ordering as GPX core/ntsc/md_ntsc.c. Immutable only. */
static const md_ntsc_setup_t presets[4] = {
    {0,-1,0,0,.2, 0,0,-.2,-.2,-1, NULL},
    {0,0,0,0,0, 0,0,0,0,0, NULL},
    {0,0,0,0,0, 0,.2,-1,-1,0, NULL},
    {0,0,0,0,.2, 0,.7,-1,-1,-1, NULL}
};

static int valid_preset(Kega32xNtsc565Preset preset)
{
    return preset >= KEGA32X_NTSC565_MONOCHROME && preset <= KEGA32X_NTSC565_RGB;
}

static void correct_errors(md_ntsc_rgb_t color, md_ntsc_rgb_t *out)
{
    unsigned i;
    for (i = 0; i < rgb_kernel_size / 4; ++i) {
        const md_ntsc_rgb_t error = color -
            out[i] - out[i+2+16] - out[i+4] - out[i+6+16] -
            out[i+8] - out[(i+10)%16+16] - out[(i+12)%16] - out[(i+14)%16+16];
        CORRECT_ERROR(i + 6 + 16);
    }
}

Kega32xNtsc565Result kega32x_ntsc565_setup(Kega32xNtsc565 *filter,
                                         Kega32xNtsc565Preset preset)
{
    init_t impl;
    float gamma_rb[32];
    const md_ntsc_setup_t *setup;
    float gamma;
    unsigned entry;
    if (!filter || !valid_preset(preset)) return KEGA32X_NTSC565_INVALID_ARGUMENT;
    if (filter->initialized && filter->preset == preset) return KEGA32X_NTSC565_OK;
    setup = &presets[(unsigned)preset - 1u];
    init(&impl, setup); /* Includes all 64 green levels, without dropping bit0. */
    gamma = 1.1333f - (float)setup->gamma * 0.5f;
    for (entry = 0; entry < 32; ++entry)
        gamma_rb[entry] = (float)pow(entry * (1.0f / 31.0f), gamma) * impl.contrast + impl.brightness;
    for (entry = 0; entry < 65536; ++entry) {
        const float rr = gamma_rb[entry >> 11];
        const float gg = impl.to_float[(entry >> 5) & 63u];
        const float bb = gamma_rb[entry & 31u];
        float y, i, q = RGB_TO_YIQ(rr, gg, bb, y, i);
        int r, g, b = YIQ_TO_RGB(y, i, q, impl.to_rgb, int, r, g);
        const md_ntsc_rgb_t rgb = PACK_RGB(r, g, b);
        gen_kernel(&impl, y, i, q, filter->table[entry]);
        correct_errors(rgb, filter->table[entry]);
    }
    filter->preset = preset;
    filter->initialized = 1;
    return KEGA32X_NTSC565_OK;
}

Kega32xNtsc565 *kega32x_ntsc565_create(Kega32xNtsc565Preset preset)
{
    Kega32xNtsc565 *filter;
    if (!valid_preset(preset)) return NULL;
    filter = (Kega32xNtsc565 *)malloc(sizeof(*filter));
    if (!filter) return NULL;
    filter->initialized = 0;
    filter->preset = (Kega32xNtsc565Preset)0;
    if (kega32x_ntsc565_setup(filter, preset) != KEGA32X_NTSC565_OK) {
        free(filter);
        return NULL;
    }
    return filter;
}

void kega32x_ntsc565_delete(Kega32xNtsc565 *filter) { free(filter); }

size_t kega32x_ntsc565_output_width(size_t width)
{
    return width >= 4u && width <= KEGA32X_NTSC565_MAX_INPUT_WIDTH && !(width & 1u)
        ? width * 2u : 0u;
}

/* One RGB565 output from the original eight phase contributions. */
static uint16_t rgb_out(const uint32_t *const kernel[4],
                        const uint32_t *const previous[4], unsigned x)
{
    uint32_t raw = kernel[0][x] + kernel[1][(x+6)%8+16] +
        kernel[2][(x+4)%8] + kernel[3][(x+2)%8+16] +
        previous[0][x+8] + previous[1][(x+6)%8+24] +
        previous[2][(x+4)%8+8] + previous[3][(x+2)%8+24];
    MD_NTSC_CLAMP_(raw, 0);
    return (uint16_t)((raw >> 13 & 0xf800u) | (raw >> 8 & 0x07e0u) | (raw >> 4 & 0x001fu));
}

Kega32xNtsc565Result kega32x_ntsc565_row(const Kega32xNtsc565 *filter,
    const uint16_t *input, size_t width, size_t input_capacity,
    uint16_t border, uint16_t *output, size_t output_capacity,
    size_t pitch, size_t row)
{
    const size_t out_width = kega32x_ntsc565_output_width(width);
    const uint32_t *kernel[4], *previous[4];
    uint16_t *dst;
    size_t offset, cursor, n;
    uintptr_t src_address, dst_address;
    if (!filter || !filter->initialized || !input || !output || !out_width ||
        input_capacity < width || pitch < out_width ||
        output_capacity > SIZE_MAX / sizeof(uint16_t) || row > SIZE_MAX / pitch)
        return KEGA32X_NTSC565_INVALID_ARGUMENT;
    offset = row * pitch;
    if (offset > output_capacity || out_width > output_capacity - offset)
        return KEGA32X_NTSC565_INVALID_ARGUMENT;
    dst = output + offset;
    src_address = (uintptr_t)input;
    dst_address = (uintptr_t)dst;
    if (src_address <= dst_address ? dst_address - src_address < width * sizeof(uint16_t)
                                  : src_address - dst_address < out_width * sizeof(uint16_t))
        return KEGA32X_NTSC565_INVALID_ARGUMENT;
    kernel[0] = filter->table[border];
    kernel[1] = filter->table[input[0]];
    kernel[2] = filter->table[input[1]];
    kernel[3] = filter->table[input[2]];
    previous[0] = previous[1] = previous[2] = previous[3] = kernel[0];
    cursor = 3;
#define COLOR_IN(index, color) do { \
    previous[index] = kernel[index]; kernel[index] = filter->table[color]; \
} while (0)
#define OUT(x) do { *dst++ = rgb_out(kernel, previous, x); } while (0)
    for (n = 0; n + 1u < width / 4u; ++n) {
        COLOR_IN(0, input[cursor++]); OUT(0); OUT(1);
        COLOR_IN(1, input[cursor++]); OUT(2); OUT(3);
        COLOR_IN(2, input[cursor++]); OUT(4); OUT(5);
        COLOR_IN(3, input[cursor++]); OUT(6); OUT(7);
    }
    COLOR_IN(0, input[cursor++]); OUT(0); OUT(1);
    if (width & 2u) {
        /* Horizontal overscan is 334 input pixels. Finish its two real
         * pixels, then flush the same four-phase kernel at the border. */
        COLOR_IN(1, input[cursor++]); OUT(2); OUT(3);
        COLOR_IN(2, input[cursor]); OUT(4); OUT(5);
        COLOR_IN(3, border); OUT(6); OUT(7);
        COLOR_IN(0, border); OUT(0); OUT(1);
        COLOR_IN(1, border); OUT(2); OUT(3);
    } else {
        COLOR_IN(1, border); OUT(2); OUT(3);
        COLOR_IN(2, border); OUT(4); OUT(5);
        COLOR_IN(3, border); OUT(6); OUT(7);
    }
#undef OUT
#undef COLOR_IN
    return KEGA32X_NTSC565_OK;
}
