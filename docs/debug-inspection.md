# Read-only hardware inspection

Phase 1 of [the debugger roadmap](debugger-plan.md). The Debug menu now includes
Memory, while the existing Graphics and Sound Channels windows have inspector
tabs. These are modeless tools: opening/focusing them does not pause emulation.
Layer masks, backdrop and channel gain keep their existing temporary behavior.

## Available tools

- Memory: domain-relative hexadecimal offset, 256-byte pages, ASCII, changed
  byte highlighting, signed/unsigned 8/16/32-bit selected values with automatic
  or explicit byte order, and hex-pattern search within the current page.
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
Both English and Russian captions are supplied; chip names and register notation
remain conventional technical labels.

## What a snapshot means

Images are decoded from the captured table/framebuffer storage. They do not
reconstruct earlier raster writes or the internal sprite evaluation cache.
Sprite order is table/link order, not a promise of scanline evaluation order.
Pixel provenance and a hardware event timeline remain later roadmap work.

Firecore six-bit tiles and RGB888 palette are supported. Bitmap/YUV and
interlaced six-bit tile previews explicitly report unsupported modes;
raw memory, palette and registers remain inspectable. MegaLD player/controller
values are available, while separate disc-video/compositor inspection remains
later work. Special cartridge memories/values are exposed only when supported,
not inferred from game names.

This phase is read-only. CPU disassembly, instruction stepping/breakpoints,
editing, full memory watch lists, full-domain search and trace capture are not
advertised as available. Existing cheat search and frame stepping remain separate.

## Threading and resource limits

The versioned C ABI is in src/core/DebugInspectionAbi.h. Core-only adapters live
in src/coreinspect and the reproducible genesis-plus-gx-inspection.patch.
All exported reads use physical storage/latches without invoking guest bus
handlers. CPU logical-bus peeks and editing are future interfaces.

LibretroHost copies requested data on the emulation owner thread. EmulationSession
holds at most one pending request and one latest result; a serial discards
superseded results. State changes advance a generation. Qt receives copied
snapshots and cannot reference or mutate running core data.

A single GUI timer requests visible inspector tabs at 10 Hz; controls-only tabs
do not subscribe. Closed/inactive viewers release captured buffers. The worker's
inactive path is one atomic pending-request check, with no capture or polling
timer. Reads are bounded to 256 KiB each, memory windows to 4096 bytes, with fixed
domain/value catalog capacities. Only active graphic tabs decode images.

No new periodic disk writes, hardware-state fields, recording compatibility
changes or guest timing hooks were introduced.

## Restrictions

The worker rejects inspection in achievement Hardcore and during offline video
export. Existing presentation controls retain their previous restrictions.
Ordinary netplay permits local read-only snapshots. Linked-console inspection is
explicitly unavailable until per-instance coordinated debugging is implemented.
No inspection command is replicated as gameplay input.

## Verification

See debugger-phase1-verification.json for actual fixture coverage and
performance measurements. Source/compilation verification is distinct from
Windows interactive testing, which is performed by the user. Unexecuted media
paths are not counted as tested games.
