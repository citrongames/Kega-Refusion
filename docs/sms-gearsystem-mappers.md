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

## HiCom and Hwasung

**HiCom.** A write to `FFFF` selects a 32 KiB bank for `0000-7FFF`. The upper window at `8000-BFFF` stays on the first 16 KiB of the ROM. The old core mirrored the first half of the selected bank there. The fixed window agrees with [Bock's cartridge measurements](https://www.smspower.org/forums/15147-KoreanHiCom3In18In1Cartridges) and [Gearsystem's decoder](https://github.com/drhelius/Gearsystem/blob/master/src/KoreanFFFFHiComMemoryRule.cpp).

| The Best Game Collection | CRC32 by volume, in letter order | Menu routes checked |
| --- | --- | --- |
| 3-in-1 A-F | `98AF0236`, `6EBFE1C3`, `81A36A4F`, `8D2D695D`, `82C09B57`, `4088EEB4` | All three on each |
| 8-in-1 A-C | `FBA94148`, `8333C86E`, `00E9809F` | All eight on each |
| 4-in-1 A-F, standalone 128 KiB images | `761E35AE`, `002E1A49`, `6A63EEA6`, `F27E8F80`, `52587FB9`, `6F62F5A4` | Third entry on each |
| 3-in-1 G / 4-in-1 G | `752D198C` / `DEB45078` | Identified from published dump metadata; gameplay unverified |

On each of the 42 routes through the nine full dumps, the old and fixed cores selected the requested bank and produced identical video and PCM over 850 frames. No upper-window data read encountered differing ROM bytes on those routes. Standalone 4-in-1 A-F showed a separate, visible recognition bug. They are the six 128 KiB halves of the three 8-in-1 dumps. With the cursor on entry three, the old core left the lower window on bank 0 on all six. The fixed core selected bank 2 on all six. The [dumper's database entries](https://github.com/ocornut/meka/pull/108) identify the two G images. The 3-in-1 G entry applies to the standalone 128 KiB segment only.

**Hwasung.** On the 64 KiB 2 Hap in 1 cartridge (`1B3E032E`), even writes to `2000` select pages 0/1/2, odd writes select 0/3/0. Our selector and Gearsystem's arithmetic agree for every byte value. The old core powered up with pages 0/1/0. The [hardware report](https://www.smspower.org/forums/19383-Dumping2HapIn1David2MoaiUiBomulKRHwaseongHwasungMulticart) describes either game selection at startup. We initialize to the valid even selection, consistently across launches. David II and Moai-ui Bomul both reached gameplay on each build. Fourteen sampled frames per game matched through frame 1,500.

To exercise the actual Z80 bus and native state restoration on generated ROMs, run `python3 -B tests/sms_hicom_hwasung_tests.py build/linux/core/genesis_plus_gx_libretro.so`. The diagnostic also checks the shared 32 KiB mapping code on an 11 Hap Gam-Boy cartridge fixture.

## Korean 2000 XOR 1F and FFF3/FFFC

| Cartridge | CRC32 | Comparison on the original ROM |
| --- | --- | --- |
| 128 Hap | `BA5EC0E3` | Two menu selections, 1,200 and 1,800 frames. Only writes to `2000`. |
| Game Mo-eumjip 188 Hap v0 | `380D7400` | Menu to Bomberman gameplay, 1,200 frames. Five writes to `2000`. |
| Game Mo-eumjip 188 Hap v1 | `C76601E0` | Same route and five writes, verified separately. |
| Super Game 150 | `1B8956D1` | Five selections, 1,150 frames per route. Teddy Boy, Knightmare, Galaga, The Goonies and a fifth selection using mode `50`, mapper `03`. |
| Super Game 270 | `D9EF7D69` | Board identification from the [MEKA database](https://github.com/ocornut/meka/blob/master/meka/meka.nam). No game dump available for this check. The similarly named `D3056492` uses a different board. |

**2000 XOR 1F.** [The cartridge measurements](https://www.smspower.org/forums/17902-DumpingKorean128128In1CartridgeLikelyPirateMissingOriginalLabel) establish that any write in `2000-3FFF` or `A000-BFFF` sets the selector. Both fixed windows read logical page zero, and a frequent power-on layout puts pages `60-63` at `4000-BFFF`. The old core accepted only `2000`, exposed page one at `2000` and started all switchable slots on page zero. The fixed core uses the measured startup layout consistently. Unlike [Gearsystem](https://github.com/drhelius/Gearsystem/blob/master/src/Korean2000XOR1FMemoryRule.cpp), it returns zero for unpopulated pages, as reported by the dumper. All three ROMs wrote the selector on the first frame. Complete video and PCM matched the baseline on each tested route.

**FFF3/FFFC.** The original [Super Game 150 cartridge investigation](https://www.smspower.org/forums/19752-DumpingSuperGame150) describes the bank modes and tests the canonical addresses. The later [MEKA integration credited to the dumper](https://github.com/ocornut/meka/commit/cdc96303454484355f4ae9a9bbb345fddda811e9) and [Gearsystem](https://github.com/drhelius/Gearsystem/blob/master/src/KoreanFFF3FFFCMemoryRule.cpp) both ignore address bit A14 for these two registers. The fixed core accepts `BFF3/BFFC` accordingly. Canonical writes also put the original CPU byte in work RAM; the old core stored an intermediate bank index. Alias writes leave RAM untouched. The existing bank layouts agree with the published mode table.

Each of the five original Super Game 150 routes wrote twice to each canonical register, never to either alias, and had identical video and PCM on both builds. A separate controlled run redirected the four canonical writes to the aliases during The Goonies selection, without modifying the dump. The old core selected different banks and produced different output. The fixed core reproduced the original route's video and PCM. This distinguishes a demonstrable decoder defect from a failure seen in an unmodified game.

Run `python3 -B tests/sms_korean_alias_tests.py build/linux/core/genesis_plus_gx_libretro.so` for generated Z80 bus checks covering all eight mode values, both bank parities, both register pairs, RAM mirrors, nearby addresses and XOR power-on/missing pages.

## Collection survey

Of 657 loose SMS files surveyed by CRC, 18 matched the special Gearsystem entries checked (5 Codemasters, 4 Korean A000, 7 MSX including Nemesis, one Janggun and one 4PAK). The survey did not inventory archive contents.
