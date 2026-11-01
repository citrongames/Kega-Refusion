# Verification — version 0.2.1 — 2026-11-01

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
