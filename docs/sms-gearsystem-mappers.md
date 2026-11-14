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

## Candidates for hardware or cartridge verification

* 4 PAK All Action: compare upper-window reads immediately after a write to 3FFE. A local Australian dump is available.
* HiCom: check which bank is visible at 8000-BFFF. Hwasung: check values beyond 0 and 1 at its selector.
* Korean 2000 XOR 1F and FFF3/FFFC: compare address aliases, especially BFF3/BFFC on the latter board.

These are differences between the implementations. Record a cartridge access or hardware reference before changing a decoder. Of 657 loose SMS files surveyed by CRC, 18 matched the special Gearsystem entries checked (5 Codemasters, 4 Korean A000, 7 MSX including Nemesis, one Janggun and one 4PAK). The survey did not inventory archive contents.
