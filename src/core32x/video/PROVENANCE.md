# 32X scanline compositor provenance

The display-mode decoding and 32X priority rules are adapted from PicoDrive's
`pico/32x/draw.c`, revision `07abb00550528f7bf53087d932dd3d747cf2d0df`:
https://github.com/irixxxx/picodrive/blob/07abb00550528f7bf53087d932dd3d747cf2d0df/pico/32x/draw.c

Original notices: (C) notaz, 2009,2010; (C) irixxxx, 2019-2026.
The donor file names its license as the MAME license. Its complete accompanying
noncommercial license is retained in `LICENSE`. No BlastEm code was imported.

Kega Refusion adaptations (2026): independent C99 API with caller-owned buffers;
explicit MD transparency input instead of comparing against the backdrop index;
host-endian numeric framebuffer words; bounded bank-wrapping reads; separate
priority and RGB565 color conversion; full eight-bit GPX palette indices;
allocation-free per-line execution and input validation. Decode produces a
caller-owned immutable BGR555+priority row; later MD remaps mix against that row
without reading live framebuffer, 32X palette or control registers. The PicoDrive
line-offset 0xff shift exception is retained for packed, direct and RLE modes.

H32 composition point-samples a 320-pixel EDCLK output grid whose origin is the
MD active image, without an intermediate 1280-pixel row or interpolation. In
quarter-H40-pixel units each output center is `sample = 4*x + 2`; the MD color and
/YS index is `sample / 5`. The 32X origin is 13 units later, so its index is
`(sample - 13) / 4` when `sample >= 13`; before that MD wins. Consequently the
first three output pixels contain only MD, and the final three 32X pixels are
outside the cropped MD-origin viewport. A blank 32X layer uses the same MD center
samples. H40 remains one-to-one and SFT remains an independent decode-stage skip.

The H32 coordinate derivation uses the behavioral reference in ares:
[`VDP::mainH32()`](https://github.com/ares-emulator/ares/blob/master/ares/md/vdp/main.cpp)
places the 32X layer 13 units after MD, while
[`VDP::DAC::output()`](https://github.com/ares-emulator/ares/blob/master/ares/md/vdp/dac.cpp)
emits five units per H32 MD pixel and four per H40 pixel. No ares implementation
was copied. This deterministic point sampling models that relative 3.25-pixel
phase; it does not claim analogue reconstruction or exact /YS clock-domain race
behavior. PicoDrive's donor also documents the offset and uncertainty at /YS
boundaries.

The displayed bank, line-table contents, palette, video mode, global priority and
shift must be supplied at the correct scanline time by the device implementation.
The compositor has no emulated CPU, register side effects, bus, Qt or Pico globals.
It does not implement an emulator backend or replace the GPX MD/CD engines.

Integration must pass GPX's true pre-remap transparency decision independently of
its final RGB565 palette lookup, including shadow/highlight. Equal RGB colors or
a palette index equal to the backdrop must not make an opaque MD pixel disappear.
GPX `remap_line()` also owns Blargg output; composition/filter ordering and filtered
32X colors require an explicit integration decision by the lead implementation.
This module neither disables nor changes the filter.
