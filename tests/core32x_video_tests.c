/* Original deterministic fixtures for the isolated 32X compositor. */
#include "core32x/video/compositor.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t framebuffer[KEGA32X_FRAMEBUFFER_WORDS];
static uint16_t palette32x[KEGA32X_PALETTE_COLORS];
static uint16_t palette_md[KEGA32X_PALETTE_COLORS];
static uint8_t indices[KEGA32X_VIDEO_WIDTH];
static uint8_t transparent[KEGA32X_VIDEO_WIDTH];
static uint16_t guarded_output[KEGA32X_VIDEO_WIDTH + 2u];

static void require(int okay, const char *message)
{
    if (!okay) {
        fprintf(stderr, "32X compositor fixture failed: %s\n", message);
        exit(1);
    }
}

static Kega32xVideoLine prepare(Kega32xVideoMode mode)
{
    Kega32xVideoLine line;
    unsigned x;
    memset(framebuffer, 0, sizeof(framebuffer));
    memset(palette32x, 0, sizeof(palette32x));
    memset(palette_md, 0, sizeof(palette_md));
    memset(indices, 0, sizeof(indices));
    memset(transparent, 1, sizeof(transparent));
    for (x = 0; x < KEGA32X_VIDEO_WIDTH + 2u; ++x)
        guarded_output[x] = 0x5aa5;
    memset(&line, 0, sizeof(line));
    line.md_indices = indices;
    line.md_palette_rgb565 = palette_md;
    line.md_transparent = transparent;
    line.md_width = 320;
    line.framebuffer = framebuffer;
    line.palette_bgr555 = palette32x;
    line.line = 7;
    line.mode = mode;
    framebuffer[line.line] = 0x200;
    palette32x[1] = 0x001f; /* red */
    palette32x[2] = 0x03e0; /* green */
    palette32x[3] = 0x7c00; /* blue */
    palette32x[4] = 0x7fff; /* white */
    return line;
}

static uint16_t *compose(const Kega32xVideoLine *line)
{
    require(kega32x_compose_rgb565(guarded_output + 1, 320, line) == KEGA32X_VIDEO_OK,
            "valid line rejected");
    require(guarded_output[0] == 0x5aa5 && guarded_output[321] == 0x5aa5,
            "scanline wrote outside 320 pixels");
    return guarded_output + 1;
}

static void test_direct_and_priority(void)
{
    Kega32xVideoLine line = prepare(KEGA32X_VIDEO_DIRECT);
    uint16_t *out;
    framebuffer[0x200] = 0x001f;
    framebuffer[0x201] = 0x03e0;
    framebuffer[0x202] = 0x7c00;
    framebuffer[0x203] = 0xffff;
    out = compose(&line);
    require(out[0] == 0xf800 && out[1] == 0x07e0 && out[2] == 0x001f && out[3] == 0xffff,
            "BGR555/RGB565 channels, green expansion or priority color stripping");
    /* Explicit /YS wins over matching palette indices or RGB colors. Full
     * GPX indices include shadow/highlight, so 0xc2 must not become 0x02. */
    memset(transparent, 0, sizeof(transparent));
    indices[0] = 0xc2;
    indices[1] = 0xc2;
    indices[2] = 0xc2;
    indices[3] = 0xc2;
    palette_md[0xc2] = 0x1234;
    palette_md[0x02] = 0xabcd;
    framebuffer[0x200] = 0x001f;
    framebuffer[0x201] = 0x801f;
    framebuffer[0x202] = 0x001f;
    framebuffer[0x203] = 0x801f;
    transparent[2] = transparent[3] = 1;
    out = compose(&line);
    require(out[0] == 0x1234 && out[1] == 0xf800 && out[2] == 0xf800 && out[3] == 0xf800,
            "normal per-pixel priority or explicit MD transparency");
    line.priority_invert = 1;
    out = compose(&line);
    require(out[0] == 0xf800 && out[1] == 0x1234 && out[2] == 0xf800 && out[3] == 0xf800,
            "PRI inversion must not hide 32X through transparent MD");
    line.priority_invert = 0;
    palette_md[0] = palette_md[5] = 0x001f;
    indices[0] = 0;
    indices[1] = 5;
    framebuffer[0x200] = framebuffer[0x201] = 0x03e0;
    out = compose(&line);
    require(out[0] == 0x001f && out[1] == 0x001f,
            "matching backdrop indices/RGB incorrectly made opaque MD transparent");
    transparent[0] = 1;
    framebuffer[0x200] = 0;
    out = compose(&line);
    require(out[0] == 0, "32X color zero is visible black, not an alpha key");
}

static void test_packed(void)
{
    Kega32xVideoLine line = prepare(KEGA32X_VIDEO_PACKED);
    uint16_t *out;
    framebuffer[0x200] = 0x0102;
    framebuffer[0x201] = 0x0304;
    out = compose(&line);
    require(out[0] == 0xf800 && out[1] == 0x07e0 && out[2] == 0x001f && out[3] == 0xffff,
            "packed pixels must read high byte first");
    memset(transparent, 0, sizeof(transparent));
    palette_md[0] = 0x1234;
    palette32x[2] |= 0x8000;
    out = compose(&line);
    require(out[0] == 0x1234 && out[1] == 0x07e0,
            "packed priority must come from palette bit15");
    line.priority_invert = 1;
    out = compose(&line);
    require(out[0] == 0xf800 && out[1] == 0x1234, "packed PRI inversion");
    memset(transparent, 1, sizeof(transparent));
    line.pixel_shift = 1;
    out = compose(&line);
    require(out[0] == 0x07e0 && out[1] == 0x001f && out[2] == 0xffff,
            "packed shift skips one pixel, not one word");
}

static void test_rle(void)
{
    Kega32xVideoLine line = prepare(KEGA32X_VIDEO_RLE);
    uint16_t *out;
    unsigned x;
    framebuffer[0x200] = 0x0001; /* one red */
    framebuffer[0x201] = 0x0102; /* two green */
    framebuffer[0x202] = 0xff03; /* 256 blue */
    framebuffer[0x203] = 0xff04; /* rest white; crop the run to output */
    out = compose(&line);
    require(out[0] == 0xf800 && out[1] == 0x07e0 && out[2] == 0x07e0 &&
            out[3] == 0x001f && out[258] == 0x001f && out[259] == 0xffff && out[319] == 0xffff,
            "RLE length-plus-one, palette decoding or final-run crop");
    line.pixel_shift = 1;
    out = compose(&line);
    require(out[0] == 0x07e0 && out[1] == 0x07e0 && out[2] == 0x001f && out[258] == 0xffff,
            "RLE shift must consume exactly one decoded pixel across runs");
    memset(transparent, 0, sizeof(transparent));
    palette_md[0] = 0x1234;
    palette32x[2] |= 0x8000;
    out = compose(&line);
    require(out[0] == 0x07e0 && out[2] == 0x1234, "RLE per-run palette priority");
    line.priority_invert = 1;
    out = compose(&line);
    require(out[0] == 0x1234 && out[2] == 0x001f, "RLE global priority inversion");
    memset(transparent, 1, sizeof(transparent));
    for (x = 0; x < KEGA32X_FRAMEBUFFER_WORDS; ++x) framebuffer[x] = 0x0001;
    framebuffer[line.line] = 0xfffe;
    line.pixel_shift = 0;
    out = compose(&line);
    for (x = 0; x < 320; ++x)
        /* The wrapped stream also encounters line-table word 7 (0xfffe),
         * which encodes a 256-pixel run of palette entry 254 (black). */
        require(out[x] == (x >= 9 && x <= 264 ? 0 : 0xf800), "RLE minimum runs or bank wrap");
}

static void test_address_wrap_and_shift_exception(void)
{
    Kega32xVideoLine line = prepare(KEGA32X_VIDEO_DIRECT);
    uint16_t *out;
    line.line = 1;
    framebuffer[1] = 0xffff;
    framebuffer[0xffff] = 0x001f;
    framebuffer[0] = 0x03e0;
    line.pixel_shift = 1;
    out = compose(&line);
    require(out[0] == 0xf800 && out[1] == 0x07e0, "direct 0xff shift exception or bank wrap");
    line.mode = KEGA32X_VIDEO_PACKED;
    framebuffer[0xffff] = 0x0102;
    framebuffer[0] = 0x0304;
    out = compose(&line);
    require(out[0] == 0xf800 && out[1] == 0x07e0 && out[2] == 0x001f && out[3] == 0xffff,
            "packed 0xff shift exception or bank wrap");
    line.mode = KEGA32X_VIDEO_RLE;
    framebuffer[0xffff] = 0x0001;
    framebuffer[0] = 0x0002;
    out = compose(&line);
    require(out[0] == 0xf800 && out[1] == 0x07e0, "RLE 0xff shift exception or bank wrap");
    line = prepare(KEGA32X_VIDEO_DIRECT);
    framebuffer[0x200] = 0x001f;
    framebuffer[0x201] = 0x03e0;
    line.pixel_shift = 1;
    require(compose(&line)[0] == 0x07e0, "direct shift must skip one word");
}

static void test_h32_and_blank(void)
{
    Kega32xVideoLine line = prepare(KEGA32X_VIDEO_BLANK);
    uint16_t *out;
    unsigned x;
    for (x = 0; x < 256; ++x) indices[x] = (uint8_t)x, palette_md[x] = (uint16_t)x;
    line.md_width = 256;
    line.md_transparent = NULL;
    line.framebuffer = NULL;
    line.palette_bgr555 = NULL;
    line.line = UINT_MAX;
    out = compose(&line);
    require(out[0] == 0 && out[1] == 0 && out[4] == 3 && out[5] == 4 &&
            out[318] == 254 && out[319] == 255, "declared H32 nearest-neighbor policy");
    line.mode = KEGA32X_VIDEO_DIRECT;
    line.md_transparent = transparent;
    line.framebuffer = framebuffer;
    line.line = 7;
    memset(transparent, 0, sizeof(transparent));
    transparent[1] = 1;
    framebuffer[0x202] = 0x03e0;
    out = compose(&line);
    require(out[1] == 0 && out[2] == 0x07e0, "H32 /YS must use the MD pixel sampling position");
}

static void reject_without_write(Kega32xVideoLine *line, size_t count)
{
    uint16_t before[KEGA32X_VIDEO_WIDTH + 2u];
    memcpy(before, guarded_output, sizeof(before));
    require(kega32x_compose_rgb565(guarded_output + 1, count, line) == KEGA32X_VIDEO_INVALID_ARGUMENT,
            "invalid input accepted");
    require(!memcmp(before, guarded_output, sizeof(before)), "invalid input partially wrote output");
}

static void test_validation(void)
{
    Kega32xVideoLine line = prepare(KEGA32X_VIDEO_PACKED);
    reject_without_write(&line, 319);
    line.line = 256;
    reject_without_write(&line, 320);
    line.line = 7;
    line.md_width = 0;
    reject_without_write(&line, 320);
    line.md_width = 320;
    line.mode = (Kega32xVideoMode)4;
    reject_without_write(&line, 320);
    line.mode = KEGA32X_VIDEO_PACKED;
    line.palette_bgr555 = NULL;
    reject_without_write(&line, 320);
    line.mode = KEGA32X_VIDEO_DIRECT;
    line.md_transparent = NULL;
    reject_without_write(&line, 320);
    require(kega32x_compose_rgb565(NULL, 320, &line) == KEGA32X_VIDEO_INVALID_ARGUMENT,
            "null output accepted");
    require(kega32x_compose_rgb565(guarded_output + 1, 320, NULL) == KEGA32X_VIDEO_INVALID_ARGUMENT,
            "null descriptor accepted");
}

int main(void)
{
    test_direct_and_priority();
    test_packed();
    test_rle();
    test_address_wrap_and_shift_exception();
    test_h32_and_blank();
    test_validation();
    puts("32X compositor: modes, /YS, priority, color, shifts, H32 policy and bounded bank reads passed");
    return 0;
}
