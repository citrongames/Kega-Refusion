# Code and data coverage

Open **Debug → Code/data coverage**. Select console A/B where available,
a CPU (or all supported CPUs on that console), and execution/data-read
observation. **Start** creates a new map; opening the window alone does not
start capture or pause the game.

The view describes physical storage, not CPU address ranges. Two mapper banks
at the same CPU address occupy different positions in the cartridge map.
Mirrored accesses share the same physical bytes. Instruction operands are code
when actually consumed by the CPU; speculative prefetch, debugger peeks and
trace lookahead are not counted as executed code. Data means CPU data reads,
not DMA or graphics/sound devices reading their own memory.

## Reading the map

The page contains up to 256 physical-byte marks:

- **U**: not observed.
- **C**: consumed as instruction-stream code.
- **R**: read as CPU data.
- **B**: both.

Code and data totals each include the bytes in Both. These are unique physical
bytes, not execution frequency. A byte not observed is not proof that it is
unused or unreachable. A source marked partial has access paths outside the
implemented physical resolver. Unknown-origin totals count observed span bytes,
including repeated accesses, and are not a percentage of the cartridge.

An exact **Open in Memory** link is offered only when the current memory viewer
can represent the same physical source and offset. For example, canonical CD
Word RAM coverage has no direct link while the inspector uses the interleaved
1M bank layout. Old captures remain inspectable after unload, but their links
are disabled after the game or selected console ceases to match.

Coverage remains the union of observed paths across resets, save loads and
rewinds; it does not undo observations. Those boundaries are counted. Internal
frames generated only to reconstruct rewind pictures are excluded. **Clear**
resets marks and totals without resetting or pausing the guest. Changing games
or cable topology stops capture. Hide/close stops capture and GUI polling;
the bounded map remains available for export.

## Resources and export

Four physical-byte marks fit in one packed byte. A 4 MiB ROM therefore needs
1 MiB of marks, plus marks for the observed RAM/BIOS sources and small metadata.
The packed allocation has a hard 64 MiB limit per capture. Allocation and
clearing occur only on explicit commands; no ROM scan is needed. A native
observation only updates the relevant packed bits and counters.

Export writes a new **.kcdl** file on a separate thread. It contains source
descriptors, identity, counters and packed flags, not ROM or RAM contents.
One bounded copy of the map is taken for the explicit export, and only one
writer runs at a time. Gameplay and capture can continue while it writes.
Existing files are not overwritten. There is no automatic per-game file.

The portable file begins with the eight bytes `KRCDL1\r\n`, followed by a
little-endian 32-bit JSON-header length, that UTF-8 JSON header and packed source
data in ascending source-ID order. `packedOffset` is relative to the payload;
`packedBytes` is ceil(source size / 4). Within a packed byte, physical byte
positions use two bits from least to most significant. The header records console
A/B as instance 0/1 and the selected observation masks for each source. Bit 0 means code, bit 1
means data. This is a Kega Refusion format, not an FCEUX or other emulator CDL
format. Import/merging is not implemented.

## Boundaries and permissions

Native source descriptors advertise actual CPU/read/execute support. Unknown
handler-backed or transformed accesses are not assigned to a guessed ROM
offset. Source IDs describe physical allocations; observing changing RAM does
not identify which file originally supplied its contents.

Capture is blocked in achievement Hardcore, netplay and video export.
Input recording/playback permits observation. The existing restrictions on
memory/register edits remain. Debugger, trace and coverage attachment are
independent. Selected SH-2 observation uses the interpreter while enabled;
ordinary execution uses its usual backend after tools detach.

Exact pixel provenance, profiling, shared physical symbols and debugger-project
persistence remain separate roadmap work. The [native implementation](debugger-coverage-native.md) and
[verification ledger](debugger-coverage-verification.md) record which access paths were actually checked.
