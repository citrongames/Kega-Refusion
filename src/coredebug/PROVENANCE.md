# Guest CPU disassemblers

- Musashi MC68000 decoder, Karl Stenerud, MIT-style license in
  [musashi_disasm.c](musashi_disasm.c). Upstream
  [m68kdasm.c](https://github.com/kstenerud/Musashi/blob/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd/m68kdasm.c),
  SHA-256 c31b1d3a9863c4271fd0ddcddf3cde8fadac691909ca994db6ec41941696dca2.
  The adaptation restricts the interface to MC68000 and buffers supplied by
  Kega Refusion, bounds every byte read and marks call and return instructions. MC68000 applies
  the short branch encoding to a displacement of -1.
- MAME portable Z80 opcode tables, Juergen Buchmueller, BSD-3-Clause, with
  [license text](Z80-LICENSE.txt). Upstream
  [z80dasm.cpp](https://github.com/mamedev/mame/blob/a8e7c6b6bcbd4106dfd69f8dec08a1a1e0b6c56c/src/devices/cpu/z80/z80dasm.cpp),
  SHA-256 091f5d1a0d9f0a2602b09a39cb36c1239224349304be14abc7d9ac217349a4c8,
  and [z80dasm.h](https://github.com/mamedev/mame/blob/a8e7c6b6bcbd4106dfd69f8dec08a1a1e0b6c56c/src/devices/cpu/z80/z80dasm.h),
  SHA-256 7a5c2a035493007fcae715924d41ee5b8d5ebc44c0a6d67666fc80150897971e.
  The adaptation uses the Z80 mnemonics and five opcode tables in a C
  decoder with bounded input and output. It consumes repeated and ignored
  index prefixes within one decoded instruction, as the emulated CPU does.

- SH-2 decoder adapted from the pinned PicoDrive 32X donor,
  [sh2dasm.c](https://github.com/irixxxx/picodrive/blob/07abb00550528f7bf53087d932dd3d747cf2d0df/cpu/sh2/mame/sh2dasm.c).
  Donor non-commercial terms remain in `third_party/picodrive32x/COPYING`.
  Adaptations bound output, remove the command-line frontend, avoid signed
  shift overflow, and expose guest call/return/delay-slot metadata.
