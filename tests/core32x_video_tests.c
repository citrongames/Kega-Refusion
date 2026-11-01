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
    require(out[0] == 0 && out[1] == 1 && out[2] == 2 && out[3] == 2 &&
            out[4] == 3 && out[5] == 4 && out[318] == 254 && out[319] == 255,
            "blank H32 MD pixels must sample EDCLK centers");
    line.mode = KEGA32X_VIDEO_DIRECT;
    line.md_transparent = transparent;
    line.framebuffer = framebuffer;
    line.line = 7;
    memset(transparent, 0, sizeof(transparent));
    transparent[2] = 1;
    framebuffer[0x200] = 0x03e0;
    out = compose(&line);
    require(out[2] == 2 && out[3] == 0x07e0,
            "H32 /YS must use the MD center sample, with no 32X before its origin");
}

static void test_h32_physical_coordinates(void)
{
    Kega32xVideoLine line = prepare(KEGA32X_VIDEO_DIRECT);
    uint16_t *out;
    unsigned x;
    line.md_width = 256;
    for (x = 0; x < 256; ++x) {
        indices[x] = (uint8_t)x;
        palette_md[x] = (uint16_t)(0x1000 + x);
    }
    memset(transparent, 0, sizeof(transparent));
    /* Deliberately distinguish the left boundary and both ends of the 32X
     * stream. Negative source indexing must never expose the preceding word. */
    framebuffer[0x1ff] = 0xffff;
    framebuffer[0x200] = 0x801f;       /* x32=0: red, priority */
    framebuffer[0x201] = 0x03e0;       /* x32=1: green, behind MD */
    framebuffer[0x202] = 0xfc00;       /* x32=2: blue, priority */
    framebuffer[0x205] = 0x7fff;       /* x32=5: white, behind MD */
    framebuffer[0x200 + 77] = 0x001f;
    framebuffer[0x200 + 156] = 0x83e0;
    framebuffer[0x200 + 157] = 0x7c00;
    framebuffer[0x200 + 316] = 0xffff;
    framebuffer[0x200 + 317] = framebuffer[0x200 + 318] = framebuffer[0x200 + 319] = 0x83e0;
    transparent[0] = transparent[1] = transparent[2] = 1;
    transparent[6] = transparent[128] = 1;
    out = compose(&line);
    require(out[0] == 0x1000 && out[1] == 0x1001 && out[2] == 0x1002,
            "H32 left edge must retain MD even when /YS or 32X priority would select 32X");
    /* Hand-selected quarter-grid positions:
       x=3 sample14 -> MD2/32X0; x=4 sample18 -> MD3/32X1;
       x=8 sample34 -> MD6/32X5; x=80 sample322 -> MD64/32X77;
       x=159 sample638 -> MD127/32X156; x=160 sample642 -> MD128/32X157;
       x=319 sample1278 -> MD255/32X316 (the remaining three colors clip). */
    require(out[3] == 0xf800 && out[4] == 0x1003 && out[5] == 0x001f &&
            out[8] == 0xffff && out[80] == 0x1040 && out[159] == 0x07e0 &&
            out[160] == 0x001f && out[319] == 0xffff,
            "H32 physical coordinates, sampled /YS, per-pixel priority or right crop");
    transparent[64] = 1;
    line.priority_invert = 1;
    out = compose(&line);
    require(out[0] == 0x1000 && out[1] == 0x1001 && out[2] == 0x1002 &&
            out[4] == 0x07e0 && out[5] == 0x1004 && out[80] == 0xf800 &&
            out[159] == 0x107f && out[160] == 0x001f && out[319] == 0x10ff,
            "H32 PRI inversion must preserve point-sampled MD /YS and absent left edge");
    line.priority_invert = 0;
    line.pixel_shift = 1;
    require(compose(&line)[3] == 0x07e0, "H32 phase must not replace the independent SFT pixel skip");
}

static void test_frozen_decode_and_md_remap(void)
{
    Kega32xVideoLine line = prepare(KEGA32X_VIDEO_PACKED);
    Kega32xVideoLine source = line;
    Kega32xDecodedLine decoded, frozen;
    uint16_t *out = guarded_output + 1;
    framebuffer[0x200] = 0x0102;
    palette32x[1] |= 0x8000; /* red ahead of opaque MD; green behind it */
    memset(transparent, 0, sizeof(transparent));
    indices[0] = indices[1] = 0xc2;
    palette_md[0xc2] = 0x1234;
    /* Decode has no dependence on the MD snapshot. */
    source.md_indices = NULL;
    source.md_palette_rgb565 = NULL;
    source.md_transparent = NULL;
    source.md_width = 0;
    require(kega32x_decode_line(&decoded, &source) == KEGA32X_VIDEO_OK,
            "decode incorrectly required MD inputs");
    require(decoded.enabled == 1 && decoded.priority_invert == 0 &&
            decoded.colors[0] == 0x801f && decoded.colors[1] == 0x03e0,
            "frozen row did not preserve native color and priority bits");
    frozen = decoded;
    /* A later register write, framebuffer edit and palette edit must not
     * leak into repeated GPX MD remaps of the already latched 32X row. */
    framebuffer[line.line] = 0x300;
    framebuffer[0x200] = 0x0304;
    palette32x[1] = palette32x[2] = 0;
    line.priority_invert = 1;
    line.mode = KEGA32X_VIDEO_BLANK;
    line.framebuffer = NULL;
    line.palette_bgr555 = NULL;
    line.line = UINT_MAX;
    require(kega32x_mix_rgb565(out, 320, &decoded, &line) == KEGA32X_VIDEO_OK &&
            out[0] == 0xf800 && out[1] == 0x1234,
            "live 32X storage/registers changed a frozen row");
    palette_md[0xc2] = 0xabcd;
    require(kega32x_mix_rgb565(out, 320, &decoded, &line) == KEGA32X_VIDEO_OK &&
            out[0] == 0xf800 && out[1] == 0xabcd,
            "MD palette remap failed to retain frozen 32X priority");
    transparent[1] = 1;
    require(kega32x_mix_rgb565(out, 320, &decoded, &line) == KEGA32X_VIDEO_OK &&
            out[1] == 0x07e0, "changed MD /YS did not reveal frozen 32X color");
    transparent[1] = 0;
    indices[1] = 0x82;
    palette_md[0x82] = 0x4567;
    require(kega32x_mix_rgb565(out, 320, &decoded, &line) == KEGA32X_VIDEO_OK &&
            out[1] == 0x4567, "MD indexed remap ignored the new full palette bank");
    require(!memcmp(&decoded, &frozen, sizeof(decoded)), "mix changed frozen 32X data");
    require(guarded_output[0] == 0x5aa5 && guarded_output[321] == 0x5aa5,
            "frozen mix wrote outside its row");
}

static void test_split_validation(void)
{
    Kega32xVideoLine line = prepare(KEGA32X_VIDEO_PACKED);
    Kega32xDecodedLine decoded, before;
    uint16_t output_before[KEGA32X_VIDEO_WIDTH + 2u];
    memset(&decoded, 0x5a, sizeof(decoded));
    before = decoded;
    line.line = 256;
    require(kega32x_decode_line(&decoded, &line) == KEGA32X_VIDEO_INVALID_ARGUMENT &&
            !memcmp(&decoded, &before, sizeof(decoded)), "invalid decode changed its destination");
    require(kega32x_decode_line(NULL, &line) == KEGA32X_VIDEO_INVALID_ARGUMENT &&
            kega32x_decode_line(&decoded, NULL) == KEGA32X_VIDEO_INVALID_ARGUMENT,
            "null decode input accepted");
    line.mode = KEGA32X_VIDEO_BLANK;
    line.framebuffer = line.palette_bgr555 = NULL;
    require(kega32x_decode_line(&decoded, &line) == KEGA32X_VIDEO_OK &&
            !decoded.enabled && !decoded.priority_invert,
            "blank decode required unused memory or left active flags");
    line.md_transparent = NULL;
    require(kega32x_mix_rgb565(guarded_output + 1, 320, &decoded, &line) == KEGA32X_VIDEO_OK,
            "disabled frozen row required unused /YS");
    memcpy(output_before, guarded_output, sizeof(output_before));
    decoded.enabled = 1;
    require(kega32x_mix_rgb565(guarded_output + 1, 320, &decoded, &line) == KEGA32X_VIDEO_INVALID_ARGUMENT &&
            !memcmp(output_before, guarded_output, sizeof(output_before)),
            "invalid mix partially wrote output");
    decoded.enabled = 2;
    line.md_transparent = transparent;
    require(kega32x_mix_rgb565(guarded_output + 1, 320, &decoded, &line) == KEGA32X_VIDEO_INVALID_ARGUMENT &&
            !memcmp(output_before, guarded_output, sizeof(output_before)),
            "invalid frozen flags accepted or partially wrote output");
    decoded.enabled = 0;
    require(kega32x_mix_rgb565(guarded_output + 1, 319, &decoded, &line) == KEGA32X_VIDEO_INVALID_ARGUMENT &&
            !memcmp(output_before, guarded_output, sizeof(output_before)),
            "short mix output accepted or partially wrote output");
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
    test_h32_physical_coordinates();
    test_validation();
    test_frozen_decode_and_md_remap();
    test_split_validation();
    puts("32X compositor: modes, /YS, priority, shifts, H32, bounded reads and frozen-row MD remaps passed");
    return 0;
}
