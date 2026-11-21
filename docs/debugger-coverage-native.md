# Physical code and data coverage

The native observer reports canonical physical byte spans during selected processor execution. Each loaded console enumerates stable allocation IDs, available CPU masks and sizes before capture. The host owns the bounded bitmap and export. The game-facing core state and save format remain unchanged.

**Code** marks bytes the interpreter actually consumed from the instruction stream, including immediate operands and prefixes. The main 68000's prefetch sidecar records each byte's origin when the real prefetch queue fills. Consumption can therefore credit the old bank after a mapper write. Attaching after an existing prefetch or restoring a state can leave the first queued word without an origin. Such bytes go into the unknown counter. Speculative and HALT refresh fetches do not promote code coverage.

**Data** marks observed CPU memory reads. Longword and multi-byte accesses can produce several physical spans if the bus uses different pages. Reads from an unsupported handler, open bus or MMIO report unknown bytes where observed. A held Sega CD DTACK transaction does not report its placeholder as a read. Video/audio DMA, Z80 port I/O, native writes and debugger or trace peeks are outside this capture.

| System | Observed physical sources | Limits |
|---|---|---|
| Mega Drive | Cartridge ROM banks, main RAM, Z80 RAM, known cartridge SRAM and active boot ROM | Genesis Z80 opcode fetch uses its direct ZRAM map. Its banked 68000 bus is observed for data. Dynamic device handlers without proven backing yield unknown spans. |
| SMS, Game Gear, SG‑1000 | Direct ROM, known mapped data paths, system RAM, and active BIOS/save/flash storage | EEPROM, protection/status reads and opaque mapper windows are unknown unless a backing byte is identified. |
| Firecore | Direct cartridge pages and wrapping copies resolved to the actual selected flash chip and byte offset | The currently copied wrap page retains its origin across logical aliases. |
| Sega CD and Mega‑LD | Boot ROM, PRG RAM, Word RAM, and backup RAM | The two 1M Word RAM banks alternate 16-bit words in the physical 2M numbering. Cell and dot reads follow the actual transform, including single-byte dot sources. Navigation to the differently ordered 1M memory view is disabled. |
| 32X | Both SH‑2 boot ROMs, both local data arrays, shared SDRAM and mapped ROM/framebuffers. Main 68000 uses its own mapped boot buffer and proven bank windows. | Covered SH‑2s use the interpreter while armed. The boot shadow buffer has separate coverage. Peripheral registers and unproven mapper-backed reads are unknown. |
| SVP | Actual consumed SSP1601 words in IRAM and its initialized program ROM copy. Main CPU can observe direct SVP DRAM. | SSP1601 data operations are outside this coverage implementation. |

PICO has no CPU target in the existing execution debugger. Link cable coverage selects one actual console at a time and reports that console's native allocation IDs. ROM bits are evidence of observed paths. Writable RAM bits identify locations across writes and can include earlier contents at those locations.

## Disabled observation

Direct 68000 reads share one processor-local gate for debugger read traps and coverage. The active path captures the backing address before the read and emits the observation afterward. It runs out of line, leaving ordinary direct reads free of coverage temporaries. Handler-backed reads can still observe a debugger change made while a nested device is paused. Coverage and debugger attachment update their own bits independently. Instruction coverage and the Z80 data observer use out-of-line paths as well.

## Focused verification

The original SMS fixture switches the same logical instruction and operand bytes between physical banks 1 and 3, then reads the same bank through a second window. The original 68000 SSF fixture switches the executing bank and distinguishes the old prefetched NOP from the different word at the same logical PC in the new bank. It also checks multiword operands, mirrored reads, main RAM and unknown VDP status.

The existing original expansion programs check the executed SSP opcode and immediate, both SH‑2s' actual SDRAM instructions and PC-relative loads, and sub-68000 PRG RAM code. The Sega CD program observes a physical Word RAM read through a mirror, switches to 1M mode and verifies bank interleave and dot-view byte offsets. These are synthetic fixtures with a local Sega CD BIOS. Real-game coverage and specialized opaque mappers were not used to establish those exact assertions.
