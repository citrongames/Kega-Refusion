# Instruction and hardware tracing

Open **Debug → Trace**. Select an available CPU, or all CPUs, and the event
categories to capture. Linked consoles have separate A/B targets. Opening the
window leaves the game running; capture starts only when requested.

The table shows the most recent 256 records, with their sequence, frame,
master-clock position, CPU/device, address and decoded instruction or event.
Double-click an instruction to open its logical address in the CPU debugger.
Navigation is disabled after changing to a different game or console topology.
The tracer is not a physical-bank-aware code/data logger.

## Memory and files

Recent history uses a bounded ring: 16 MiB by default, adjustable from 1 to
64 MiB. Older records are replaced when it fills; the overwritten count is
visible. **Clear** empties recent history. It does not erase records already
written to a capture file or restart their sequence numbers.

Enable file capture explicitly to write a TSV log. Its fixed queue uses less
than 3 MiB in addition to the ring. Formatting and file writes run on a separate
writer thread. Stop ends capture immediately while the queued data finishes
writing. If the writer cannot keep up, capture stops and the written prefix is
marked incomplete. There is no unbounded queue or blocking disk write in an
instruction callback. Existing files are never silently overwritten.

Exporting recent history takes one bounded copy of the ring, up to its selected
size, and writes it asynchronously. Only one file job runs at a time. Closing
or hiding the window stops capture and GUI polling; the recent ring remains
available until cleared, replaced by a new capture, or the emulator exits.

The TSV includes captured opcode bytes and raw hardware payloads. Mnemonics
are decoded for the viewer on the emulation worker, outside the instruction
callback. The actual first opcode comes from execution; following bytes are a
side-effect-free code-map snapshot and are explicitly flagged as lookahead.
They are not a promise that every extension byte was fetched from the bus.
See the [native record contract](debugger-trace-native.md) for field meanings.

## Timing and boundaries

Sequence records callback order. CPU scheduling can deliver an earlier
frame-local master-clock timestamp after a later one from another CPU;
the viewer does not sort away that ordering. CPU-local ticks remain available
in the file. SVP timestamps are marked as scheduler-quantized.

Reset, power restart, state load, rewind and accepted debugger edits add
discontinuity records and increment the epoch. Internally reconstructed rewind
pictures are excluded from capture. Changing games or cable topology stops the
old capture. No trace data is added to hardware save states or input recordings.

CPU tracing includes main/sub 68000, Z80, both SH-2s and SSP1601 when those
processors are actually present. The event filters follow available hardware.
Implemented events include accepted VDP-register writes, VDP DMA, accepted
interrupts, committed Sega CD/MegaLD commands, real cable edges/received bytes
and Firecore bank changes. Other mapper events and 32X device-register writes
are not implemented yet.

SH-2 instruction/interrupt observation selects the interpreter while needed;
ordinary execution returns to its usual backend after capture stops.
Capture therefore has a cost, especially when all CPUs are selected. With no
capture, there is no ring allocation, file writer or instruction formatting.

Tracing is blocked in achievement Hardcore, netplay and video export.
Observation is allowed during input recording/playback; existing restrictions
on arbitrary register/memory edits remain. Code/data coverage, exact pixel
provenance, profiling and saved debugger projects remain later Phase 4 work.

See [verification](debugger-phase4-verification.md) for actual tests and
performance measurements; it does not imply complete game-by-game coverage.
