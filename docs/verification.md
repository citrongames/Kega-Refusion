# Verification — version 0.3.0 — 2026-11-01

## Environment

- Linux: Ubuntu WSL, Qt 6.10.2, GCC 15.2, CMake/Ninja.
- Windows x64: native execution on the user's machine; cross-built in WSL with MinGW-w64 GCC 13.2 POSIX and Qt 6.10.2 Windows MinGW libraries.
- Core: Genesis Plus GX libretro, commit `f27655b592ef43f317272d79883bcaa10d3ac8ad`.

## Verified

- Linux CTest real-core integration suite: PASS.
- Windows native real-core integration executable: PASS.
- Both applications launched the user's existing Black Jewel Reborn DEMO 2.11 ROM read-only with isolated temporary settings; smoke runs exited 0 and produced visible game frames. Screenshots were inspected.
- Generated original ROM tests cover frame/audio production, state restore video replay, independent slots 0/9, invalid slots, missing/tiny ROMs, truncated/corrupt states, wrong ROM identity, reopening the core and restoring a saved state.
- SRAM fixture writes byte 0x42 to backup RAM. Tests cover full-capacity persistence, old short save padding, reopening, and preservation of oversized rejected saves through flush/shutdown.

## Defects caught and fixed

- Core reports shortened SRAM size after emulation begins: host now captures full capacity at load and accepts legacy trimmed saves.
- Windows atomic save replacement failed while the previous file remained open for comparison: close the read handle before QSaveFile commit.
- Windows startup failed with missing nanosleep64 because Qt bundled an older MinGW threading runtime: package uses the exact compiler's GCC/POSIX runtime DLLs.

## Limits

The smoke checks prove startup and frames, not full-game compatibility, every peripheral or audible playback on every device. Linux WSL reported a dummy audio output in the test environment. Normal core file-state loading resets sound resampler history; tests do not claim byte-identical PCM replay or cross-platform state compatibility. No copyrighted ROM or BIOS was added to the repository or package.

The requested compact UI has only the top menu and game image, with neither an Open/Pause/Reset toolbar nor a bottom status bar. Commands remain available from menus and hotkeys. Build outputs and QA evidence live only in ignored build directories; obsolete probes are removed.

## Controller / OSD release verification

- Final Linux build: CTest input_tests and core_integration both passed (2/2).
- Final Windows build: native input_tests.exe and core_integration_tests.exe both exited 0. Tests use isolated temporary settings and do not change user mappings.
- Virtual SDL controller tests cover discovery, two players, reconnects, identical-device keys, logical defaults versus explicit raw bindings, buttons/hats/axes, deadzone, neutral capture, triggers, command edges and rearming after focus disable, keyboard migration and persistence.
- Controls dialog preview checks covered all four diagrams, command mapping, and Save/Cancel across profiles.
- Native Windows windowed OSD, fullscreen OSD, and fullscreen keyboard smoke runs all exited 0. Actual Escape/Alt+Enter QKeyEvents checked show/hide/exit transitions, including zero reserved menu height after hiding. Screenshots inspected: pixel Cyrillic rendered correctly; fullscreen has no menu or bottom bar.
- Selected blue/violet KR artwork compiled into the executable icon and Qt resources. Package contains SDL2.dll and required license notices, retaining matching GCC/POSIX runtime DLLs.

No physical controller was manually exercised during these automated checks. Device-specific feel, unusual adapters and driver-specific behavior remain for testing on the user's hardware. OSD demonstration screenshots deliberately display sample statuses, not evidence of a real state-save action. Final logs and screenshots are retained in ignored build/verification; temporary settings and redundant preview builds were removed.

## 0.2.1 corrections — 2026-11-01

- Selected icon changed to the user-confirmed C: blue KR, two white stripes and outline. PNG hash matches the original generated image; ICO contains all seven sizes from 16 to 256 px.
- Linux final build: input_tests, osd_tests and core_integration all passed (3/3).
- Windows native final input, OSD and real-core executables all exited 0. The separate OSD test initially lacked the packaged plugins/platforms path; corrected test environment passed without production changes.
- Input regression covers Windows Set-1 and Linux XKB scans, Russian/English letters and punctuation, physical identity overriding layout text, extended keys, release across layout changes, old Cyrillic settings migration and reserved digit mappings.
- A real-window regression harness with an original generated ROM passed on Linux and native Windows. QKeyEvents delivered to the actual focused widget exercised RU/EN pause/mute/reset (once per press), repeats, gameplay release across layout/modifier changes, all ten slots, keypad digits, modifier suppression, Russian keyboard capture and reserved-digit rejection in the settings editor.
- OSD rendering tests cover three viewport sizes, upper-left-only placement, silent pause/fast-forward release, latest-action replacement, expiry, full disable and restoration of current status without stale notices.
- Packaged Windows application smoke runs (OSD on, OSD off and fullscreen Escape/Alt+Enter) exited 0. Screenshots visually inspected: no bottom or right notifications; disabling OSD hides every indicator. Test demo statuses are illustrative.
- General menu/FPS/device/status notifications no longer enter OSD. Slot selection and successful reset/save/load remain short action notices; pause/fast-forward persist only while active.

Latest evidence: build/verification/linux-0.2.1.log, windows-0.2.1.log, native-windows-results.txt and osd-0.2.1-*.png. The focused-window reproduction harness source is retained under ignored build/keyboard-ui; its redundant compiled copies and temporary app data were removed. Existing user saves and settings were preserved.

## 0.2.2 corrections — 2026-11-01

- Tab repeat-release events previously cleared held acceleration before the auto-repeat guard. They are now ignored; only real release or loss of focus/deactivation clears the hold. OSD expiry is independent of this state.
- Native Windows event traces reproduced the Russian right-Alt representation as Ctrl+Alt (0x0c000000). Fullscreen now accepts AltGr on Windows and GroupSwitch on XKB, as well as left Alt. Native Windows tests passed all 16 enter/exit transitions across EN/RU, left/right Alt and main/keypad Enter; the original test thread layout was restored.
- Expanded focused-window regression passed on Linux and Windows: Tab hold across repeat release/press pairs, real release, focus loss and application deactivation, plus fullscreen toggling without repeat. Harness source remains under ignored build/keyboard-ui.
- Loaded-game pillarbox/letterbox fields are black. Rendering tests verify both orientations and preservation of the game image.
- Incremental RC compilation was missing the ICO dependency, leaving the older icon inside the EXE despite an updated Qt resource. The dependency is now explicit. Final PE resource inspection verified that all seven embedded icon payloads exactly equal the selected KR ICO.
- Final Linux build: 3/3 CTest suites passed. Native Windows OSD/border and real-core suites exited 0; native UI key harness exited 0. Packaged executable smoke with the existing demo ROM exited 0 and its screenshot was inspected.

Current evidence: build/verification/linux-0.2.2.log, windows-0.2.2.log, native-keys-0.2.2.log, icon-0.2.2.txt and fullscreen-0.2.2.png. Temporary test settings and redundant harness binaries were removed; user data was untouched.

## 0.3.0 — archives, rewind and compact slot feedback — 2026-11-01

- Final Linux CTest: archive_tests, input_tests, osd_tests and core_integration all passed (4/4). The same four final Windows native executables exited 0.
- ZIP/7z fixtures cover listing, selecting multiple members, extraction, corruption/encryption and resource limits. Real Genesis Plus GX loaded the original generated ROM from both formats; raw and archived copies share their state identity. The core required GET_GAME_INFO_EXT for in-memory cartridge data; integration tests caught and verified that correction.
- Rewind tests verify reverse frame order, restored core/SRAM state, subsequent forward execution, discarded future history, duration/memory bounds and disabled history. The real-window harness checks held R in RU/EN, repeat events, changed modifiers, focus loss, application deactivation and virtual controller input.
- Speed selection at x2 and x8 updates the active OSD and survives settings reload. Rewind history settings and the OSD toggle also survive opening a new window.
- All ten slot selections and F5/F8 operations are exercised through the window. Rendering tests cover occupied/empty cells, selected outline, label, two-second expiry, three viewport sizes, replacement by action notices and full OSD disable. Occupied means a state file exists; its integrity is checked when loading.
- Two real Qt top-level windows verify that losing focus stops emulated frames, returning resumes only automatic pause, manual pause remains set, and disabling the option permits background execution.
- Final packaged Windows EXE loaded an original generated ROM directly from ZIP in fullscreen, exited 0 and saved slots-0.3.0.png. That screenshot was visually inspected: all notifications are upper-left, occupied slots are filled, empty slots dim, selected slot outlined, black background and no menu/bottom strip. Demo pause/acceleration/slot statuses are illustrative.
- Rewind defaults to 30 seconds (10/30/60 or disabled), with a 128 MiB compressed-history cap which may shorten the available history. Temporary working buffers are additional. Rewind audio is muted; byte-identical PCM replay is not claimed. Physical controller testing remains for the user's hardware.
- Archives are read in memory, without disk extraction. Limits and supported formats are documented in archive-support.md. Large solid archives can take time to inspect; decoding is synchronous and bounded.

Latest evidence retained in ignored build/verification: linux-0.3.0.log, windows-0.3.0.log, native-tests-0.3.0.log, keyboard-ui-0.3.0.log, native-ui-0.3.0.log and slots-0.3.0.png. Temporary smoke data/ROM and redundant standalone test builds were removed. Harness source and useful incremental build/dependency caches remain. Existing user settings, ROMs and saves were preserved during packaging.

## Empty-slot loading correction — 2026-11-01

- Missing state files are classified separately from corrupt/unreadable states. Loading an empty slot emits a two-second action OSD notice, without the modal error path, audio reset, history reset or pause changes. The existing global OSD toggle applies.
- Linux CTest passed 4/4; native Windows real-core integration passed. A missing load is rejected without changing serialized emulation state.
- Native Windows full window regression passed, including F8 on an empty slot with continuing video frames, no modal-error signal, no audio reset and the expected notice in GameView.
- Linux window regression passed the same empty-slot check, then failed its unrelated focus-regain check under WSL; the entire Linux window suite is not claimed as passed for this run. Native Windows focus-regain checks passed.
- Evidence: linux-empty-slot.log, windows-empty-slot.log, native-core-empty-slot.log, native-ui-empty-slot.log, ui-empty-slot-linux.log in ignored build/verification. Temporary harness binaries removed; source retained.

## Archive member list — 2026-11-01

- Multi-game archives now open a resizable dialog with a single-selection list, automatic scrollbars, Load/Cancel buttons and double-click loading. Archive member identity is stored separately from its display name. Single-ROM archives still launch directly.
- Linux build and CTest passed 4/4; Windows package rebuilt. Native Windows UI check used an original generated 40-ROM ZIP: last-row selection/scrolling, Cancel without loading, Load and double-click both selecting member 39 all passed. The dialog screenshot was visually inspected.
- Evidence: linux-archive-list.log, windows-archive-list.log, archive-list-native.log and archive-list.png in ignored build/verification. Temporary preview sources, binaries and generated ZIP were removed. User data retained.
