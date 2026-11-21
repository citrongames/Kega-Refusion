# Debugger phase 1 verification

## Real core in Linux

`inspection_core_tests CORE BIOS_DIRECTORY` passed on the integrated Linux
core. Seven original generated cartridges exercised Mega Drive, Master System,
Game Gear, SG-1000, Firecore tile and bitmap modes, and 32X. An original
generated 150-sector ISO with a local NTSC-U BIOS exercised Sega CD and
Sega CD + 32X. The 32X video fixture writes through the main 68000 with both
SH-2s held in reset.

The tests checked active hardware, filtered catalogs, invalid arguments, bounded
and odd-offset reads, canonical guest bytes, PSG register values set by the
guest, and the absence of out-of-date domains after content changes and unload.
Cartridge tests also checked the 4 KiB frontend memory limit and complete
graphics-domain copies. All nine cases showed identical native states before
and after inspection. With the FM option forced on, the Master System fixture
reported the YM2413. The Game Gear and SG-1000 catalogs suppressed it.

## Two-game worker benchmark

The same release-mode Qt host ran both cores on logical CPU 5 of an AMD Ryzen
7 4800H in Linux WSL2. The baseline core was preserved from the preceding
workspace at `28c61f9`. Both games used the MAME YM2612 backend with Blargg
and rewind disabled.

- Sonic the Hedgehog 2 (World), title-screen checkpoint. [Sonic Team and Sega Technical Institute](https://www.youtube.com/watch?v=dv_rOiO8Pkg)
- Mega Bomberman (Europe), opening planet montage checkpoint. [Westone and Hudson Soft archival attribution](https://archive.gamehistory.org/item/efca1d6a-da2c-4988-8ef4-ebce154c06d4)

Each sample used 180 warmup frames and 600 timed frames with identical scripted
input. Start was held at frames 0–3 and 120–123, Right from frame 180, and C
for five frames every 60 frames from frame 240. Full output produced video
and PCM every frame at an unpaced rate.
Accelerated output drew every 32 frames and the final frame, with PCM output
muted. Inspection sampled all four categories, 256 bytes of work RAM
and full graphics domains every six emulated frames. The measurement covers
core and host capture work before the window draws the snapshot.

Each cell gives the two observed rates in frames per second. The differences
use the medians of the two samples. The short runs and sample count limit the
precision of the throughput comparison.

| Game and refresh rate | Mode | Previous core | Phase 1, idle | Phase 1, capturing | Idle vs previous | Capturing vs idle |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Sonic the Hedgehog 2, NTSC 59.922743 Hz | Full output | 2308.4 / 2279.8 | 2300.8 / 2229.8 | 2274.4 / 2240.8 | -1.3% | -0.3% |
| Sonic the Hedgehog 2, NTSC 59.922743 Hz | Accelerated | 3363.9 / 3565.0 | 3485.0 / 3505.2 | 3440.3 / 3429.8 | +0.9% | -1.7% |
| Mega Bomberman, PAL 49.701459 Hz | Full output | 2285.2 / 2218.7 | 2228.0 / 2082.9 | 2182.1 / 2154.3 | -4.3% | +0.6% |
| Mega Bomberman, PAL 49.701459 Hz | Accelerated | 2851.9 / 3012.1 | 2966.2 / 2775.4 | 2921.7 / 2947.1 | -2.1% | +2.2% |

All configurations matched per game on final RGBA pixels, the first PCM buffer
after normal audio resumed, and native state. Process peak RSS was
36.8–37.5 MiB across the samples. See the [raw measurements](debugger-phase1-verification.json)
for per-run peak RSS and checkpoint hashes. The performance comparison covers
the two listed Mega Drive scenes. Expansion coverage comes from the generated
Sega CD and 32X integration cases.

## Longer Mega Bomberman comparison

A follow-up compared the same cores and Qt host with inspection idle. It used
three interleaved pairs, each with 180 warmup frames and 6,000 timed frames.
The input schedule continued for about 120.7 emulated seconds at PAL 49.701459 Hz.
This sequence extended beyond the opening montage and ended at the title screen
with the start menu visible.

Rates below are emulated frames per second, previous core → Phase 1. The last
column gives the median of the three paired percentage differences.

| Mode | Pair 1 | Pair 2 | Pair 3 | Median paired change |
| --- | ---: | ---: | ---: | ---: |
| Full output | 2116.6 → 2158.1 | 2098.7 → 2144.9 | 2101.4 → 2130.1 | +2.0% |
| Accelerated | 3152.6 → 3175.8 | 3125.5 → 3193.1 | 3128.5 → 3173.0 | +1.4% |

All three longer pairs measured higher throughput with Phase 1. Final RGBA
pixels, resumed PCM and native state hashes matched in both output modes.


## Interface and packaged build

Linux offscreen inspection_view_tests passed for numeric memory inspection,
navigation, snapshot clearing/subscriptions and graphics decoding. The existing
debug_menu_tests passed for modeless output controls, menus and related behavior.
inspection_session_tests passed the latest-request burst, paused capture,
generation change, cancellation and unload checks. Default-size memory and
graphics windows were visually reviewed with generated data.

The Windows Release application and both core modules were built and installed
in the user's testbuild directory. SHA-256 of the installed EXE, core modules and
English/Russian catalogs matched the package staging files; values are retained
in the JSON report. No interactive Windows control or execution was performed.

MegaLD/Pico real-media inspection was not runtime-tested in this batch. Linked
console inspection and unsupported Firecore previews remain explicitly limited;
see [debug-inspection.md](debug-inspection.md).
