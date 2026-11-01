# 32X integration — experimental 0.7.0

## Boundary

One Genesis Plus GX fork, preserving its MD/CD CPU, VDP, mapper and sound implementations. 32X is an independent attached device, not a replacement core or a mutually exclusive system_hw value. Normal MD/CD execution must retain its fast path and allocate no SH2/code/framebuffer storage. Experimental modules are not exposed as working support until real boot, timing, audio and state checks pass.

The reference donor is irixxxx/PicoDrive revision 07abb00550528f7bf53087d932dd3d747cf2d0df: 32X devices and SH2 only. Preserve COPYING and per-file notices. Interpreter first provides a reference for later x86-64 DRC. ares dc4f4659d49693ac91705389e6c37734c016bf16 is a second behavior/reference source. Do not copy GPL BlastEm into this noncommercial GPX distribution.

## Contracts

- GPX clocks are master-clock ticks, not 68000 cycles. SH2 advances 3 cycles per 7 master ticks; retain fractional remainder and instruction overshoot. Synchronize on shared-memory/communication/DMA changes and event deadlines, preserve CD sub-68000 reentrancy guards. Never change clock ratios to improve throughput.
- Keep existing frame runners for MD/CD without 32X. Active 32X uses a separate event-bounded run-until path for the main 68000, both SH2s and CD hardware/sub-68000. The original scd_update retains its scanline semantics for ordinary CD execution; it is not called in small quanta.
- 32X bus overlays must preserve CD register handlers, cartridge mappers and GPX instruction-fetch/VDP-DMA base pointers. MD A151xx/A152xx and SH2 4000/4100/4200 accesses have different masks/ownership.
- Frame composition occurs before GPX loses MD transparent/backdrop metadata. 32X priority is independent of MD plane priority. Preserve H32/H40, interlace and Blargg behavior explicitly. No frame-sized allocation on scanline paths.
- PWM FIFO/timers/DREQ continue while audio output is muted or frames skipped. Mix timestamped deltas through GPX audio before clipping; do not reuse CD/YX5200 blip slots.
- States require a versioned independent 32X block (both SH2s, peripheral registers, memories, FIFO, interrupts, deadlines, PWM phase). Never serialize host pointers or executable code. Keep ordinary legacy states compatible. Restore bus maps and invalidate DRC code on load.
- Fixed memory/cache bounds; no per-frame file access or allocation. DRC failures must fall back safely, never exit the application. x64 rel32 helper reach must be retained or handled by long-call stubs.

## Acceptance sequence

These are acceptance stages, not a claim that every stage is complete. Current evidence and remaining limits are recorded below.

1. Independent C SH2 interpreter, video and bus/device components; focused synthetic execution/register/priority cases.
2. GPX overlays and master-clock scheduler: cartridge 32X BIOS boot and real image/audio.
3. CD + 32X simultaneous execution: provided Corpse Killer and Night Trap images; data FIFO and CD streaming.
4. State/rewind, reset, archive/loading/BIOS frontend integration.
5. x64 DRC, bounded caches and measured performance; unchanged MD/CD comparison. Windows/Linux builds, deploy only achieved functionality, cleanup, commit/push.

## Available read-only validation media

User supplied cartridge images in D:/emulation/Sega Super 32X/roms and BIOS in Fusion364/Bios (32X_G_BIOS.BIN 256 bytes, M 2048, S 1024). CD32X images are in D:/emulation/Sega CD/roms/Corpse Killer (USA) (Sega CD 32X) and Night Trap (USA) (Disc 1) (Sega CD 32X) (RE-1). No game or BIOS data belongs in the repository.

## Earlier component milestone

Before GPX integration, the standalone assembly ran both user-supplied SH2 BIOS images against the read-only Doom cartridge. Both memory/ROM startup checks completed: M_OK/S_OK at master clock 35181540, PCs 0203602a/020360fa (game SH2 entry code). At that stage GPX scheduling and video/audio were still pending; the integrated results below supersede that limitation. The original 4M/32M probe bounds expired during normal BIOS scans; the 64M bounded run completed.

Register/FIFO rules also checked against Sega Hardware Manual MAR-32-R4-072294: https://consolemods.org/wiki/images/e/e9/32X_Hardware_Manual_1994_Sega_text.pdf. DREQ uses two four-word banks; write space is freed at block boundaries. SH2 4006 FULL/EMPTY instead describes framebuffer write cache. PEN is available in H/V blank and blank/direct-color modes. These are component-level checks, not broad game compatibility evidence.

## Current integrated evidence

The shipped library is one Genesis Plus GX fork with an attached 32X device. Its MD/CD engines remain in use. Linux and Windows x86-64 use the SH2 DRC; unsupported targets or executable-memory failures fall back to the interpreter. KEGA32X_INTERPRETER=1 selects the reference interpreter for diagnosis.

Bounded, headless checks capture video and PCM with temporary user-data directories:

| Scenario | Observed result |
| --- | --- |
| Doom, 1200 frames with menu input | First playable level on Linux and native Windows; identical final video and whole-run PCM hashes |
| Chaotix, 1800 frames | Gameplay scene |
| Virtua Racing Deluxe, 1800 frames | Mode selection with rendered polygon scene |
| Corpse Killer CD32X, 3600 frames with Start input | Intro video; final image and whole-run PCM match the earlier interpreter run |
| Night Trap CD32X, 2400 frames | Title/intro video on native Windows |
| Doom, Corpse Killer, Night Trap | State/rewind roundtrip; malformed extension rejected without mutation; two restores replay identical subsequent video and PCM |
| Doom soft/hard reset, 600 subsequent frames each | Both reach the title screen; hard-reset frame equals a fresh 600-frame boot |

Doom's 1200-frame Windows run averaged 340.8 emulated frames/s and Night Trap's 2400-frame run 280.2. These uncapped averages include startup and exclude GUI, audio-device pacing and rewind, so they are not minimum gameplay rates. They establish tested scenes, not full game completion or broad compatibility.

Ordinary Mega Drive (The Lion King, 2400 frames) and Sega CD (The Terminator CHD, 1200 frames) retain identical final video and whole-run PCM versus the prior production core. Mega Drive throughput was 1928 versus 1963 frames/s; no regression was observed in this bounded comparison.

A disabled-IRQ FRT auto-clear timer previously forced millions of unnecessary CPU slices. Advancing its state arithmetically while scheduling only observable IRQ deadlines raised the same 300-frame Doom sample from 37.23 to 367.99 frames/s with identical video and PCM hashes. Exact idle-loop batching is separately guarded by side-effect-free instruction fetches.

The 32X machine codec is explicit little-endian and pointer-free: 535743 bytes including 512 KiB RAM. The complete GPX+32X state is 1572087 bytes. A reusable snapshot is allocated only when states/rewind are used. The DRC has a bounded 4 MiB generated-code cache; cache plus metadata/markers totals about 15.2 MiB, allocated only for an attached 32X. The optional full-colour Blargg table adds 8 MiB and is reused while enabled. Ordinary MD/CD never creates the 32X machine or these caches.

Component checks cover SH2 instructions, IRQ/delay slots, wait/end-run behavior, self-modifying code, bounded cache eviction, safe interpreter fallback, side-effectful fetch rejection, FRT bulk-vs-step timing, DMA, bus/FIFO, PWM, rendering and state replay. Linux/Windows focused checks passed; new timer/idle cases also passed ASan/UBSan. This is not a claim of cycle-exact hardware validation.

## Remaining limits

- Support is experimental. The small set of real-game checks does not establish every game's compatibility.
- Rendering is scanline based. Midline changes to 32X display state are not yet reproduced with pixel timing; the current render-sync callback does not flush partial lines.
- H40 composition is one-to-one. H32 currently scales the 256-pixel MD image to 320 pixels with nearest-neighbour sampling; exact hardware H32 geometry is not claimed.
- Framebuffer contention and some bus waits are approximated. SH2 watchdog reset output is not connected.
- On non-x86-64 builds the interpreter remains available but comparable full-speed 32X performance is not promised.
