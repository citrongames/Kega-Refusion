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
allocation-free per-line execution and input validation. The only H32 policy
implemented expands MD's 256 pixels and transparency samples to a 320-pixel line
using nearest-neighbor sampling. It does not claim exact independent-clock phase
or fractional 3.25-pixel alignment. The PicoDrive line-offset 0xff shift exception
is retained for packed, direct and RLE modes.

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
