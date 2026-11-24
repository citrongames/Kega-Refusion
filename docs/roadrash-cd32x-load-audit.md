# Road Rash Rapido CD32X race-load audit

**2026-11-24:** The temporary odd-address compatibility profile described below
was removed at the user's request, pending an author-side loader fix. The RAM
cartridge remains supported with strict MC68000 address errors. This document
preserves the historical diagnosis; a new game revision has not been tested.

The initial CD32X program RAM implementation booted the original disc to the
menu. In the captured race-load failure, the loader consumed a stale sector
before a new read reached the drive, then attempted an odd MC68000 longword
access. The scoped loader compatibility profile introduced afterward reaches
The City with the 16 MiB cartridge selected.

## Historical failure reproduction

- Original `RoadRashRapido.cue` with a MODE1/2352 data track, mounted read-only
  using an isolated data directory.
- Disc-booted Sega CD with 32X connected and 16 MiB program RAM, NTSC-U at
  59.922743 Hz. The run used the SH-2 interpreter.
- US Model 1 BIOS 1.10, October 11, 1992, product BR 000003-1.10.
- Use the initial RAM-cart core build with strict sub-CPU alignment checks.
  Restore a menu checkpoint from the same configuration and hold Start for
  four frames from zero-based frame 10. The trace failed within three frames.

```sh
KEGA32X_INTERPRETER=1 core32x_game_probe CORE BIOS_DIR CUE 60 OUTPUT.png \
  --ram-cart=16 --load-state=MENU_STATE --start-at=10 --checkpoint=60
```

## Trace

1. The directory cache at sub-CPU Program RAM `$2CD28` initially held the
   expected `$22`-byte ISO directory record. The 109 records in the on-disc
   root directory at LBAs 23–25 all had even offsets and lengths.
2. At sub-CPU PC `$D774`, the loader requested LBA 24, count 1, with
   destination `$2CD28`. It called `CDCSTOP`, submitted a ROM read through
   BIOS function `$20`, then called `CDCSTAT`, `CDCREAD` and `CDCTRN`.
   Before `CDCSTAT`, BIOS RAM `$5AF2` contained the command `$8020` followed by
   LBA 24 and count 1. The BIOS handler at `$2F4C` had set bit 7 to queue
   the request. At `$1D10`, `CDCSTAT` observed unequal software ring pointers
   at `$5A72` and `$5A74` (`$0008` and `$0028`) and reported prepared data.
   The guest checked the carry results and called `CDCACK` after transfer
   without an intervening `CDBCHK` or validation of the returned header.
3. The BIOS copy at `$1DCA` read the CDC host data register `$FF8008` into
   `$2CD28` at sub-CPU tick 355062. The request sequence had begun at tick
   345694 and the drive status remained `$0102` (playing). Breakpoints
   recorded zero CDD commands and zero CDC decoder updates between request
   and transfer. The returned header was `$00137201`, or MSF 00:13:72,
   Mode 1. The copied bytes match LBA 897 of `RRFE16.BIN;1` starting at file
   offset `$41000`.
4. The loader interpreted the replaced cache as directory data. The sub-CPU
   executed `MOVE.L (6,A1),$D85C` at `$D612` with `A1=$2CD75`.
   Vector 3 led to `$5F40` with fault address `$2CD7B`, instruction
   register `$23E9` and saved PC `$D616` on the exception stack.
   The BIOS then cleared the CD ASIC reset bit at `$026C` and restarted the
   subprogram initialization. It posted low flag `$49` and waited for
   main high flag `$42` at `$608E`. The main loop echoed `$49` and
   waited for the low flag to clear at `$FF102E`. The flags stayed
   `$4949` throughout the black screen.

### Model 1 firmware service

The ROM read entry (`0`) copies its parameters into the mailbox and sets bit 7 at `$2F4C`
before returning. In this BIOS, `CDBCHK` at `$2EAE` checks the same bit and
returns it as carry. IRQ level 4 enters at `$5F88`, reaches the handler at
`$0610` and resumes the drive-command coroutine through `$2F5A`. That
coroutine clears the mailbox bit at `$2FAC` and dispatches the read request.
The read handler at `$3B36` calls `$1CFC`, which copies the ring's write pointer
into its read pointer and discards the queued data. `CDCSTART` also flushes
the ring at `$202C`.

At the reproduced transfer, the 50 MHz CD scheduler had its next sector tick
due at 562685. The BIOS queued the new read by tick 348686 and copied data at
355062, with the drive at LBA 2277. IRQ level 4 was enabled, with the CDD
host clock enabled and no pending ASIC interrupt or asserted sub-CPU IRQ.
Vector 3 ran at tick 546310, before the scheduled sector tick. These values
establish the ordering in this run and give the reference trace specific
events to compare.

## BIOS comparison

The second trace used US Model 2 BIOS 2.00W, built-in 32X BIOS and a menu
checkpoint made with that configuration. When the mailbox at `$5AC8` held
`$8020`, the ring cursors at `$5A48` and `$5A4A` were still `$0008` and
`$0028`. The BIOS wrote the same LBA 897 payload to `$2CD28` at tick
358909, with next sub-CPU PC `$18D8`. The drive was at LBA 2989 with its
next sector tick due at 656051. The current CDC header was `$00416301`
(LBA 2988), while the transfer returned the queued header `$00137201`
(LBA 897) for the requested LBA 24. The same `$D612` instruction raised
vector 3 with fault address `$2CD7B` at tick 550157.
The longer follow-up stayed black for 3,000 frames and showed both CPUs in
the same communication loops. At the user's request, a further 36,000 frames
(600.77 emulated seconds) were allowed without input: the same black output and
communication wait remained. This distinguishes the failure from an ordinary
black loading screen.

The clean Model 2 2.11X BIOS from the newly supplied collection was also booted
from scratch through intro/title/menu. Starting the selected track remained
black for 6,000 further frames; the register snapshots show the same main/sub
communication wait. The detailed first-fault hardware traces above were taken
for 1.10 and 2.00W, not independently repeated for every BIOS revision.

| US BIOS image in the supplied collection | CRC32 | SHA-256 |
| --- | --- | --- |
| Sega CD Model 1 1.10 | `C6D10268` | `807384678ef259494d943eddb6954dfc7069693d0dde0c424e2028488d88e81a` |
| Sega CD Model 2 2.00W | `9F6F6276` | `b1c2036b79467514eaaa69c76e5ba83801621e821059caa53cb921a0b21e3af4` |
| Sega CD Model 2 2.11X | `2E49D72C` | `4acda6ced2951f4fc34b42cc08c7510b8edc1f716b17ce4f84920751a98b9204` |
| Sega CDX 2.21X | `D48C44B5` | `559a2914e18b91b772629a3088ebecfd34128ed9880fc31c286add7f76a6a8f6` |

The first two images match the previously used files byte for byte. The
frontend accepts the 2.11X image through its existing Model 2 BIOS validation.
CDX has a distinct BIOS title and drive/CDC model. Its image requires frontend
support beyond the current Sega CD BIOS importer.

## ares reference run

The stock MicroNut99 ares fork reached the active The City race using the
same original CUE, matching US Model 1 1.10 BIOS and 16 MiB RAM cartridge.
The headless replay restored a title checkpoint from a fresh boot, pressed
Start at frames 10–15 and 180–185, then pressed B at frames 700–707.
It rendered the race at frames 2200 and 2500, after the black transition.
Compact reproduction notes, screenshots and trace excerpts are retained under
`build/verification/roadrash-cd32x/ares-reference/`; temporary reference builds
and large intermediate captures were removed after comparison.
[ares reference revision](https://github.com/MicroNut99/ares_android/tree/a50891a600e1fb9e3854f0327bbe408f6e41cbc9)

Replacing Start with B in the Kega Refusion Model 1 menu checkpoint preserved
the exact failure timing. It queued the same command by tick 348686, copied the
same stale sector at 355062 and took vector 3 at 546310.

An instrumented run of the same ares revision reproduced the stale transfer
during a successful race launch. The loader requested LBA 24 at `$D774`.
The BIOS copied LBA 897 with header `$00137201` and first words
`$4D6E $CCE6` into `$2CD28`. The loader reached `$D612` with
`A1=$2CD75` and effective address `$2CD7B`, then continued the scan
and reached `$D612` again with `A1=$2CE37`. Only after this did the
next IRQ4 appear. The continued scan requested LBA 25, and the BIOS eventually
copied the correct sector with header `$00022501`. The run subsequently rendered the race.
These events were recorded at lines 5170–5220 and 37774–37791 of the original
reference trace. The decisive stale-sector/odd-access excerpt is retained as
`build/verification/roadrash-cd32x/ares-reference/sub-cd-trace.txt`.

## Interpretation

The old-sector transfer occurs in both emulators. Sega's BIOS manual
describes `CDBCHK` as a drive-command completion check, `CDCSTAT` as a
data-availability check and `CDCREAD` as returning the prepared sector's
header. The captured loader consumes the available sector before its new
request reaches the drive and begins parsing it as directory data.
[Sega Mega-CD BIOS Manual, sections 3-B and 3-C](https://segaretro.org/images/archive/4/44/20190509114241%21MCDBios.pdf)

The observed execution diverges at the odd longword operand. Kega Refusion
raises the MC68000 address-error exception. The ares reference clears address
bit 0 in its word and longword bus helpers, so the same instruction can read
from the rounded-down address and the directory scan continues.
[Motorola M68000 User's Manual, section 6.3.10](https://www.nxp.com/docs/en/reference-manual/MC68000UM.pdf#page=104), [ares reference M68000 memory helpers](https://github.com/MicroNut99/ares_android/blob/a50891a600e1fb9e3854f0327bbe408f6e41cbc9/ares/component/processor/m68000/memory.cpp#L17-L27)

The initial Kega Refusion build with strict sub-68000 alignment checks stayed
black after selecting a race with US Model 1 1.10 and Model 2 2.00W/2.11X
BIOS revisions. The comparison informed the cartridge-gated compatibility
profile below.

## Verification with loader compatibility

With program RAM active in the disc-booted Sega CD + 32X session, the core
uses the preceding even address for odd word and longword data accesses by the
Sega CD sub-68000. This reproduces the ares loader behavior as an emulator
compatibility feature. The main 68000 and sessions without the active
cartridge retain their normal address-error checks.

The Linux core resumed the same US Model 1 1.10 menu checkpoint with 16 MiB
and held B for eight frames from zero-based frame 10. The City displayed GO
by frame 1500 and remained active through frame 3000. The run used the SH-2
JIT with zero fallback calls. Native and rewind state restoration reproduced
subsequent video and PCM. The log and frame captures are under
`build/verification/roadrash-cd32x/compat-race*`.

This verification covered The City using the read-only disc image. Race
startup on physical Sega CD and 32X hardware remains unverified.
