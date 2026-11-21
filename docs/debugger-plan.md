# Debugger tools implementation plan

Agreed with the user on 2026-11-20. This is the implementation roadmap, not a
description of shipped features. The user authorized starting implementation on
2026-11-21; the current batch is Phase 1.

Planning baseline: release 0.28.0, repository a5edb31. The plan was committed as
28c61f9 before implementation. See [debug-inspection.md](debug-inspection.md)
for the delivered interface and explicit scope limitations.

## Progress and next action

- [x] Research reference debuggers and inspect the current architecture.
- [x] Agree on scope, architecture and delivery order; preserve this plan.
- [x] Phase 1: common inspection interface and memory, graphics and audio viewers.
- [ ] Phase 2: 68000/Z80 debugger with resumable execution.
- [ ] Phase 3: coordinated debugging of expansions, special hardware and link peers.
- [ ] Phase 4: integrated tracing, hardware timeline, code/data coverage and pixel provenance.

Phase 1 completed on 2026-11-21 with the scope and limits recorded in
[debug-inspection.md](debug-inspection.md) and
[verification](debugger-phase1-verification.md). Next task when requested: Phase 2.
Update this checklist and
append concrete verification evidence after each completed phase. Do not mark a
whole phase complete on the basis of one representative system. Preserve
remaining CPU/device limitations explicitly.

## Scope and interface

Extend the existing Debug menu rather than adding a separate emulator or debugger
core. Use a common backend and connected, modeless windows:

| Window | Purpose |
| --- | --- |
| Debugger | Disassembly, registers, flags, stack, breakpoints and execution controls |
| Memory | Memory domains, search, editing, watched values and bank mapping |
| Graphics | Extend existing graphics controls with tiles, maps, sprites, palettes, registers and framebuffers |
| Sound | Extend existing channel controls with chip state, PCM and external-track inspection |
| Trace | Selected CPU instructions and hardware events |
| Hardware | Active mapper, cartridge devices, ports, link and disc/player state |

Opening or focusing these windows must not itself pause the game. Explicit
execution commands and breakpoint hits may stop it. Keep windows compact,
resizable and consistent with the application's disabled minimize/maximize
controls. Use tabs within related tools; allow code and graphics side by side.

Choose tools from actual runtime hardware capabilities, not ROM filenames.
Reconfigure already open windows when hardware or games change; remove stale
targets and show an empty placeholder without a game. A CPU selector appears
only when needed. Linked consoles have explicit instance A/B selection.

All windows share address navigation, labels, comments, breakpoints and selected
device context. Example workflow: tile -> VRAM address -> write breakpoint ->
responsible CPU instruction.

## Common inspection architecture

Keep one versioned debugger interface between LibretroHost and the integrated
core. Enumerate instances, processors, memory domains, devices and supported
operations through stable IDs and explicit capabilities. Do not depend on
private symbol offsets or duplicate emulation logic in Qt.

EmulationSession owns execution and all debugger requests on its emulation
thread. Qt receives immutable, bounded snapshots of requested registers and
ranges, never direct pointers into running core memory. Decode/present captured
data away from the emulation thread where appropriate.

Use existing command-queue and latest-frame-mailbox patterns:
coalesce replaceable requests, cap queues, and capture only subscribed data.
Start live viewers at approximately 10 updates per second; refresh promptly on
a stop. Hidden/closed tools do not capture or trace. This is a proposed starting
rate, not a new per-frame requirement.

Associate requests and responses with a machine generation. Invalidate stale
work after load/unload, reset, state restoration/rewind, relevant media changes
and link topology changes. Destroy peer-specific data when a peer is released.

### Memory semantics

Separate logical CPU addresses, physical memory and bank/file offsets.
Represent mirrors and active mappings explicitly; banked code must not be
identified by logical PC alone.

Provide side-effect-free peek operations. Inspecting VDP, CDC or other MMIO
must not consume FIFO data, acknowledge interrupts or advance hardware.
Expose shadows/latched or unavailable values honestly where appropriate.
An intentional hardware write and editing ROM bytes are different operations.

Memory editing is paused-only and goes through the owning thread with cache,
renderer and DRC invalidation as required. Never overwrite the source ROM
automatically. Reuse existing cheat-search/watch facilities rather than adding
a competing scanner. Support byte patterns, numbers, text, endianness, changed
value highlighting and navigation into disassembly.

### Execution and scheduling

Current frame execution assumes a completed frame: frame counters, input
recording, rewind and audio publication depend on that boundary. Instruction
debugging therefore needs a resumable scheduler with explicit outcomes such as
instruction stop, frame completed and termination. Do not merely return early
from a CPU hook or block the worker inside a callback.

Step the selected CPU while maintaining scheduled execution of the rest of the
machine, VDP, DMA and IRQ events. Document breakpoint semantics, stop granularity
and any instruction-boundary overshoot. Read/write stops must specify whether
the access has already completed.

Phase 1 retains existing full-frame stepping. Later add pause/continue,
step into/over/out, run to address and supported scanline/interrupt boundaries.
Stack-based step-out and reconstructed call stacks are not universally reliable.

For SH-2, initially use explicit debug interpreter execution with verified state
handoff and DRC invalidation. Account for delay slots and direct DRC memory
accesses. Ordinary execution retains the fast backend when debugging is inactive.
Instruction tracing must not silently omit execution through idle-loop shortcuts.

Existing reverse-frame execution may be reused. Reverse-instruction execution
is deferred: it requires whole-machine checkpoints and deterministic replay.
Do not label reverse-frame as reverse-instruction.

## Platform and device coverage

| Hardware | Inspection/debugging scope |
| --- | --- |
| Mega Drive | Main 68000, Z80; planes A/B/Window, sprites, tiles, palettes, scroll, VDP FIFO/DMA, PSG/YM2612 |
| Master System / Mark III | Z80; tiles, tilemap, sprites, palette, mapper banks, PSG and actually enabled YM2413 |
| Game Gear | Z80 and GG video/palette/viewport; link registers and separate linked instances |
| SG-1000 | Z80 and its VDP modes, pattern/color/sprite tables, PSG |
| Sega CD | Main/sub 68000 and Z80; Program/Word RAM and ownership, CDC/CDD, graphics transformation, PCM/CDDA |
| 32X | MD CPUs plus master/slave SH-2, caches, SDRAM, palette, both framebuffers, line table, display modes and PWM |
| CD+32X | Combined CD and 32X devices on a common emulated timeline |
| MegaLD | Existing main/sub 68000 and Z80 topology; disc video, MD overlay, composition, playback/side/commands and audio synchronization |
| Firecore | Base MD CPU topology with its VDP extensions, tile and bitmap/YUV modes, collection banks and sound path |
| Special cartridges and supported peripherals | Active mapper banks/registers, RAM/EEPROM/Flash, external music and actual additional processors such as SVP SSP1601; expose Pico/peripheral-specific state when active |

Do not invent a CPU for an attachment that is currently modeled as a device.
MegaLD uses the existing CD scheduler; Firecore is not a new CPU architecture.
Do not copy disc images or decode complete large media for inspection.

Select graphics viewers by real format. 32X framebuffers are not MD tilemaps;
SG-1000 modes are not interchangeable with SMS modes. External MP3 playback
has track/command/decoder state rather than imaginary sound-chip registers.

## Debugger features

Provide execute/read/write breakpoints with ranges, conditions, hit counts and
one-shot behavior. Evaluate conditions only when relevant, without allocating
or parsing expressions on each access. Distinguish instruction fetch from data
read traps.

Add disassembly navigation, registers/flags, stack, bookmarks, comments and
shared symbolic addresses; import documented symbol formats in defined stages.
Verify guest disassembler availability/licensing before integration: a host DRC
disassembler is not a guest SH-2 disassembler.

Show actual CPU/device state and emulated timing. Keep emulated cycle profiling
separate from host performance measurements. Optional profiling must be bounded
and enabled explicitly.

## Graphics and sound details

Show sprite image, coordinates, dimensions, priority, flips, address and
evaluation order; link tiles to palette and map entries. Preserve current
layer/channel toggles, backdrop and channel gain behavior.

A VRAM snapshot represents the capture time. It cannot alone reconstruct
mid-scanline palette/bank writes or the exact displayed pixel. Label decoded
snapshots accordingly. Add optional selected-line snapshots where useful.

Precise pixel inspection is Phase 4: capture provenance from actual fetch/compose
operations only while requested. Respect priority, transparency, 32X and MegaLD
composition. Do not build a second approximate game renderer and call it exact.

Sound inspection reuses the current channel backend. Offer common YM2612 state
for MAME/Nuked with backend-specific details only where supported. Expose PCM
position and external music status without altering playback or retaining whole
tracks. Existing temporary channel/layer controls remain nonpersistent.

## Trace and coverage

Provide a bounded instruction ring and filtered hardware events: DMA, IRQ,
video-register writes, mapper switches, disc commands and link exchange.
Use instance/CPU identity and common emulated timestamps, with CPU-local timing
where needed. Connect event addresses to memory and disassembly.

Explicit file capture writes batched data asynchronously with a bounded queue.
If it cannot keep up, stop capture and clearly report incomplete capture;
do not silently grow RAM or stall normal play. Ring history retains only its
configured recent portion. Format text outside hot execution paths.

Mark resets, state loads, rewinds and context changes as timeline discontinuities.
Trace history is not part of native hardware save states.

Optional code/data coverage records observed execution and reads using physical
bank identity. It is evidence of visited paths, not a complete static analysis.
Enable it separately; do not continuously scan ROM.

## Integration and persistence

- Enforce debugger permissions in the emulation-thread backend, not only disabled buttons.
- Achievement Hardcore blocks internal-state inspection/editing tools; retain existing rules for ordinary presentation controls.
- Initially allow netplay-local read-only inspection. Disable invasive execution/memory operations until coordinated peer control exists.
- Input recording permits observation; disable arbitrary memory/register edits while recording/playback because these are not recorded input events.
- Do not publish partial frames to rewind, input recordings, audio or network synchronization.
- Preserve native-state and recording compatibility; debugger runtime metadata does not belong in hardware state.
- Keep layer masks, backdrop and diagnostic gains temporary as already agreed.
- Permit explicit saving of a debug project containing labels, comments, breakpoints and watches. Avoid automatic per-game writes and do not change normal save filenames.
- Link debugging ultimately requires consistent stop/resume of both isolated instances; a selected peer is not permission to let the other run unsynchronized.

## Delivery phases and acceptance

### Phase 1

Implement capability enumeration, bounded snapshots, safe memory domains and
automatic context changes. Extend graphics/audio viewers and add basic memory
inspection/watch navigation. Keep execution at existing complete-frame boundaries.

Acceptance: viewers do not mutate hardware state; changing/unloading games with
windows open is safe; subscriptions stop when closed; tools identify actual
hardware and report unsupported views honestly. Verify Windows/Linux behavior.

### Phase 2

Implement resumable execution and 68000/Z80 instruction controls, breakpoints,
conditions, disassembly and symbols for MD and eight-bit systems. Audit real
hook coverage, including Z80 and memory fast paths.

Acceptance: a stop/resume preserves execution against uninterrupted runs;
steps do not advance frame accounting twice or publish incomplete frames.
Check DMA, IRQ, audio continuation, saves, rewind and input recordings.

### Phase 3

Extend coordinated debugging to CD main/sub CPUs, both SH-2s and CD+32X,
then MegaLD, Firecore-specific state, additional processors and both link
instances. Implement only capabilities actually supported by each backend.

Acceptance: cross-CPU ordering, SH-2 delay slots/cache handoff, media continuation
and linked-instance lifetime are correct. Maintain an explicit per-device ledger.

### Phase 4

Integrate instruction/event tracing, the hardware timeline, code/data coverage
and precise pixel provenance. Add optional profiling and debug-project export
without routine disk activity.

Acceptance: bounded long capture, explicit overflow behavior, meaningful event
ordering and accurate pixel provenance in raster/composited scenes.

For each phase measure disabled-tool overhead against the preceding build in
normal and accelerated play. Hardware-wide changes require at least two games
from different developers/regions and appropriate expansion coverage. Record
actual scenes, refresh rate, backend, memory and measured results. Avoid repeated
unrelated test sweeps; never claim untested platforms are verified.

## Research findings and references

Current Qt DebugGraphicsDialog/DebugAudioDialog only expose output masks and
gains. EmulationSession owns LibretroHost on its worker; its bounded queue and
latest-frame publication are suitable patterns. Existing exported core APIs do
not supply instruction stepping or guest disassembly.

The prepared GPX source has conditional M68K/VDP CPU hooks. Z80 event names alone
do not prove Z80 hook coverage; the inspected source only declared those names.
Hooks are not a resumable scheduler. Inspect and extend source/exports rather
than relying on test-only private-symbol techniques.

Reference workflows, not blanket permission to copy implementations:

- [FCEUX debugger](https://fceux.com/web/help/Debugger.html): execution controls, shared symbols and navigation.
- [FCEUX memory editor](https://fceux.com/web/help/HexEditor.html): distinguish CPU bus writes from ROM editing.
- [FCEUX trace logger](https://fceux.com/web/help/TraceLogger.html): bounded tracing and optional display updates.
- [FCEUX code/data logger](https://fceux.com/web/help/CodeDataLogger.html): observed code/data coverage.
- [FCEUX PPU viewer](https://fceux.com/web/help/PPUViewer.html): capture timing matters for graphics inspection.
- [Gearsystem](https://github.com/drhelius/Gearsystem#features): Z80 navigation, symbols and viewers.
- [BlastEm](https://www.retrodev.com/blastem/features.html): conditional debugging and graphics inspection.
- [Exodus](https://www.exodusemulator.com/): pixel provenance and detailed VDP inspection.
- [Gens KMod](https://gendev.spritesmind.net/page-gensK.html): watches and hardware-oriented diagnostics.
- [MAME debugger](https://docs.mamedev.org/debugger/): devices, address spaces and side-effect-aware access.
- [ares SH-2 debugging](https://github.com/ares-emulator/ares/blob/8bc30c8c3d3c8081d2a66587add602df771441dd/ares/md/m32x/debugger.cpp): trace/JIT interaction.

Review individual source licenses before reusing code; our core-linking exception
does not automatically cover third-party code. Existing diagnostic behavior is
documented in [debug-controls.md](debug-controls.md).
