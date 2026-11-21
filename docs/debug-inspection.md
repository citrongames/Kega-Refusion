# Hardware inspection

Open **Memory**, **Graphics** or **Sound Channels** from the Debug menu.
The modeless windows leave emulation running when opened or focused.
Layer masks, backdrop and channel gain remain temporary presentation controls.

## Available tools

- Memory: domain-relative hexadecimal offset, 256-byte pages, ASCII, changed
  byte highlighting, signed/unsigned 8/16/32-bit selected values with automatic
  or explicit byte order, and hex-pattern search within the current page.
  Writable domains also support edits during an explicit full-frame pause.
- Memory / Hardware: actual console, CPU/device presence, relevant mapper bank
  registers, ports, CD and MegaLD controller state.
- Graphics: decoded tiles, maps, sprites, palette, VDP registers and 32X
  indexed/direct-color/RLE framebuffer snapshots.
- Sound Channels / State: PSG, common MAME/Nuked YM2612 channel/timer state,
  physically available YM2413, CD PCM/CDDA, 32X PWM and supported external music
  state. Inspection never activates an audio decoder or advances playback.

Graphics links open the corresponding physical memory domain/offset. Switching
games or display modes updates available domains and viewers. Tools retain user
selection where valid and clear captured data on close, unload or blocked access.
The Memory window shows a **Console A/B** selector when Link Cable has two
consoles. This selection also applies to Graphics and Sound snapshots.
Both English and Russian captions are supplied alongside conventional chip
names and register notation.

## What a snapshot means

Images show captured table and framebuffer storage at the snapshot time.
Sprite order follows the captured table and links. Raster history, scanline
sprite evaluation, pixel provenance and a hardware event timeline remain
later roadmap work.

Firecore six-bit tiles and RGB888 palette are supported. Bitmap/YUV and
interlaced six-bit tile previews explicitly report unsupported modes;
raw memory, palette and registers remain inspectable. MegaLD player/controller
values are available, while separate disc-video/compositor inspection remains
later work. Special cartridge memories/values are exposed only when supported,
not inferred from game names.

## Editing paused memory

Double-click a hex byte to change it, or choose a width and byte order and
click **Edit…** for a multi-byte value. Each command writes at most four
bytes to the selected physical domain and console. The worker checks the
machine generation and original bytes before writing. A successful edit
updates the affected video caches and clears rewind history.

Main and Z80 RAM, Sega CD Program, Word, PCM and Backup RAM, and ordinary
VRAM, CRAM and VSRAM support writes where present. CD Backup RAM follows
normal save persistence. ROM, VDP registers, cartridge save storage,
expansion-specific buffers, native wide-mode VRAM and MD Mode 4 CRAM
remain read-only. The available actions follow actual hardware and mode.

The [CPU debugger](debug-execution.md) provides logical CPU addresses,
registers, disassembly and execution controls. Full-domain search, persistent
watch lists and trace capture remain [later work](debugger-plan.md).

## Threading and resource limits

The versioned C ABI is in src/core/DebugInspectionAbi.h. Core-only adapters live
in src/coreinspect and the reproducible genesis-plus-gx-inspection.patch.
Exported inspection reads use physical storage and latches. Memory
writes use bounded core adapters for the selected domain and avoid guest bus
handlers. The CPU debugger has a separate API for logical-address peeks.

LibretroHost copies requested data on the emulation owner thread. EmulationSession
holds one pending snapshot request, one latest result and one pending memory
edit. A serial discards superseded results. Context changes and accepted
memory edits advance a generation. Qt receives copied snapshots and submits
edit requests to the worker.

A single GUI timer requests visible inspector tabs at 10 Hz. Controls-only
tabs release their subscriptions. Closed or inactive viewers release captured
buffers. The worker checks for queued requests and edits and only captures
when requested. Reads are bounded to 256 KiB each and memory windows to
4096 bytes, with fixed domain and value catalog capacities. Graphic tabs
decode images while active.

## Restrictions

The worker blocks inspection in achievement Hardcore and during offline
video export. Online sessions permit read-only local snapshots. Memory
editing requires an explicit pause between complete frames and is blocked
while recording or playing back input, during online play, in Hardcore and
during export. Inspection requests stay local to the emulation session.

## Verification

See the [Phase 1 verification](debugger-phase1-verification.md) for snapshot
coverage and performance measurements. The [roadmap](debugger-plan.md)
tracks verification of the Phase 3 memory and Link Cable extensions.
Windows interactive testing is performed by the user.
