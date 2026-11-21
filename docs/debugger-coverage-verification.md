# Physical coverage verification

Verified on 2026-11-21 against `bd91e5e`. This Phase 4 increment adds
physical byte coverage for executed instruction streams and CPU data reads.
Pixel provenance, profiling and debug-project persistence remain open.
[Native coverage and limitations](debugger-coverage-native.md) ·
[Machine-readable measurements](debugger-coverage-verification.json).

## Focused checks

| Check | Passed evidence |
| --- | --- |
| Packed history and export | Physical source IDs stay distinct. Code, read and overlap count unique bytes. An invalid span adds its full width to the cumulative unknown byte counter. Tests cover bounds, unchanged revisions on repeated marks, bounded pages, two-bit packing, clear during async export and preservation of existing output files. |
| Native SMS and Mega Drive | An original Sega-mapper program executes and reads from two physical banks through one logical SMS window and reads an alias of the first bank. An original 68000 SSF program switches its own execution bank and distinguishes the old prefetched opcode from the differently mapped next word. Native checks cover consumed operands, mirrors, main RAM and unknown VDP status. |
| Native expansions | Original fixtures cover both SH-2s' SDRAM instructions and PC-relative reads, SSP1601 opcode and immediate words, and sub-68000 PRG RAM execution with canonical Sega CD Word RAM reads through 2M mirrors, 1M bank interleave and the dot view. They compare completed state, picture, PCM and clock across the existing debugger resume path. The established SH-2 comparison omits only backend scratch PPC and EA words. |
| Host and worker | The original Mega Drive guest matches uninterrupted state, picture, PCM and clock. It checks canonical ROM/RAM flags, stable repeated observations, reset/restore union, rendered rewind-preview exclusion, independent trace and breakpoint hooks, stale Clear rejection and unload. The linked Game Gear fixture sends Right only to B and checks that B's distinct code and data appear without A's. The worker finishes an explicit export even when Stop follows immediately. |
| Coverage window | Linux Qt offscreen checks cover the captured instance versus the next selection, per-CPU capability controls, bounded pages, stable unchanged updates, one change notification for a modified page, navigation, stale pages and closing the window. English and Russian previews were saved. |
| Builds | Linux and Windows native cores and applications built. Windows interactive runtime was left to the user. |

Active coverage assertions use generated guest programs and the local Sega CD
BIOS. The four commercial cartridges below exercise the disabled paths. The
physical map reflects observed reads and consumed instruction bytes. Details
of unsupported handlers, sources and processors remain in the native guide.

The optimized Linux core passed the native bank/prefetch, expansion, host
and worker checks again. The native trace fixture also passed its opcode,
IRQ, VDP, DMA, detach and HALT checks with coverage present.

## Closed-tool performance

The same Linux host ran both cores pinned to CPU 5 with MAME YM2612, fixed
input and 180 warm-up frames. Coverage, trace and the CPU debugger were
inactive. Normal rendered each frame and collected PCM. Fast rendered every
32nd frame and the final frame with PCM collection disabled during the timed
run. Each title used one baseline/candidate pair in each mode. Core order
alternated by title. Using one host executable for both cores isolates
the native-core change in this comparison.

| Core batch and game | Refresh | Frames per mode | Normal | Fast |
| --- | ---: | ---: | ---: | ---: |
| Initial coverage core, Sonic 2 (World) | 59.922743 Hz | 6,000 | −2.54% | −7.73% |
| Initial coverage core, Mega Bomberman (Europe) | 49.701459 Hz | 6,000 | −1.69% | −7.61% |
| Initial coverage core, Doom 32X (Japan, USA) | 59.922743 Hz | 600 | −1.18% | −1.86% |
| Initial coverage core, Darxide (Europe), 32X | 49.701459 Hz | 600 | +0.36% | −1.24% |
| After inactive-path optimization, Sonic 2 (World) | 59.922743 Hz | 6,000 | +1.71% | +2.50% |
| After inactive-path optimization, Mega Bomberman (Europe) | 49.701459 Hz | 6,000 | +3.62% | −1.21% |
| After inactive-path optimization, Doom 32X (Japan, USA) | 59.922743 Hz | 600 | +3.39% | −0.14% |
| After inactive-path optimization, Darxide (Europe), 32X | 49.701459 Hz | 600 | +0.22% | +1.41% |

Paired changes describe unpaced processing capacity. The final image, PCM
from the next full-output frame and serialized native state match exactly
in every pair, including closed 32X runs with no state exclusions. Peak RSS
and per-pair rates are in the JSON ledger.

The end checkpoints were Sega logo, Mega Bomberman title menu, Doom start
menu and Darxide language selection. The initial fast MD pair for each
title showed a 7.6–7.7% loss. The optimization shares an instrumentation
mask with the debugger, uses one direct 68000 read gate and moves physical
origin capture to cold helpers. The repeat measured +2.50% in Sonic 2 and
−1.21% in Mega Bomberman fast mode. These are individual timing pairs.

Raw local logs and the harness are under
`build/verification/debugger-coverage`.

## Test build

The Windows package was deployed to the existing testbuild directory. All 73 staged
files matched their deployed SHA-256 hashes. User games, saves and settings were
preserved. This is a test build of 0.28.0, not a published release.
