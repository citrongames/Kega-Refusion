/* Original focused fixtures for the optional RGB565 Blargg component. */
#include "core32x/video/ntsc565.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int okay, const char *message)
{
    if (!okay) {
        fprintf(stderr, "32X RGB565 NTSC fixture failed: %s\n", message);
        exit(1);
    }
}

static unsigned red(uint16_t p) { return p >> 11; }
static unsigned green(uint16_t p) { return (p >> 5) & 63u; }
static unsigned blue(uint16_t p) { return p & 31u; }
static unsigned difference(unsigned a, unsigned b) { return a > b ? a-b : b-a; }

static void constant(Kega32xNtsc565 *filter, uint16_t color, uint16_t out[640])
{
    uint16_t input[320];
    size_t x;
    for (x = 0; x < 320; ++x) input[x] = color;
    require(kega32x_ntsc565_row(filter, input, 320, 320, color, out, 640, 640, 0) == KEGA32X_NTSC565_OK,
            "constant row rejected");
    for (x = 40; x < 600; ++x)
        require(out[x] == out[40], "constant input acquired a phase/checker pattern");
}

static void test_colors_and_presets(Kega32xNtsc565 *filter)
{
    uint16_t out[640], before[640], next[640], input[320];
    Kega32xNtsc565 *independent;
    size_t x;
    constant(filter, 0, out);
    require(out[100] == 0, "black gained a bias");
    constant(filter, 0xffff, out);
    require(out[100] == 0xffff, "white lost full RGB565 range");
    constant(filter, 0xf800, out);
    require(red(out[100]) >= 30 && green(out[100]) <= 1 && blue(out[100]) <= 1,
            "red channel or RGB565 byte order");
    constant(filter, 0x07e0, out);
    require(green(out[100]) >= 62 && red(out[100]) <= 1 && blue(out[100]) <= 1,
            "green channel or six-bit range");
    constant(filter, 0x001f, out);
    require(blue(out[100]) >= 30 && green(out[100]) <= 1 && red(out[100]) <= 1,
            "blue channel or RGB565 byte order");
    constant(filter, 0x0400, before);
    constant(filter, 0x0420, next);
    require(green(next[100]) > green(before[100]), "green least significant bit was quantized away");
    for (x = 0; x < 320; ++x) input[x] = x & 1u ? 0xf800 : 0x001f;
    require(kega32x_ntsc565_row(filter, input, 320, 320, 0, before, 640, 640, 0) == KEGA32X_NTSC565_OK,
            "RGB chroma row rejected");
    require(kega32x_ntsc565_setup(filter, KEGA32X_NTSC565_COMPOSITE) == KEGA32X_NTSC565_OK,
            "composite setup");
    require(kega32x_ntsc565_row(filter, input, 320, 320, 0, next, 640, 640, 0) == KEGA32X_NTSC565_OK &&
            memcmp(before, next, sizeof(before)), "composite preset did not affect chroma");
    require(kega32x_ntsc565_setup(filter, KEGA32X_NTSC565_SVIDEO) == KEGA32X_NTSC565_OK,
            "S-Video setup");
    require(kega32x_ntsc565_row(filter, input, 320, 320, 0, out, 640, 640, 0) == KEGA32X_NTSC565_OK &&
            memcmp(next, out, sizeof(out)), "S-Video preset did not affect chroma");
    require(kega32x_ntsc565_setup(filter, KEGA32X_NTSC565_MONOCHROME) == KEGA32X_NTSC565_OK,
            "monochrome setup");
    constant(filter, 0x001f, out);
    require(difference(red(out[100]), blue(out[100])) <= 1 &&
            difference(green(out[100]), red(out[100])*2u) <= 2,
            "monochrome output retained color");
    independent = kega32x_ntsc565_create(KEGA32X_NTSC565_RGB);
    require(independent != NULL, "independent instance allocation");
    constant(independent, 0x001f, next);
    require(blue(next[100]) >= 30 && red(next[100]) <= 1,
            "another instance changed the active preset");
    kega32x_ntsc565_delete(independent);
    require(kega32x_ntsc565_setup(filter, KEGA32X_NTSC565_RGB) == KEGA32X_NTSC565_OK, "restore RGB");
    constant(filter, 0x9a67, before);
    require(kega32x_ntsc565_setup(filter, KEGA32X_NTSC565_RGB) == KEGA32X_NTSC565_OK, "same preset setup");
    constant(filter, 0x9a67, out);
    require(!memcmp(before, out, sizeof(before)), "same preset changed output");
    require(kega32x_ntsc565_setup(filter, (Kega32xNtsc565Preset)0) == KEGA32X_NTSC565_INVALID_ARGUMENT,
            "disabled/invalid preset accepted");
    constant(filter, 0x9a67, out);
    require(!memcmp(before, out, sizeof(before)), "invalid setup damaged working kernels");
}

static void test_geometry_bounds_and_border(Kega32xNtsc565 *filter)
{
    uint16_t input[512], image[2052], before[2052], black_edge[640], red_edge[640];
    uint16_t small[4] = {0xffff, 0xffff, 0xffff, 0xffff};
    size_t i;
    for (i = 0; i < 512; ++i) input[i] = 0xffff;
    for (i = 0; i < 2052; ++i) image[i] = 0x5aa5;
    require(KEGA32X_NTSC565_TABLE_BYTES == 8388608u, "kernel storage budget changed");
    require(kega32x_ntsc565_output_width(320) == 640 && kega32x_ntsc565_output_width(348) == 696 &&
            kega32x_ntsc565_output_width(512) == 1024, "output geometry");
    require(!kega32x_ntsc565_output_width(0) && !kega32x_ntsc565_output_width(3) &&
            !kega32x_ntsc565_output_width(319) && !kega32x_ntsc565_output_width(516) &&
            !kega32x_ntsc565_output_width(SIZE_MAX), "invalid geometry accepted");
    require(kega32x_ntsc565_row(filter, input, 320, 512, 0xffff, image, 2052, 642, 1) == KEGA32X_NTSC565_OK,
            "pitched image row rejected");
    for (i = 0; i < 642; ++i) require(image[i] == 0x5aa5, "preceding row overwritten");
    for (i = 642; i < 1282; ++i) require(image[i] == 0xffff, "pitched constant row changed");
    for (i = 1282; i < 2052; ++i) require(image[i] == 0x5aa5, "row padding or later row overwritten");
    require(kega32x_ntsc565_row(filter, small, 4, 4, 0xffff, image, 8, 8, 0) == KEGA32X_NTSC565_OK,
            "minimum width or exact input capacity failed");
    require(kega32x_ntsc565_row(filter, input, 512, 512, 0xffff, image, 1024, 1024, 0) == KEGA32X_NTSC565_OK,
            "maximum width or exact output capacity failed");
    memcpy(before, image, sizeof(before));
#define REJECT(call) do { \
    require((call) == KEGA32X_NTSC565_INVALID_ARGUMENT, "invalid row accepted"); \
    require(!memcmp(before, image, sizeof(before)), "invalid row partially wrote output"); \
} while (0)
    REJECT(kega32x_ntsc565_row(filter, input, 320, 319, 0, image, 2052, 640, 0));
    REJECT(kega32x_ntsc565_row(filter, input, 320, 512, 0, image, 639, 640, 0));
    REJECT(kega32x_ntsc565_row(filter, input, 320, 512, 0, image, 2052, 639, 0));
    REJECT(kega32x_ntsc565_row(filter, input, 320, 512, 0, image, 2052, 640, SIZE_MAX));
    REJECT(kega32x_ntsc565_row(filter, input, 320, 512, 0, image, 2052, 0, 0));
    REJECT(kega32x_ntsc565_row(filter, input, 320, 512, 0, image, SIZE_MAX, 640, 0));
    REJECT(kega32x_ntsc565_row(filter, input, 318, 512, 0, image, 2052, 640, 0));
    REJECT(kega32x_ntsc565_row(NULL, input, 320, 512, 0, image, 2052, 640, 0));
    REJECT(kega32x_ntsc565_row(filter, NULL, 320, 512, 0, image, 2052, 640, 0));
    REJECT(kega32x_ntsc565_row(filter, image, 320, 2052, 0, image, 2052, 640, 0));
#undef REJECT
    require(kega32x_ntsc565_row(filter, input, 320, 512, 0, black_edge, 640, 640, 0) == KEGA32X_NTSC565_OK &&
            kega32x_ntsc565_row(filter, input, 320, 512, 0xf800, red_edge, 640, 640, 0) == KEGA32X_NTSC565_OK &&
            memcmp(black_edge, red_edge, sizeof(black_edge)), "caller border color was ignored");
    require(!memcmp(black_edge+40, red_edge+40, 560*sizeof(uint16_t)), "border changed the middle of the image");
}

int main(void)
{
    Kega32xNtsc565 *filter;
    require(!kega32x_ntsc565_create((Kega32xNtsc565Preset)0), "invalid create preset accepted");
    kega32x_ntsc565_delete(NULL);
    filter = kega32x_ntsc565_create(KEGA32X_NTSC565_RGB);
    require(filter != NULL, "RGB filter allocation failed");
    test_colors_and_presets(filter);
    test_geometry_bounds_and_border(filter);
    kega32x_ntsc565_delete(filter);
    puts("RGB565 Blargg: four presets, full green precision, constants/chroma, independent instances, pitched bounds passed");
    return 0;
}
