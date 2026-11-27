# CD32X program RAM cartridge

This is an optional, experimental cartridge used by CD loaders to stage program
code and assets for the 32X. It is separate from the official Sega CD backup RAM
cartridge. It is **off by default** and never selected by a game's filename or hash.

## Selection

In Settings → Games, select 4, 8 or 16 MiB of CD32X program RAM and connect the 32X.
Changes apply on the next game load or Power, not immediately or on Reset.
The device exists only with a disc-booted Sega CD + 32X; ordinary cartridges,
MegaLD and other consoles retain their existing maps and allocations.

Road Rash Rapido uses a US Sega CD boot sector. Its author requests the 16 MiB
board and a US CD BIOS. The disc's generic `SEGA CD Loader` header does not identify
32X hardware, so the old image uses the content-based 32X detection described below. Open the original CUE;
there is no converted/patched game copy.

## CPU address alignment

The RAM cartridge does not change MC68000 alignment rules. Odd word and longword
accesses raise Address Error on both 68000 CPUs; byte accesses may be odd.

A narrow compatibility exception supports the original Road Rash Rapido CD32X
image. At mount, the core recognizes the exact old APP executable from logical
Mode 1 sectors, independent of image filenames. It enables 32X in Auto mode;
an explicit Disconnected selection still wins. Program RAM remains a separate
manual setting (16 MiB for this game).

Only the two confirmed odd longword reads in the original sub-CPU directory
scanner are rounded down, with Program RAM attached and the original instruction
sequence still present. All other addresses/instructions/writes retain their
normal exceptions. The profile resets on disc replacement/unload. Changed APP
code does not match; a new repaired release must use its own 32X header/detection.
The profile is reflected in recording/network compatibility IDs, without
changing native state layout. This is a game compatibility exception, not a
claim about physical RAM-cartridge behavior.

## Bus model

- Nonbootable cartridge: CART_IN remains inactive and the Sega CD BIOS owns boot.
- 4 MiB bus aperture, split into two 2 MiB pages for the 8/16 MiB variants.
- Reset selects physical pages 0 and 1 without erasing RAM.
- Upper-byte /TIME writes select pages at A130F0/F2 and decoded mirrors
  (CA2 clear, CA1 selects the window). Odd-byte writes do not select a page.
- The 8 MiB board masks the page index to two bits; the 4 MiB board is linear.
- Direct 68000 window at 400000–7FFFFF when ADEN is off or RV is set.
- With ADEN enabled and RV clear, 880000–8FFFFF is read-only; 900000–9FFFFF
  is read/write through the 32X's existing 1 MiB bank selector.
- SH-2 reads use the same pages through its cartridge aperture. SH-2 cannot write
  this memory. Blocked 68000 data **and instruction** fetches see open-bus storage.
- Cartridge writes invalidate compiled SH-2 code before its next execution;
  a loader's consecutive writes coalesce, and data-only writes do not flush
  translated BIOS/SDRAM code when no cartridge code has been fetched.

Storage is allocated only when attached, released on unload, and initialized to
FFFF. Native states contain the RAM and page latches. Capacity-mismatched and
malformed states are rejected before restoring the device. Ordinary states retain
the previous wire format. Input recordings and netplay snapshots carry the chosen
hardware configuration; missing legacy metadata means disabled.

The current implementation retains RAM across Reset and in save states,
but not across Power or unloading the disc: it does not create a separate battery image
for the program cartridge. Existing CD internal/backup saves remain independent.
A physical battery image and write-protect switch are not exposed in this version.
Subcycle cartridge-bus arbitration remains the existing 32X scheduling model;
this is not a claim of a cycle-exact electrical model or retail-hardware validation.

## References

- [RAM-cart hardware](https://consolemods.org/wiki/Genesis:MD_32X_RAM_Cart)
- [4 MiB PCB](https://github.com/MicroNut99/MD-32X-RAM-Cart-32Bit)
- [Paged 4/8/16 MiB reference implementation](https://github.com/MicroNut99/ares_android/blob/main/ares/md/cartridge/board/ram-cart.cpp)
- [32X windows](https://github.com/MicroNut99/ares_android/blob/main/ares/md/m32x/bus-external.cpp)
- [Road Rash CD32X requirements](https://anibalsmith.itch.io/road-rash-segacd-32x)

## Historical verification — 2026-11-22 (before workaround removal)

`tests/cd32x_ram_cart_tests.py` executes an original generated 68000 program
inside a generated CD Mode 2 fixture. It verifies disabled/4/8/16 MiB, page
selection/mirrors, byte lanes, RV windows, reset and state restoration/rejection.
No retail BIOS or game is needed for this check.

Frontend integration checks cover default/persisted selection, power-only
application, legacy recording metadata, real 16 MiB input-recording playback,
and a 4 MiB authoritative peer snapshot with an 8 MiB local preference.

Road Rash Rapido reached The City with the loader profile active, 16 MiB
program RAM and the US Model 1 1.10 BIOS. The run resumed the menu, pressed B
for eight frames from frame 10 and displayed GO by frame 1500. It ran through
frame 3000 at an emulated refresh of 59.922743 Hz with zero SH-2 JIT
fallback calls. Native and rewind state restoration reproduced the subsequent
video and PCM. The test covered The City using a read-only disc image.
Race startup on physical hardware remains unverified.
[Road Rash Rapido loading audit](roadrash-cd32x-load-audit.md)

The updated generated fixture separately checks Address Error for odd word and
longword reads and writes with the cartridge enabled, disabled, and 32X
disconnected. Odd byte reads and writes remain valid.

The cartridge fixture additionally runs SH-2 code from RAM, rewrites its opcode
from the 68000 and switches its physical page at the same guest PC. Both JIT and
interpreter observe the new code. Data-only cartridge writes preserve warmed
SDRAM translations. The SH-2 component test covers an original short MAC.L block
whose host code exceeds the previous per-instruction size estimate, transient
fallback and cache pressure.

Preceding-core comparisons, 2,400 frames with the same scripted inputs:

| Scene | Actual refresh | Final frame and captured PCM | Before / after unrestricted fps |
| --- | --- | --- | --- |
| Doom (Japan, USA), id/Sega | 59.922743 Hz | Identical | 484.95 / 481.65 |
| Darxide (Europe), Frontier | 49.701459 Hz | Identical | 136.80 / 135.82 |

These single local timing samples measured about 0.7% lower unrestricted
throughput in both scenes.
Before the sub-CPU compatibility change, the separate SH-2 compiler
correction eliminated fallback calls and retained identical video and PCM
for the original failing transition.

Additional checks with the program cartridge **disabled** compared the final
core against the preceding binary:

| System and title | Refresh | Frames | Result |
| --- | --- | --- | --- |
| Sega CD: Sonic CD (Europe) | 49.701459 Hz | 3,600 | Intro final frame and complete captured PCM identical |
| Sega CD: The Terminator (USA), CHD | 59.922743 Hz | 3,600 | Intro final frame and complete captured PCM identical |
| CD+32X: Corpse Killer (USA) | 59.922743 Hz | 3,600 | Six sampled intro frames and complete captured PCM identical |
| CD+32X: Night Trap (USA), Disc 1 RE-1 | 59.922743 Hz | 2,400 | Four sampled intro/title frames and complete captured PCM identical |

Both ordinary CD runs and both CD+32X runs reproduced future video and audio
after native state and rewind-state restoration. These checks cover the
recorded startup/intro scenes, not full-game playthroughs.


## Removal verification — 2026-11-24

Removed the sub-CPU alignment override and its bridge accessor; no cartridge
mapping, state layout or allocation changes. The real-core RAM fixture passes
for disabled/4/8/16 MiB, state compatibility and SH-2 code coherency. Separate
odd word/long read/write cases now take vector 3 with RAM enabled, disabled,
and 32X disconnected; the odd byte read/write control succeeds.

Doom (Japan, USA), NTSC-U 59.922743 Hz, and Darxide (Europe), PAL 49.701459 Hz:
600 cold-boot frames each, no input, before/after cores. Final-frame RGBA and
complete PCM hashes match in both games. This is a short unaffected-system
check, not a new full-game audit. Recordings with an active CD32X RAM cartridge
now receive a distinct execution compatibility identifier so older recordings
can warn rather than silently claim the same behavior. Native state layout
and compatibility are unchanged. The Windows Release test build contains
this removal; a repaired Road Rash revision remains to be tested when available.
