/*
 * Kega Refusion: isolated 32X video compositor, 2026.
 * Display decoding/priority rules adapted from PicoDrive pico/32x/draw.c:
 * (C) notaz, 2009,2010
 * (C) irixxxx, 2019-2026
 * Licensed under the accompanying noncommercial MAME-style LICENSE.
 * See PROVENANCE.md for donor revision, local changes and integration limits.
 */
#include "compositor.h"

static uint16_t bgr555_to_rgb565(uint16_t color)
{
    const unsigned green = (color >> 5) & 31u;
    /* Replicate the high green bit: full white stays 0xffff. Priority is
     * tested separately and never leaks into the output color. */
    return (uint16_t)(((color & 31u) << 11) |
                      (((green << 1) | (green >> 4)) << 5) |
                      ((color >> 10) & 31u));
}

static unsigned md_position(const Kega32xVideoLine *line, unsigned x)
{
    return line->md_width == KEGA32X_VIDEO_WIDTH ? x : (x * 4u + 2u) / 5u;
}

Kega32xVideoResult kega32x_decode_line(Kega32xDecodedLine *decoded,
                                      const Kega32xVideoLine *line)
{
    unsigned x, offset, shift;
    if (!decoded || !line || line->mode < KEGA32X_VIDEO_BLANK ||
        line->mode > KEGA32X_VIDEO_RLE)
        return KEGA32X_VIDEO_INVALID_ARGUMENT;
    if (line->mode == KEGA32X_VIDEO_BLANK) {
        decoded->enabled = decoded->priority_invert = 0;
        return KEGA32X_VIDEO_OK;
    }
    if (!line->framebuffer || line->line > 255u ||
        (line->mode != KEGA32X_VIDEO_DIRECT && !line->palette_bgr555))
        return KEGA32X_VIDEO_INVALID_ARGUMENT;

    decoded->enabled = 1;
    decoded->priority_invert = line->priority_invert != 0;
    offset = line->framebuffer[line->line];
    shift = line->pixel_shift && (offset & 255u) != 255u;
    switch (line->mode) {
    case KEGA32X_VIDEO_PACKED:
        for (x = 0; x < KEGA32X_VIDEO_WIDTH; ++x) {
            const unsigned byte = x + shift;
            const uint16_t pair = line->framebuffer[(offset + (byte >> 1)) & 65535u];
            const unsigned index = (pair >> ((byte & 1u) ? 0 : 8)) & 255u;
            decoded->colors[x] = line->palette_bgr555[index];
        }
        break;
    case KEGA32X_VIDEO_DIRECT:
        for (x = 0; x < KEGA32X_VIDEO_WIDTH; ++x)
            decoded->colors[x] = line->framebuffer[(offset + x + shift) & 65535u];
        break;
    case KEGA32X_VIDEO_RLE: {
        unsigned remaining = 0;
        uint16_t color = 0;
        if (shift) {
            const uint16_t run = line->framebuffer[offset++ & 65535u];
            remaining = run >> 8; /* (length + 1) minus the skipped pixel. */
            color = line->palette_bgr555[run & 255u];
        }
        for (x = 0; x < KEGA32X_VIDEO_WIDTH; ++x) {
            if (!remaining) {
                const uint16_t run = line->framebuffer[offset++ & 65535u];
                remaining = (run >> 8) + 1u;
                color = line->palette_bgr555[run & 255u];
            }
            --remaining;
            decoded->colors[x] = color;
        }
        break;
    }
    default:
        break; /* Validated before any output; blank was handled above. */
    }
    return KEGA32X_VIDEO_OK;
}

Kega32xVideoResult kega32x_mix_rgb565(uint16_t *dst, size_t dst_pixels,
                                     const Kega32xDecodedLine *decoded,
                                     const Kega32xVideoLine *line)
{
    unsigned x;
    if (!dst || !decoded || !line || dst_pixels < KEGA32X_VIDEO_WIDTH ||
        decoded->enabled > 1u || decoded->priority_invert > 1u ||
        !line->md_indices || !line->md_palette_rgb565 ||
        (line->md_width != 256u && line->md_width != KEGA32X_VIDEO_WIDTH) ||
        (decoded->enabled && !line->md_transparent))
        return KEGA32X_VIDEO_INVALID_ARGUMENT;
    if (!decoded->enabled) {
        for (x = 0; x < KEGA32X_VIDEO_WIDTH; ++x)
            dst[x] = line->md_palette_rgb565[line->md_indices[md_position(line, x)]];
        return KEGA32X_VIDEO_OK;
    }
    for (x = 0; x < KEGA32X_VIDEO_WIDTH; ++x) {
        const unsigned md_x = md_position(line, x);
        unsigned x32 = x;
        uint16_t color;
        unsigned priority;
        if (line->md_width == 256u) {
            /* Quarter-H40 grid, sampled at EDCLK pixel centers. The 32X
             * origin is 13 units after MD; guard before unsigned subtraction. */
            const unsigned sample = 4u * x + 2u;
            if (sample < 13u) {
                dst[x] = line->md_palette_rgb565[line->md_indices[md_x]];
                continue;
            }
            x32 = (sample - 13u) / 4u;
        }
        color = decoded->colors[x32];
        priority = ((color >> 15) & 1u) ^ decoded->priority_invert;
        dst[x] = line->md_transparent[md_x] || priority ? bgr555_to_rgb565(color) :
            line->md_palette_rgb565[line->md_indices[md_x]];
    }
    return KEGA32X_VIDEO_OK;
}

Kega32xVideoResult kega32x_compose_rgb565(uint16_t *dst, size_t dst_pixels,
                                        const Kega32xVideoLine *line)
{
    Kega32xDecodedLine decoded;
    Kega32xVideoResult result;
    if (!dst || dst_pixels < KEGA32X_VIDEO_WIDTH)
        return KEGA32X_VIDEO_INVALID_ARGUMENT;
    result = kega32x_decode_line(&decoded, line);
    if (result != KEGA32X_VIDEO_OK) return result;
    return kega32x_mix_rgb565(dst, dst_pixels, &decoded, line);
}
