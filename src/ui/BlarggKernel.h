#pragma once

#include <cstdint>
#include <vector>

// Exact md_ntsc 0.1.2 / sms_ntsc 0.2.3 packed contributions for all RGB565
// colors. Pure initialization code; no image processing runs on the CPU.
namespace BlarggKernel {
constexpr int colors = 65536;
// GG/SMS/H32 widths are 160/256 (up to 284 with borders); H40 is
// 320/334/348. The native 32X composite is normalized to H40 width.
inline bool usesH40(int nativeWidth) { return nativeWidth >= 300; }
inline int entrySize(bool h40) { return h40 ? 32 : 42; }
inline int outputWidth(int width, bool h40) {
    return width > 0 ? (h40 ? width * 2 : (width / 3 + 1) * 7) : 0;
}
// Presets match the public filter IDs: 1 monochrome, 2 composite, 3 S-Video,
// 4 RGB. Invalid IDs return an empty table.
std::vector<std::uint32_t> generate(int preset, bool h40);
// RGB32 0xffRRGGBB rows; output capacity must be outputWidth(width,h40).
// Table is the matching generate() result. This bounded fallback is also the
// integer reference for GPU verification. Each row repeats its own left/right
// samples outside the frame (the common RGB image has no palette backdrop).
// Input and output must not overlap.
void filterRow(const std::uint32_t* table, bool h40,
               const std::uint32_t* input, int width, std::uint32_t* output);
}
