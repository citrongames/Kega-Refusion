/*
 * Kega Refusion: allocation-free 32X scanline composition.
 * PicoDrive-derived display rules; see LICENSE and PROVENANCE.md beside this file.
 */
#ifndef KEGA32X_VIDEO_COMPOSITOR_H
#define KEGA32X_VIDEO_COMPOSITOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEGA32X_VIDEO_WIDTH 320u
#define KEGA32X_FRAMEBUFFER_WORDS 65536u
#define KEGA32X_PALETTE_COLORS 256u

typedef enum Kega32xVideoMode {
    KEGA32X_VIDEO_BLANK = 0,
    KEGA32X_VIDEO_PACKED = 1,
    KEGA32X_VIDEO_DIRECT = 2,
    KEGA32X_VIDEO_RLE = 3
} Kega32xVideoMode;

typedef struct Kega32xVideoLine {
    /* Full GPX pixel indices, including its shadow/highlight palette banks. */
    const uint8_t *md_indices;
    const uint16_t *md_palette_rgb565; /* 256 already-remapped native MD colors. */
    /* Nonzero means MD's actual /YS transparency decision, not matching RGB
     * or matching the configured backdrop palette index. Required when 32X
     * display is enabled. Opaque pixels may have the same RGB as backdrop. */
    const uint8_t *md_transparent;
    /* 320: H40 one-to-one. 256: H32 MD and /YS are nearest-neighbor expanded
     * to 320 while 32X remains 320 pixels. H32 is a deterministic approximation;
     * independent EDCLK phase and the fractional layer offset are not modeled. */
    unsigned md_width;
    /* The currently displayed 128 KiB bank, as host-endian numeric 16-bit
     * words. The caller owns bank selection and register/timing latching. */
    const uint16_t *framebuffer;
    /* Native 32X words: bit15 priority, bits14..10 blue, 9..5 green, 4..0 red.
     * Required for packed/RLE modes; index zero is an ordinary visible color. */
    const uint16_t *palette_bgr555;
    unsigned line; /* 0..255; framebuffer[line] is a 16-bit word offset. */
    Kega32xVideoMode mode;
    unsigned priority_invert; /* Nonzero is 32X VDP PRI: invert bit15. */
    unsigned pixel_shift; /* Nonzero is SFT: skip one decoded pixel, except
                          * when the line-table offset has low byte 0xff. */
} Kega32xVideoLine;

typedef enum Kega32xVideoResult {
    KEGA32X_VIDEO_OK = 0,
    KEGA32X_VIDEO_INVALID_ARGUMENT = -1
} Kega32xVideoResult;

/* Writes exactly 320 RGB565 pixels; no allocations, global state or I/O.
 * All buffers remain caller-owned and input/output buffers must not overlap.
 * Framebuffer addressing wraps within its 65536-word physical bank. RLE work
 * is bounded to 321 decoded pixels, including optional shift, for any data.
 * MD transparent OR (32X priority XOR PRI) selects 32X; otherwise MD wins.
 * Blank mode needs only MD indices/palette/width and produces the MD line.
 * Invalid arguments return before writing output. This is pre-filter output:
 * the caller must integrate any Blargg/other filter; this code changes none. */
Kega32xVideoResult kega32x_compose_rgb565(uint16_t *dst, size_t dst_pixels,
                                        const Kega32xVideoLine *line);

#ifdef __cplusplus
}
#endif
#endif
