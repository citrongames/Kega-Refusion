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
    return line->md_width == KEGA32X_VIDEO_WIDTH ? x : (x * 4u) / 5u;
}

static void put_pixel(uint16_t *dst, const Kega32xVideoLine *line,
                      unsigned x, uint16_t color)
{
    const unsigned md_x = md_position(line, x);
    const unsigned priority = ((color >> 15) & 1u) ^ (line->priority_invert != 0);
    if (line->md_transparent[md_x] || priority)
        dst[x] = bgr555_to_rgb565(color);
    else
        dst[x] = line->md_palette_rgb565[line->md_indices[md_x]];
}

Kega32xVideoResult kega32x_compose_rgb565(uint16_t *dst, size_t dst_pixels,
                                        const Kega32xVideoLine *line)
{
    unsigned x, offset, shift;
    if (!dst || !line || dst_pixels < KEGA32X_VIDEO_WIDTH ||
        !line->md_indices || !line->md_palette_rgb565 ||
        (line->md_width != 256u && line->md_width != KEGA32X_VIDEO_WIDTH) ||
        line->mode < KEGA32X_VIDEO_BLANK || line->mode > KEGA32X_VIDEO_RLE)
        return KEGA32X_VIDEO_INVALID_ARGUMENT;
    if (line->mode == KEGA32X_VIDEO_BLANK) {
        for (x = 0; x < KEGA32X_VIDEO_WIDTH; ++x)
            dst[x] = line->md_palette_rgb565[line->md_indices[md_position(line, x)]];
        return KEGA32X_VIDEO_OK;
    }
    if (!line->md_transparent || !line->framebuffer || line->line > 255u ||
        (line->mode != KEGA32X_VIDEO_DIRECT && !line->palette_bgr555))
        return KEGA32X_VIDEO_INVALID_ARGUMENT;

    offset = line->framebuffer[line->line];
    shift = line->pixel_shift && (offset & 255u) != 255u;
    switch (line->mode) {
    case KEGA32X_VIDEO_PACKED:
        for (x = 0; x < KEGA32X_VIDEO_WIDTH; ++x) {
            const unsigned byte = x + shift;
            const uint16_t pair = line->framebuffer[(offset + (byte >> 1)) & 65535u];
            const unsigned index = (pair >> ((byte & 1u) ? 0 : 8)) & 255u;
            put_pixel(dst, line, x, line->palette_bgr555[index]);
        }
        break;
    case KEGA32X_VIDEO_DIRECT:
        for (x = 0; x < KEGA32X_VIDEO_WIDTH; ++x)
            put_pixel(dst, line, x, line->framebuffer[(offset + x + shift) & 65535u]);
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
            put_pixel(dst, line, x, color);
        }
        break;
    }
    default:
        break; /* Validated before any output; blank was handled above. */
    }
    return KEGA32X_VIDEO_OK;
}
