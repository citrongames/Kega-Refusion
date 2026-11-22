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
32X hardware, so select **32X: Connected** explicitly. Open the original CUE;
there is no converted/patched game copy.

## Loader compatibility

An active CD32X program RAM cartridge also selects the ares-style loader
profile. On the Sega CD sub-68000, a word or longword data access at an odd
address uses the preceding even address. The main 68000 and sessions without
the active RAM cartridge retain their normal address-error checks. This
behavior is an emulator compatibility feature. A physical MC68000 raises an
address error for an odd word or longword operand.
[Motorola M68000 User's Manual, section 6.3.10](https://www.nxp.com/docs/en/reference-manual/MC68000UM.pdf#page=104)

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

## Verification — 2026-11-22

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

The generated fixture also checks odd word and longword reads and writes,
byte accesses, and strict address-error behavior with the cartridge disabled
or 32X disconnected.

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
