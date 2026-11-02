ROM audit observer (Linux/WSL, native libretro ELF)

Purpose
Run one exact local ROM/core pair per process and preserve a bounded JSON report
plus optional checkpoint PNGs. No ROM, user save, BIOS or settings are written.
This is an observation tool, not a compatibility verdict or a full playthrough.

Requirements
Python 3, NumPy, Pillow, GCC, binutils (nm), the native Linux core with local ELF
symbols retained, and source headers matching that binary's ABI. The observer
builds its small native adapter under ignored build/verification/rom-audit/.
It does not rebuild the emulator. Run only trusted local native core libraries.

Example, from the repository root
python3 tools/rom-audit/observe.py --rom /absolute/path/game.md --core build/linux/core/genesis_plus_gx_libretro.so --source build/deps/genesis-plus-gx-32x-linux --output build/verification/my-audit/game.json --frames 1200

Options
--no-hooks gives a video/audio/CPU control without intercepting memory handlers.
--no-input disables the default bounded Start/Right/A/B/C schedule.
--buttons FRAME:COUNT:MASK adds zero-based libretro button-mask intervals.
--option KEY=VALUE overrides one native core option; repeat as needed.
--auto-checksum applies the frontend eligibility and exported preservation query
only to the private in-memory copy; the file's original hash remains reported.
--bios-dir points to an existing read-only BIOS/accessory directory. The tool
never populates it. Accessory ROMs must use the actual appropriate core mode.
--no-png keeps only hashes/statistics. --checkpoints changes recorded frames.
Use --help for all options. Default runs stop after 1200 frames, not wall time.

Coverage and limits
Wrappers forward original values and refresh handler mappings around remaps.
Non-null 68K handlers and Z80 bank handlers are observed; direct ROM/RAM reads
are intentionally not intercepted. Normal IO totals are distinguished from
cartridge/TIME accesses. Trace rows, first-event history and noisy-handler
samples are capped; dropped-call counts are reported. Treat absence of a
retained row as inconclusive when a relevant limit was reached. A hardware
category names an address region; handler_behavior describes the actual route.

Provenance
core.sha256 identifies the exact executed ELF. Native symbol addresses come
from that ELF, not a guessed current source offset. source_md_cart_sha256 is
the inspected source-tree snapshot at run time; it is NOT proof that this
source compiled that ELF. Record a separately verified build receipt when
claiming compiled-source provenance. Matching ABI headers are required even
for --no-hooks because CPU/cart metadata still use native structures.

The 2026-11-02 collection audit used bounded hooks/no-hooks positive controls.
That validation does not replace matching the ABI and validating new adapters
when native structures or memory-handler behavior change. Never infer full
sound, persistence, ending or physical-board correctness from one screenshot.
