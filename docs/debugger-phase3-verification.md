# Expansion debugger and editing verification

Date: 2026-11-21. Baseline: `7d3fdc9e3b6b7e1603045a9705b90117b91808ba`.
Machine-readable hashes, measurements and coverage: [verification data](debugger-phase3-verification.json).
Usage and limitations: [CPU debugger](debug-execution.md), [memory inspection](debug-inspection.md).

## Delivered scope

| Hardware | Executable targets and verification |
| --- | --- |
| Mega Drive, SMS, Game Gear, SG-1000 | Existing 68000/Z80 execution, traps and stepping retained; original guests verify DMA, IRQ, state, video and PCM continuation. Register/flag and bounded memory edits added. |
| Sega CD | Main 68000, Z80 and sub-68000; original Mode 1 guest delivers IRQ2 across CPUs. Safe PRG/Word RAM, cell/dot aliases and BRAM inspection avoids MMIO handlers. |
| 32X | Both SH-2s plus MD CPUs; instruction/data traps, delayed branches, nested calls, step over/out, register edits and pure RAM/ROM/framebuffer peeks. |
| Sega CD + 32X | All five CPUs share the existing scheduler; real-media stops and continuation checked. |
| MegaLD | Existing MD/CD CPU topology and inspection, with no duplicate player/emulation engine. Real MMI remains in place. |
| Firecore | Existing 68000/Z80 plus Firecore inspection; original tile/bitmap fixtures and two real games checked. |
| SVP | SSP1601 execution and word-addressed disassembly, register/flag edits, six hardware stack entries. Original SSP-to-68000 mailbox guest resumes exactly. Data watchpoints are not advertised because SSP program and data address spaces overlap. |
| Link Cable | Explicit A/B CPU and inspector selection, local debugger checkbox, exact suspended endpoint stack, teardown after completing the paired frame. Original MD and GG guests exchange actual cable signals, stop in B, alternate presentation roles and release the peer. |

Flags use masked writes to avoid stale-snapshot overwrites. Register edits require a completed-frame boundary or an EXEC stop on that CPU, never an in-progress data access. Physical memory writes require an explicit full-frame pause, compare the original bytes and generation, and are capped at four bytes. ROM/MMIO and unimplemented domains remain read-only. Mutation clears rewind history and is blocked during input recording/playback, netplay, Hardcore and export. Observation/stepping during input recording retains one committed tape frame per completed emulated frame.

SH-2 breakpoints select the existing interpreter; both SH-2 register contexts remain coherent during nested synchronization. With no armed events, normal translated execution remains selected. Its slow-memory adapters use the original direct access path, without interpreter breakpoint checks. No alternate guest engine or serialized debugger pointers were introduced.

## Focused checks

- Original guest integration: 68000, Z80, SSP1601, both SH-2s, sub-68000 and two linked instances. Native state, completed pictures, PCM and host clock compared against uninterrupted execution.
- SH-2 tests cover delayed branches, nested BSR/RTS with every delay slot counted once, step over/out, restored PR/SP, detach to DRC and save/restore continuation.
- SH-2 SR edits with a pending masked IRQ stop at the handler's first instruction without executing the canceled instruction or ignoring the cycle budget. Reset does not emit false data-read breakpoints.
- The bounded SH-2 decoder processed all 65,536 opcode words at ordinary and wraparound addresses under ASan/UBSan.
- Session tests cover partial-frame publication, save/load, rewind, reset/power reset, input recording, shutdown and mutation-policy rejection.
- Memory tests cover endian-correct RAM/VRAM/CRAM/VSRAM writes, boundaries, stale snapshots, rejected ROM/MMIO edits and atomic rejection of an invalid multi-color palette write.
- Qt offscreen tests cover CPU capabilities/addresses, flag/register editing, memory writes and peer selection. Saved CPU/memory previews were visually inspected.
- Linux and Windows Release builds completed. Every staged Windows package file matched its deployed `testbuild` copy. Windows was not launched or controlled; interactive testing is left to the user.

### SH-2 state comparison exception

The existing 32X save wire includes PPC (previous instruction) and EA (effective-address scratch). The interpreter updates these; DRC does not. Their only relevant reads are preceded by initialization in the same instruction, or belong to disabled donor diagnostics/idle code. Neither determines IRQs, scheduling or future execution.

The first fixture mismatch was four bytes in these two slave fields. Tests validate the container version and layout, permit differences only in those four 32-bit words across both CPUs, and require every other byte, picture, PCM sample and clock to match. Production saves and execution were not normalized or modified to make a comparison pass.

## Real-media continuation

Each checkpoint used two fresh loads and identical inputs. Pause and Step Into on each listed CPU stopped within the current frame. Detach completed that frame; it and the next five ordinary frames matched. All 15 processors and 90 completed-frame comparisons passed.

| Media | Starting completed frame | CPUs | Matched frames |
| --- | ---: | ---: | ---: |
| Sonic CD (Europe) | 1340 | 3 | 18 |
| Corpse Killer (USA), CD+32X | 1800 | 5 | 30 |
| Hi-Roller Battle (Japan), MMI | 300 | 3 | 18 |
| Wall Breaking | 520 | 2 | 12 |
| 40 Bonus Games in 1 / Jewel Magic | 850 | 2 | 12 |

Sonic CD used Start pulses and 40 frames of Right; the PAL disc was playing.
Corpse Killer used Start at frames 60–63: CD status was 1, adapter control
`$8083`, and both SH-2s executed. Its 32X VDP pixel mode was 0 at that checkpoint;
this is not a claim of verified shooting gameplay.
Hi-Roller used the opening checkpoint without input; side 1 video was enabled
and unblanked. Wall Breaking and Jewel Magic used Firecore VDP register
`$18 = $80`; Jewel's ROM offset `$34A874` and GPIO `$00` confirmed the second chip.
Images and BIOS were read at their original paths, never copied.

## Closed-tool performance

Pinned CPU 5, MAME YM2612, no shader or inspection windows. Each sample used
180 warm-up frames plus 3,000 measured frames. Normal mode renders every frame
and retains PCM; fast mode renders every 32nd frame and discards PCM. These are
unpaced processing-capacity measurements, not a claim of displayed FPS.
Before/after order alternated.

| Game | Normal capacity change | Fast capacity change | Pairs |
| --- | ---: | ---: | ---: |
| Sonic the Hedgehog 2, NTSC | −1.06% | −0.25% | 3 |
| Mega Bomberman, PAL | +1.26% | −0.53% | 3 |
| Doom 32X, NTSC | −0.18% | −2.00% | 2 |
| Darxide, PAL | −2.92% | −2.55% | 2 |

Values are median paired changes. Every timing pair matched native state,
final image and following-frame PCM exactly, including 32X without exclusions.
The fixed input schedule traverses startup/title/attract scenes; observed end
images were the Sega logo, Bomberman opening graphic, Doom menu and Darxide
3D spacecraft. These are not exhaustive gameplay benchmarks.

The initial 32X run showed roughly 4–5.5% fast-path overhead. Investigation
found DRC accesses routed through instrumented interpreter wrappers. Restoring
direct DRC accesses and reducing inactive interpreter checks cut that overhead
to the values above. Only the affected 32X measurements were repeated; MD
execution code was unchanged by that final optimization.

## Remaining scope

Phase 4 tracing, hardware timeline, coverage, pixel provenance and optional
debug-project persistence are not implemented by this batch. Invasive online
debugging remains disabled. Physical edits do not yet cover 32X RAM/cache,
SVP RAM, ROM, MMIO or wide native MD VRAM; unsupported controls are disabled.
