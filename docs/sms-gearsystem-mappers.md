# Master System mapper coverage

Compared the 23 selectable mapper families in [Gearsystem](https://github.com/drhelius/Gearsystem/blob/master/src/Cartridge.h) against the address decoders in our patched Genesis Plus GX core. IrataHack was the one missing family. Family coverage does not establish compatibility with every dump.

| Gearsystem family | Genesis Plus GX handler or hardware |
| --- | --- |
| ROM only / SEGA / Codemasters | NONE / SEGA / CODIES |
| SG-1000 / Jumbo Dahjee | SG hardware and cartridge RAM variants / RAM_8K_EXT1 |
| Korean / MSX / Janggun | KOREA_16K_V1 / MSX_8K and Nemesis / KOREA_8K |
| Multi 4PAK / EEPROM 93C46 | MULTI_16K / 93C46 |
| Korean MSX SMS 8000 / SMS 32KB 2000 / MSX 32KB 2000 | ZEMINA_4x8K / MULTI_32K / HWASUNG |
| Korean 2000 XOR 1F / MSX 8KB 0300 / 0000 XOR FF | MULTI_4x8K / MULTI_8K / ZEMINA_16K_32K |
| HiCom / Korean FFFE / Korean BFFC | HICOM / MULTI_2x16K_V1 / MULTI_2x16K_V2 |
| Korean FFF3 FFFC / MD FFF5 / MD FFF0 | MULTI_32K_16K / MULTI_16K_32K_V1 / MULTI_16K_32K_V2 |
| IrataHack | IRATA |

## IrataHack

The known Wee Knight's Escape Trilogy image is selected by CRC32 49AF30F8. The board uses four 128 KiB groups, a switchable 16 KiB window and a 512 KiB SST39SF040 flash. The emulator implements byte programming, 4 KiB sector erase, chip erase and software ID. Flash operations complete immediately. An unchanged cartridge needs no save file. See the [board author's discussion](https://github.com/drhelius/Gearsystem/pull/141) and the [flash datasheet](https://ww1.microchip.com/downloads/aemDocuments/documents/MPD/ProductDocuments/DataSheets/SST39SF010A-SST39SF020A-SST39SF040-Data-Sheet-DS20005022.pdf).

Generate the independent diagnostic with `python3 tests/sms_irata_mapper_tests.py --export irata-test.sms`. The first boot of a new diagnostic displays SAVE FRESH. After closing and reopening it, SAVE PASS verifies the previously programmed marker. Retail game verification is pending.

## 4 PAK All Action (Australia)

The 1 MiB dump (CRC32 `A67F2A5C`, SHA-1 `2f8b5a66381dbbe7524af7af6372fbb6a6575ac5`) selects the existing 4 PAK mapper. The menu copies a bank-switch routine into work RAM at `D000`. It writes `3FFE` and `7FFF`, then jumps to `0000` in the selected game.

| Game selected through the menu | Frames compared | Writes to BFFF, including menu | Upper banks observed after leaving bank 00 (hex) |
| --- | ---: | ---: | --- |
| Adventure Kid | 1,500 | 1,946 | 11, 13, 17 |
| Power Block | 4,200 | 21 | 22, 27, 28, 2F |
| Twin Mouse | 2,100 | 2,046 | 19, 1A, 1B, 1C, 1F |
| Cave Dude | 2,100 | 2,403 | 32, 34, 35, 36, 38, 39, 3D, 3E, 3F |

In additional controller checks, Adventure Kid scrolled the island level to the right, Power Block raised the score from 200 to 400, Twin Mouse moved right to the blocks and then back left, and Cave Dude scrolled World 1 Stage 1 to the enemies and lost a life. These were short gameplay routes.

On core revision `5d17a1f`, the upper 16 KiB bank changes on a write to `BFFF`. An independent probe on the same unmodified dump selected bank 03, wrote 30 to `3FFE`, then wrote 03 to `BFFF`. The upper window held bank 03 after the first register change and switched to 33 after the second. All 16 KiB of the window matched the corresponding dump bytes after each write.

For comparison, a second run updated the upper map immediately on `3FFE` with the last raw `BFFF` value, using the same initial state and controller input. Each game produced exactly one interval where the two models exposed different banks. There were zero upper-window data reads during those intervals. The hashes of the complete video and PCM streams matched between models for each route.

[Bock's original cartridge investigation](https://www.smspower.org/forums/13548-Sega8BitWinterOfDumpsPart24PAKAllAction) gives the upper bank as the sum of the group bits from `3FFE` and the value written to `BFFF`, masked to 64 banks. [MEKA](https://github.com/ocornut/meka/blob/master/meka/srcs/mappers.cpp) applies it on `BFFF`; [Gearsystem](https://github.com/drhelius/Gearsystem/blob/master/src/Multi4PAKAllActionMemoryRule.cpp) also applies it on `3FFE`. The published cartridge measurements do not establish which bank an immediate read between those two writes would see. No gameplay fault attributable to this difference was found in these runs.

## Candidates for hardware or cartridge verification

* HiCom: check which bank is visible at 8000-BFFF. Hwasung: check values beyond 0 and 1 at its selector.
* Korean 2000 XOR 1F and FFF3/FFFC: compare address aliases, especially BFF3/BFFC on the latter board.

These are differences between the implementations. Record a cartridge access or hardware reference before changing a decoder. Of 657 loose SMS files surveyed by CRC, 18 matched the special Gearsystem entries checked (5 Codemasters, 4 Korean A000, 7 MSX including Nemesis, one Janggun and one 4PAK). The survey did not inventory archive contents.
