# Trace and memory inspector verification

Verified on 2026-11-21 against baseline **41860fa**. This batch delivers the first Phase 4
increment with instruction and hardware-event tracing, a bounded timeline, TSV
capture and export, and a faster memory table. Code/data coverage (CDL), pixel
provenance, profiling and debug-project persistence remain open. [Measurements and build hashes](debugger-phase4-verification.json).

## Focused checks

| Check | Evidence |
| --- | --- |
| Trace buffer | Linux test passed ring wrap and overwrite accounting, the 256-row preview limit, active-ring export, asynchronous stop with final-batch flush, file-open failure without overwrite, and queue overflow. The overflow output retained every queued event in order and ended with an incomplete marker. |
| Native MD and SMS | Linux original-ROM fixture passed on the corrected core. It covers the real fetched opcode after a debugger PC edit, bounded instruction bytes, VDP register writes, VRAM fill DMA start/completion, IRQ acceptance before the first handler opcode, and a single Z80 HALT record. Trace and execution hooks can detach independently. Captured-byte decoding works after unload. |
| Host integration | Linux original-ROM fixture passed. The traced MD frame matched the reference native state, picture and PCM exactly. It also checked sequence preservation through Clear, reset/restore/power/unload markers, rebinding, and selection of the B-side Z80 in linked Game Gear. |
| Native SH-2 | The standalone SH-2 test passed IRQ vector 68 and the first handler opcode across two scheduler budgets. |
| Expanded CPUs and events | Original fixtures passed SSP1601 execution/quantized timing, real MD/GG cable falling edges, both SH-2 branch/delay-slot opcodes, and sub-68000 execution plus a committed CDD Get Drive Status command. Full-frame continuation checks remained exact. |
| Session | Worker trace during recorded input passed; immediate close after Export preserved its file job, and the movie retained exactly one completed frame. Existing edit restrictions passed. Trace guards for Hardcore/netplay/video export were code-reviewed, not tested against live services. |
| Qt UI and packaging | Memory, CPU, lobby and trace widget checks passed offscreen on Linux. Trace layout was inspected in English and Russian. Applications and native cores built for Linux and Windows; all 73 staged files matched the deployed testbuild by SHA-256. Windows execution was left to the user. |

The producer writes to a 1–64 MiB history ring and, during explicit file
capture, a fixed queue of up to 32,768 records. TSV formatting and file I/O
run on the writer thread. The file keeps callback order and both common and
CPU-local timing. The unit test uses generated records; the native and host
fixtures use original guest programs.

## Memory inspector responsiveness

The offscreen Qt benchmark replayed identical 259-byte snapshots with
event processing after each update. It used WSL, Qt 6.10.2, Fusion style and
g++ with optimization level 2. Opening values are medians of five samples.
Update values are means of 12 samples per case.

| Isolated widget operation | Baseline | Updated |
| --- | ---: | ---: |
| Open plus initial page | 81.582 ms | 15.170 ms |
| Unchanged page update | 208.168 ms | 5.031 ms |
| Sparse page changes | 208.985 ms | 5.681 ms |
| Dense page changes | 658.236 ms | 10.768 ms |

Across 12 unchanged updates the table model's data-change notifications fell
from 3,264 to zero. Sparse and dense updates emitted 12 notifications each.
The same benchmark measured median open time without a page at 12.479 ms
before and 9.556 ms after. These times describe the widget and its event
processing with mock snapshots.

Baseline host reads of the same 259 bytes had 95th-percentile latencies of
1.492 µs in Mega Bomberman and 1.624 µs in Doom 32X. The corresponding hardware
snapshots took 1.052 and 0.822 µs, and full graphics snapshots took 31.700 and
169.343 µs. In two 3,000-frame Mega Bomberman runs, 600 reads per run took
5.672–7.002 ms in total. Polling every five frames represents about 10 Hz at
the cartridge's nominal PAL refresh. Final image, PCM and native state
hashes matched the closed-inspector runs.

## Closed-trace comparison

The same Linux host ran both cores pinned to CPU 5 with MAME YM2612,
180 warm-up frames and fixed input. Normal mode rendered every frame and
collected PCM. Fast mode rendered every 32nd frame and the final frame,
with PCM collection disabled during the measurement. Throughput is unpaced;
the listed refresh rates are the emulated rates. The final picture, PCM
from the next full-output frame and native state matched exactly in every
pair. The initial run overlapping a background build was discarded.

| Core batch and game | Refresh | Frames per mode | Pairs | Normal | Fast |
| --- | ---: | ---: | ---: | ---: | ---: |
| Before VDP trace-index fix, Sonic 2 (World) | 59.922743 Hz | 6,000 | 2 | −2.00% | +0.73% |
| Before VDP trace-index fix, Mega Bomberman (Europe) | 49.701459 Hz | 6,000 | 2 | +1.05% | −0.50% |
| Before VDP trace-index fix, Doom 32X (Japan, USA) | 59.922743 Hz | 600 | 1 | +2.30% | +1.72% |
| Before VDP trace-index fix, Darxide (Europe) | 49.701459 Hz | 600 | 1 | −2.57% | +1.38% |
| Corrected core, Sonic 2 (World) | 59.922743 Hz | 6,000 | 1 | +2.85% | −1.36% |
| Corrected core, Mega Bomberman (Europe) | 49.701459 Hz | 6,000 | 1 | +0.10% | −0.21% |

Multi-pair values are means of paired percentage changes. Sonic 2 normal
varied from +0.96% to −4.95% before the trace-index fix and measured +2.85%
afterward. The samples show no consistent slowdown. The final MD runs also
matched the pre-fix candidate's output hashes. The short 32X comparisons
were measured before the active VDP-register trace correction.

The end checkpoints were the Sega logo for Sonic 2, the title menu for Mega
Bomberman, the start menu for Doom and the language menu for Darxide. These
measurements cover those title/menu workloads and their preceding fixed
input sequences. Peak RSS and individual pair rates are in the JSON ledger.
Raw logs and reproduction harnesses are under ignored
`build/verification/debugger-phase4`.

## Interface update audit

The memory view now emits no change notification for an unchanged page and one
bounded notification for a changed page. Fixed font-derived column widths avoid
repeated content-size layout. Byte editing, highlighting, selection, scrolling
and domain changes remain covered by the focused widget test.

Graphics previews compare only their actual source buffers and layout fields.
Unchanged register/state tables no longer force a full repaint; inactive CPU
panes defer presentation. Hidden lobby snapshots remain cached and are applied
on showing the window. An unchanged authoritative snapshot can still correct a
rejected user action. FPS updates repaint only when displayed text changes and
OSD is enabled. Trace decoding and its model also skip unchanged records.

Controls, Cheats, Achievements, Updates and other settings refresh sites were
reviewed; their existing event-driven/diffing behavior was retained. Gameplay
frame delivery, input and audio cadence were not throttled. This is a focused
refresh audit, not an exhaustive profile of every OS/desktop interaction.
