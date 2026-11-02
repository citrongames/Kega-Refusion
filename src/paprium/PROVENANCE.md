# Paprium cartridge HLE provenance

## Source and license

The Paprium cartridge implementation is adapted from
[pav1388/Genesis-Plus-GX-PUNCHiUM](https://github.com/pav1388/Genesis-Plus-GX-PUNCHiUM)
at commit `3849f3d3432df1d6320574e73695dd379ecef2b3` (2025-08-29).
The principal source is `core/cart_hw/punchium.h`. The donor's
`core/cart_hw/md_cart.c`, `core/system.c`, and `core/vdp_ctrl.c`
show the cartridge, audio, and DMA integration points.

The donor source header identifies "Project Little Man" and says "edit by
pav13". Preserve that attribution in adapted source. The donor's
`LICENSE.txt` is copied without changes to [LICENSE](LICENSE). It says
that Genesis Plus GX code, unless explicitly stated otherwise, has
non-commercial redistribution terms, a complete-source requirement for
modified binary distributions, and notice requirements. The Paprium
source header does not state a separate license. This provenance record
does not grant any additional rights.

The existing host-side `Mp3Stream` uses minimp3, documented separately
in the repository's `THIRD_PARTY.md`. Paprium soundtrack files are
external user-supplied files; neither ROM nor music is included here.

## Behavioral references

These sources were consulted to compare cartridge behavior; their code
is not incorporated by this provenance record:

- [TheHpman MAME Paprium pull request #13939](https://github.com/mamedev/mame/pull/13939),
  head `6b8fd9a` (2025-09-02): independently modeled mailbox commands,
  decompression, save transfer, sprite handling, and memory windows.
  The submitted implementation describes itself as incomplete.
- [thekoalakoa/paprium-pocket](https://github.com/thekoalakoa/paprium-pocket),
  commit `f12f62a225307734d8eb1cda92c5a1d73105d080` (2026-10-27):
  hardware-tested observations about cartridge RAM byte writes,
  sprite-table termination, cache behavior, and music cues. That FPGA
  project is GPL-3.0-or-later; its HDL and firmware are not part of this
  software integration.

## Scope

This is experimental high-level emulation of the Paprium cartridge's
observable behavior. The cartridge MCU firmware is unavailable, so
boot-time patches, graphics operations, DMA timing, and audio behavior
remain approximations. This is not a cycle-accurate model of the
DATENMEISTER hardware. Music playback uses the user's adjacent MP3
soundtrack as a substitute for the undumped cartridge synthesizer.

CPU MATE uses GPX's existing Mega CD Mode 1 hardware when a regional user
BIOS is present. The ROM uploads its own sub-68000 program and tables into
emulated CD RAM; no replacement AI or firmware is bundled. The existing
scheduler and shared communication registers execute that program. Without
a BIOS the standalone cartridge path is retained. Combined CD/cartridge
states reserve space for the cartridge HLE in addition to GPX's CD state;
ordinary games retain their existing state size and identity.

Paprium's HLE reset explicitly resets PSG/FM and clears audio resamplers,
preventing a minigame PSG tone from surviving into the main game. Music is
streamed at the source MP3 rate (44.1 or 48 kHz stereo), with bounded PCM
and tile buffers.
