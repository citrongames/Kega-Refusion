# CPU debugger verification — 2026-11-21

Phase 2 follows the Phase 1 baseline `38ad2e2`. No release/version change.

## Focused checks

- Original generated ROMs exercise the actual Mega Drive, Master System,
  Game Gear and SG-1000 runtime profiles. No copyrighted ROMs are committed.
- 68000/Z80 execute/read/write stops, ranges, register/masked/access-size/hit
  conditions, ignored hits and one-shot behavior pass. Unchanged breakpoint
  counts and one-shot disables survive a stale GUI configuration update.
- Step into, over and out, self-branches, Z80 indexed/prefixed instructions,
  safe stack/memory reads, unreadable MMIO and address wrapping pass.
- Stopped frames leave the completed-frame clock, displayed image and PCM
  publication unchanged. Completing five frames after repeated stops matches
  uninterrupted native state, image, PCM and clock.
- A generated MD ROM starts a 4,096-word H32 ROM-to-VRAM DMA. The debugger
  stops after the trigger while DMA remains active, then before the first
  instruction of the delivered VBlank handler. Exception PC/SR/stack, both DMA
  endpoints, handler execution and four completed frames match the reference.
- The worker test covers paused snapshots/steps, save/load, rewind, reset,
  power-reset breakpoint retention, input recording and shutdown while stopped.
  The recording contains exactly two completed inputs after stopping its
  second frame midway.
- Offscreen Qt checks cover action dispatch, CPU selection, range breakpoints,
  disassembly, register/stack presentation and symbol navigation. Labels survive
  reset/state restoration for the same content and clear when content changes.
  Default-size screenshots were inspected.
- Native decoder checks include all sixteen MC68000 short `$6xFF` branches
  and the `$61FF` call flag. Bounded decoder smoke passed ASan/UBSan.
- Linux and Windows Release builds compile. Windows execution and native-window
  interaction are left to the user, as requested.

Sources: `tests/debug_execution_tests.cpp`,
`tests/debug_execution_session_tests.cpp`, `tests/cpu_debugger_view_tests.cpp`.
Pass a core path to the two execution tests; the view test runs with
`QT_QPA_PLATFORM=offscreen`. CTest registers real-core tests when
`KEGA_TEST_CORE` is set.

## Closed-tool overhead and regression scope

Same Linux frontend, MAME YM2612, pinned CPU 5, alternating old/new core order,
180 warm-up and 6,000 measured frames per run, three pairs per game and mode.
Normal renders/captures audio every frame; accelerated mode renders every
32nd frame and omits frontend PCM capture.

| Cartridge | Actual refresh | Normal paired median | Accelerated paired median |
| --- | ---: | ---: | ---: |
| Sonic 2 | NTSC 59.922743 Hz | +2.645% | +1.236% |
| Mega Bomberman | PAL 49.701459 Hz | −0.589% | −2.657% |

These are unpaced throughput changes, not live display FPS or measurements of
an open Qt debugger. The Bomberman accelerated loss is retained in the report.
Peak RSS was approximately 37–38 MiB, with no retained debug coroutine when
closed.

Sonic 2 checkpoints include title, Emerald Hill split-screen attract play and
the Sega logo. Bomberman includes the space intro, copyright screen and title.
The same final pixels, following-frame PCM and native-state hashes matched in
all pairs. This is an intro/title/demo workload, not a claim about every scene.

Sonic CD (USA, title) and Virtua Racing Deluxe (USA, Big Forest ranking) used
900 frames per mode with their respective hardware confirmed active. Both
were NTSC 59.922743 Hz; pixels, PCM and native state matched the baseline in
normal and accelerated modes. This checks that dormant hooks leave these
expansions unchanged; expansion CPU debugging remains unsupported.

The final cold disassembler correction for short MC68000 branches happened
after timing. It changes no CPU execution logic; timings were not repeated or
claimed anew for this decoder-only adjustment.

Machine-readable paired results and deployment hashes:
[debugger-phase2-verification.json](debugger-phase2-verification.json).
Local logs and reproduction tools are under ignored
`build/verification/debugger-phase2`.

## Explicit limits

No CD/sub-68000/SH-2/SVP/Firecore/Link Cable execution debugging, arbitrary
register/memory writes, Z80 port-I/O breakpoints, instruction tracing,
reverse-instruction execution or bank-qualified persistent symbols is claimed.
Step-out follows returns and stack levels; it is not a reconstructed call stack.
No Windows runtime test or all-games/platforms validation is claimed.
