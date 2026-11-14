// Blargg initialization adapted from md_ntsc 0.1.2 and sms_ntsc 0.2.3.
// Copyright (C) 2006-2007 Shay Green. LGPL 2.1 or later.
// See third_party/blargg/COPYING.LGPL-2.1 and PROVENANCE.txt.
#include "BlarggKernel.h"
#include <cassert>
#include <cmath>

namespace Md {
typedef struct md_ntsc_setup_t {
    double hue, saturation, contrast, brightness, sharpness;
    double gamma, resolution, artifacts, fringing, bleed;
    const float* decoder_matrix;
} md_ntsc_setup_t;
using md_ntsc_rgb_t = std::uint32_t;
enum { md_ntsc_entry_size = 32 };
#define BLARGG_GPU_SMS 0
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
#include "../../third_party/blargg/md_ntsc_kernel_impl.h"
static const md_ntsc_setup_t presets[4] = {
    {0,-1,0,0,.2, 0,0,-.2,-.2,-1, nullptr},
    {0,0,0,0,0, 0,0,0,0,0, nullptr},
    {0,0,0,0,0, 0,.2,-1,-1,0, nullptr},
    {0,0,0,0,.2, 0,.7,-1,-1,-1, nullptr}
};
static void correct_errors(md_ntsc_rgb_t color, md_ntsc_rgb_t* out) {
    for (unsigned i = 0; i < rgb_kernel_size / 4; ++i) {
        const md_ntsc_rgb_t error = color - out[i] - out[i+2+16] - out[i+4]
            - out[i+6+16] - out[i+8] - out[(i+10)%16+16]
            - out[(i+12)%16] - out[(i+14)%16+16];
        CORRECT_ERROR(i + 6 + 16);
    }
}

std::vector<std::uint32_t> generate(int preset) {
    if (preset < 1 || preset > 4) return {};
    const auto& setup = presets[preset - 1];
    init_t impl;
    init(&impl, &setup);
    float rb[32];
    const float gamma = 1.1333f - float(setup.gamma) * 0.5f;
    for (int c = 0; c < 32; ++c)
        rb[c] = float(pow(c * (1.0f / 31.0f), gamma)) * impl.contrast + impl.brightness;
    std::vector<std::uint32_t> table(65536 * md_ntsc_entry_size);
    for (unsigned color = 0; color < 65536; ++color) {
        const float rr = rb[color >> 11];
        const float gg = impl.to_float[(color >> 5) & 63];
        const float bb = rb[color & 31];
        float y, i, q = RGB_TO_YIQ(rr, gg, bb, y, i);
        int r, g, b = YIQ_TO_RGB(y, i, q, impl.to_rgb, int, r, g);
        const md_ntsc_rgb_t rgb = PACK_RGB(r, g, b);
        auto* out = table.data() + color * md_ntsc_entry_size;
        gen_kernel(&impl, y, i, q, out);
        correct_errors(rgb, out);
    }
    return table;
}
#undef alignment_count
#undef burst_count
#undef rescale_in
#undef rescale_out
#undef artifacts_mid
#undef artifacts_max
#undef fringing_mid
#undef fringing_max
#undef std_decoder_hue
#undef gamma_size
#undef LUMA_CUTOFF
#undef md_ntsc_rgb_builder
#undef BLARGG_GPU_SMS
#undef DISABLE_CORRECTION
#undef PI
#undef rgb_bits
#undef STD_HUE_CONDITION
#undef ext_decoder_hue
#undef rgb_unit
#undef rgb_offset
#undef ROTATE_IQ
#undef RGB_TO_YIQ
#undef YIQ_TO_RGB
#undef PACK_RGB
#undef PIXEL_OFFSET_
#undef PIXEL_OFFSET
#undef CORRECT_ERROR
#undef DISTRIBUTE_ERROR
#undef RGB_PALETTE_OUT
} // namespace Md

namespace Sms {
typedef struct md_ntsc_setup_t {
    double hue, saturation, contrast, brightness, sharpness;
    double gamma, resolution, artifacts, fringing, bleed;
    const float* decoder_matrix;
} md_ntsc_setup_t;
using md_ntsc_rgb_t = std::uint32_t;
enum { md_ntsc_entry_size = 42 };
#define BLARGG_GPU_SMS 1
#define alignment_count 3
#define burst_count 1
#define rescale_in 8
#define rescale_out 7
#define artifacts_mid 0.4f
#define artifacts_max 1.2f
#define fringing_mid 0.8f
#define std_decoder_hue 0
#define gamma_size 64
#define md_ntsc_rgb_builder ((UINT32_C(1) << 21) | (UINT32_C(1) << 11) | (UINT32_C(1) << 1))
#include "../../third_party/blargg/md_ntsc_kernel_impl.h"
static const md_ntsc_setup_t presets[4] = {
    {0,-1,0,0,.2, 0,.2,-.2,-.2,-1, nullptr},
    {0,0,0,0,0, 0,.25,0,0,0, nullptr},
    {0,0,0,0,0, 0,.25,-1,-1,0, nullptr},
    {0,0,0,0,.2, 0,.7,-1,-1,-1, nullptr}
};
static void correct_errors(md_ntsc_rgb_t color, md_ntsc_rgb_t* out) {
    for (unsigned i = 0; i < rgb_kernel_size / 2; ++i) {
        const md_ntsc_rgb_t error = color - out[i] - out[(i+12)%14+14]
            - out[(i+10)%14+28] - out[i+7] - out[i+5+14] - out[i+3+28];
        CORRECT_ERROR(i + 3 + 28);
    }
}

std::vector<std::uint32_t> generate(int preset) {
    if (preset < 1 || preset > 4) return {};
    const auto& setup = presets[preset - 1];
    init_t impl;
    init(&impl, &setup);
    float rb[32];
    const float gamma = 1.1333f - float(setup.gamma) * 0.5f;
    for (int c = 0; c < 32; ++c)
        rb[c] = float(pow(c * (1.0f / 31.0f), gamma)) * impl.contrast + impl.brightness;
    std::vector<std::uint32_t> table(65536 * md_ntsc_entry_size);
    for (unsigned color = 0; color < 65536; ++color) {
        const float rr = rb[color >> 11];
        const float gg = impl.to_float[(color >> 5) & 63];
        const float bb = rb[color & 31];
        float y, i, q = RGB_TO_YIQ(rr, gg, bb, y, i);
        int r, g, b = YIQ_TO_RGB(y, i, q, impl.to_rgb, int, r, g);
        const md_ntsc_rgb_t rgb = PACK_RGB(r, g, b);
        auto* out = table.data() + color * md_ntsc_entry_size;
        gen_kernel(&impl, y, i, q, out);
        correct_errors(rgb, out);
    }
    return table;
}
#undef alignment_count
#undef burst_count
#undef rescale_in
#undef rescale_out
#undef artifacts_mid
#undef artifacts_max
#undef fringing_mid
#undef fringing_max
#undef std_decoder_hue
#undef gamma_size
#undef LUMA_CUTOFF
#undef md_ntsc_rgb_builder
#undef BLARGG_GPU_SMS
#undef DISABLE_CORRECTION
#undef PI
#undef rgb_bits
#undef STD_HUE_CONDITION
#undef ext_decoder_hue
#undef rgb_unit
#undef rgb_offset
#undef ROTATE_IQ
#undef RGB_TO_YIQ
#undef YIQ_TO_RGB
#undef PACK_RGB
#undef PIXEL_OFFSET_
#undef PIXEL_OFFSET
#undef CORRECT_ERROR
#undef DISTRIBUTE_ERROR
#undef RGB_PALETTE_OUT
} // namespace Sms

std::vector<std::uint32_t> BlarggKernel::generate(int preset, bool h40) {
    return h40 ? Md::generate(preset) : Sms::generate(preset);
}

namespace {
std::uint32_t color565(std::uint32_t color) {
    const unsigned r = ((color >> 16 & 255) * 31 + 127) / 255;
    const unsigned g = ((color >> 8 & 255) * 63 + 127) / 255;
    const unsigned b = ((color & 255) * 31 + 127) / 255;
    return r * 2048 + g * 32 + b;
}
std::uint32_t outputRgb(std::uint32_t raw) {
    constexpr std::uint32_t builder = (1u << 21) | (1u << 11) | (1u << 1);
    const std::uint32_t sub = raw >> 9 & (builder * 3u / 2u);
    std::uint32_t clamp = builder * 0x101u - sub;
    raw |= clamp;
    clamp -= sub;
    raw &= clamp;
    const unsigned r = raw >> 24 & 31;
    const unsigned g = raw >> 13 & 63;
    const unsigned b = raw >> 4 & 31;
    return 0xff000000u | ((r * 8 + r / 4) << 16)
        | ((g * 4 + g / 16) << 8) | (b * 8 + b / 4);
}
}

void BlarggKernel::filterRow(const std::uint32_t* table, bool h40,
                            const std::uint32_t* input, int width,
                            std::uint32_t* output) {
    if (!table || !input || !output || width <= 0) return;
    const int entry = entrySize(h40);
    const auto kernel = [&](int x) {
        // RGB presentation has no palette backdrop; extend this row's own
        // left/right colors so uniform frames stay uniform to their edges.
        x = x < 0 ? 0 : (x >= width ? width - 1 : x);
        return table + color565(input[x]) * entry;
    };
    const auto* left = kernel(0);
    const std::uint32_t* current[4];
    const std::uint32_t* previous[4] = {left, left, left, left};
    int cursor;
    const auto advance = [&](int index) {
        previous[index] = current[index];
        current[index] = kernel(cursor++);
    };
    if (h40) {
        current[0] = left;
        current[1] = left;
        current[2] = kernel(1);
        current[3] = kernel(2);
        cursor = 3;
        const auto emit = [&](int phase) {
            const auto raw = current[0][phase] + current[1][(phase+6)%8+16]
                + current[2][(phase+4)%8] + current[3][(phase+2)%8+16]
                + previous[0][phase+8] + previous[1][(phase+6)%8+24]
                + previous[2][(phase+4)%8+8] + previous[3][(phase+2)%8+24];
            *output++ = outputRgb(raw);
        };
        int remaining = outputWidth(width, true);
        while (remaining > 0) {
            advance(0); emit(0); emit(1); remaining -= 2;
            if (!remaining) break;
            advance(1); emit(2); emit(3); remaining -= 2;
            if (!remaining) break;
            advance(2); emit(4); emit(5); remaining -= 2;
            if (!remaining) break;
            advance(3); emit(6); emit(7); remaining -= 2;
        }
    } else {
        const int extra = width % 3;
        current[0] = left;
        current[1] = left;
        current[2] = extra ? kernel(extra - 1) : left;
        cursor = extra;
        const auto emit = [&](int phase) {
            const auto raw = current[0][phase] + current[1][(phase+12)%7+14]
                + current[2][(phase+10)%7+28] + previous[0][(phase+7)%14]
                + previous[1][(phase+5)%7+21] + previous[2][(phase+3)%7+35];
            *output++ = outputRgb(raw);
        };
        for (int chunk = 0; chunk < width / 3 + 1; ++chunk) {
            advance(0); emit(0); emit(1);
            advance(1); emit(2); emit(3);
            advance(2); emit(4); emit(5); emit(6);
        }
    }
}
