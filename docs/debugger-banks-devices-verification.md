# Bank labels, device inspection and simple watches

Verified on 2026-11-21, starting from commit 687ed18. No release/version change.
The native hardware-state layout and input-recording compatibility contract are
unchanged. [Machine-readable evidence](debugger-banks-devices-verification.json)
records binary hashes, measured scenes and deployment.

## Delivered scope

- Labels, comments and bookmarks distinguish physical sources/offsets per CPU
  and console. Aliases share annotations; banks at the same PC do not. Explicit
  navigation checks the current map and never writes a mapper register.
- Project format 2 stores physical identity and a logical navigation hint.
  Version 1 projects and plain text symbol imports preserve logical semantics.
  A queued opcode with unknown origin is not attributed to a new bank.
- Firecore's graphics window includes a YUV bitmap snapshot. Clicking a 2×2
  block navigates to VRAM; inactive mode and truncated snapshots are explicit.
- SSP1601 supports read/write stops on implemented memory operations. See the
  [address map and stop semantics](debug-execution.md#execution-semantics).
- RAM Watch has name/address/value columns. Cheat search can add a selected
  result directly. Changed values highlight briefly; previous value/change
  count are tooltips, and device/format/byte-order selectors are advanced options.
  The same action is available from Cheats and Debug.

## Focused checks

| Check | Evidence |
| --- | --- |
| Physical mapping | Original SMS switches two banks at one CPU address and aliases one bank; MD self-bank-switch distinguishes cached opcode from current mapping; 32X prefix remains boot-owned while the copied tail maps to cartridge ROM. |
| Navigation and persistence | Qt offscreen checks separate bank labels and A/B definitions. Real-core checks acknowledge the exact navigation request, reject wrong banks without mutation, ignore stale polling navigation and follow PC on a stop/reset. Version 1/2 round-trips and failed-load/save preservation pass. |
| Firecore | Generated guest VRAM fixture agrees with the native renderer within framebuffer color quantization. Independent asymmetric YUV test, H32 fixed pitch, H40 last block and active/inactive tab changes pass. Capturing leaves serialized state unchanged. |
| SVP | Generated original instructions exercise RAM0/1, program operands, PM ROM/DRAM/IRAM, byte overlap, PC/value/size and post-access edit rejection. Continuing reproduces complete frame, audio and state. Existing expansion checks, including CD, pass. |
| Watch/cheats | Selected MD/SMS search results and watch samples have identical canonical bytes and endian interpretation. UI, search, cheat-session and bounded update checks pass. Hidden windows do not request watch samples. |
| Lifecycle | Suspended-frame publication, saves/loads, rewind, resets, input-recording cursor and shutdown checks pass. |
| Builds/deployment | Linux and Windows builds completed. All 73 staged Windows files matched deployed SHA-256 values in testbuild. Windows UI execution is left to the user. |

## Performance

The initial SVP implementation was compared in five crossed runs of 3,600 frames
on Virtua Racing (NTSC-U, 59.923 Hz: title, course selection, race grid and live
opponents) and Mickey Mania (PAL, 49.701 Hz: intro and first stage). Complete final
state, full PCM stream and ten image checkpoints matched byte-for-byte. Median
paired elapsed time changed by +2.59% and -0.08%, respectively.

The rare SVP stop handler was then moved out of line: the generated interpreter
loop shrank from 11,483 to 10,853 bytes. One final clean Virtua Racing pair measured
3.296 seconds before and 3.427 seconds after (+3.97%), again with identical
state/audio/video. This is not enough to claim a stable performance improvement
or zero overhead. Three intermediate timing pairs overlapped compilation and
are explicitly excluded. No further optimization sweep was performed.

Physical label lookup runs only when a debugger snapshot is requested. It
resolves at most the displayed 32 instructions and does not enable coverage or
per-frame tracing. Firecore uses the existing requested VRAM snapshot, and watch
samples retain the existing 128-entry limit and shared 100 ms visible-window
refresh.

## Remaining limits

Opaque opcode backing and unproven queued-opcode origins use logical labels.
SSP RAM0/RAM1 debugger addresses are distinct from physical hardware bus
addresses. Unsupported PM modes and raw register traffic are not invented as
memory events. The Firecore view is a snapshot, not exact raster provenance.
Pixel provenance, profiling, scanline/IRQ run controls and reverse-instruction
stepping remain outside this batch.
