# Verification — version 0.6.0 — 2026-11-01

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

## 0.4.0 — rewind, pacing, regions, filters, resets and checksum — 2026-11-01

- Final Linux CTest passed10/10. Native Windows archive/input/OSD/shader/audio queue/audio output/rewind/pacing/core/session executables all exited0. Linux native GPU tests passed separately. No physical gamepad or audible listening test is claimed.
- Reverse history now stores compressed reverse patches plus a raw anchor. Deterministic tests exercise dense/sparse/unchanged data, size/geometry changes, branching, duration/byte bounds and disabled history. Real-core replay restores matching frames and SRAM; the original test ROM was strengthened to actually emit changing colors.
- Reproducible paired benchmark (same run/state/frames for both algorithms,65 emulated seconds, last60 retained, Linux WSL; scripted inputs, not a full-game playthrough): Black Jewel Reborn DEMO2.11 retained126.910→2.990MiB; Sonic2(World)102.263→18.118MiB; Comix Zone(Europe)90.795→11.045MiB. Average capture cost3831.85→359.10us,3250.34→763.58us,3455.97→615.00us respectively. Actual savings depend on scene/core. Sources are user-owned ROMs read-only; settings/saves used temporary directories. Benchmark source: tools/rewind-benchmark.cpp; results in rewind-benchmark-0.4.0.csv.
- Audio queue tests cover partial/blocked/error writes, frame alignment, bounded overflow, reset, concurrent producer and coalesced wakes. Simulated device tests span50/60Hz and32/44.1/48kHz; pacing tests include ±0.2% device-clock drift for ten simulated minutes. Session tests verify no backlog of GUI frame notifications and correct PAL/NTSC cadence even during frequent slot queries.
- Muted native Windows60s probe used the real default44.1kHz device, actual sink40ms. After2s warmup: zero underrun-idle/stopped/reset/error episodes; combined queue mean41.655ms,max55.804ms. Generic Windows sleeps gave frame p9931.635ms and251 intervals>25ms; high-resolution waitable timer reduced p99 to17.887ms and7 intervals>25ms. Worst interval remained34.165ms, so no universal jitter-free guarantee. Probe had no video painting; separate native GPU/UI tests validate rendering. JSON before/after retained.
- Core tests verify Japan/USA59.9227Hz vs Europe49.7015Hz, region register identity and PAL retention when loading an older NTSC state. All four real Blargg outputs differ appropriately, monochrome is grayscale and turning filtering off restores source pixels.
- Native GPU checks use actual upstream two-pass GDAPT, not a lookalike. Tests verify serial pass order, alpha tags, orientation, black borders, integer scaling, startup preset/parameters, failed shader retention, and exact unfiltered OSD glyph masks against raster. Native Windows device: NVIDIA RTX2060/OpenGL4.6; Linux: llvmpipe/Mesa OpenGL4.5. Fixed duplicate GPU child from show-event reentrancy and font atlas corruption from unpack alignment. Captures visually inspected.
- Soft reset preserves workRAM; hard reset reinitializes it while preserving SRAM and selected archive member, even if original ROM was moved/deleted. Ctrl+R/Ctrl+Shift+R and gamepad commands are distinct; old command IDs remain stable. Rejected oversized SRAM stays protected.
- Optional checksum autofix defaults OFF. A self-checking original68000 ROM fails its checksum with optionoff and succeeds with optionon in raw/SMD/swapped/ZIP forms. Original bytes/files/archive and save identity are unchanged. Ambiguous BIN remains untouched; disabling restores original ROM copy on restart.
- Native full-window harness passed RU/EN commands, all16 left/right Alt+main/keypad Enter transitions, empty-slot OSD without pause, region/Blargg/checksum menu persistence, hard reset Russian shortcut, genuine GDAPT menu and parameter dialog, restored preferences. Raster pixel comparisons and native GPU comparisons are separate.
- Packaged EXE smoke runs with GDAPT and isolated settings exited0, including fullscreen Escape/Alt+Enter; GDAPT stayed selected after transitions. The GPU screenshot path now reads the composited framebuffer rather than QWidget::grab's empty layer. Its upper-left OSD capture was visually inspected; demo statuses are illustrative. Subsequent testing was limited to this changed capture path, per the user's request to avoid redundant suites.

Current evidence retained only: linux-0.4.0.log, windows-0.4.0.log, native-tests-0.4.0.log, native-ui-0.4.0.log, gpu-linux.log, packaged-0.4.0.log, rewind-benchmark-0.4.0.csv, audio-native-before-timer-60s.json, audio-native-60s.json, fullscreen-0.4.0.png and selected native GPU captures. Superseded logs/captures, smoke settings, redundant harness binaries, probe sources/binaries and duplicate packaging stage were removed. Useful incremental build/dependency caches and the existing reusable keyboard harness source remain; user ROMs/settings/saves were not removed.


## 0.4.1 — Blargg geometry and fast-forward — 2026-11-01

- Focused real-core geometry checks passed on Linux and native Windows: original MD H40/H32, SMS and GG fixtures with all four Blargg modes, filter-off restoration and native geometry retained through rewind.
- Focused raster layout checks passed on both platforms: square-pixel/console/stretch modes, integer scaling, cropped 248/240-wide frames, interlaced height and GG geometry. GPU and raster use the same game rectangle; the prior GPU suite was not repeated for this layout-only change.
- Fast-forward skips unseen rendering via the core API and host PCM copies. All CPU/audio synthesis state still advances. Qt performs RGB565/555 conversion. Selectable limits now include x12/x16/x32 and uncapped.
- Per user preference, accelerated execution clears the old rewind timeline and does not serialize/compress snapshots. Recording restarts at normal speed; rewind cannot cross the omitted interval. A focused real-core session check passed on Linux and native Windows: uncapped execution, empty history during/after the gap, resumed recording and working rewind afterward. Existing unrelated suites were not rerun.
- Native Windows paired CPU-worker benchmark, Sonic 2 (World), 180 warm-up frames plus 1800 timed emulated frames, identical scripted input: with Blargg composite and the 30-second history setting, full-output/capture mode 497.1 fps (x8.30) versus optimized fast-forward 2974.7 fps (x49.64). Without Blargg, the same history setting measured 890.9 fps (x14.87) versus 3158.4 fps (x52.71). Accelerated history storage is zero; full-output retained 19.938 MiB / 7.489 MiB respectively. Both comparison modes use the new Qt pixel converter; full-output is not the exact previous application binary.
- The benchmark forces a rendered image every 60 emulated frames and compares it against full-output mode, checks muted PCM copying and audio/image production after resuming. All comparisons passed. These are scene-specific worker measurements without the GUI/GPU, not a guaranteed interactive speed or a full-game compatibility claim. ROM was read-only and temporary save directories were removed automatically. Source: tools/speed-benchmark.cpp; current results: build/verification/speed-windows-0.4.1.csv.
- Windows and Linux applications built successfully. Retained evidence includes the focused geometry/layout logs, fast-forward-0.4.1.log and the final Windows speed CSV. Superseded speed logs and duplicate package staging were removed; useful incremental builds and user data were preserved.


## 0.5.0 — Sega CD transport and BIOS — 2026-11-01

- Linux and native Windows focused real-core CD checks passed. Runtime-generated original 150-sector ISO/CUE/M3U fixtures cover three-region BIOS imports, missing-BIOS explanation, actual CD video/PCM callbacks, two-disc selection, disc-isolated quick states, same-disc rewind/state restore, eject/close, external replacement, hard reset, cartridge/CD transitions and 8 KiB BRAM persistence on unload. These fixtures validate BIOS/transport, not executable game boot.
- Final native Windows check additionally covers a Cyrillic image filename and a CHD that passes host signature preflight but is rejected by the core: the previous disc is remounted and continues rendering. CUE files with missing tracks are rejected before disturbing the current game.
- BIOS import helper was checked against the user's Fusion and RetroArch folders: regional identification, idempotent import, existing-file preservation and corrupted-image rejection. BIOS bytes are not committed. User-provided Fusion BIOS were copied into the portable Windows data/bios folder; source files were unchanged.
- Both applications built. No core binary rebuild/replacement and no unrelated regression suite reruns. Existing cartridge states retain their core identity. CD BRAM is outside this core's quick-state/rewind format and is persisted by GPX on unload; see architecture and README for this boundary.
- Evidence: build/verification/cd-linux-0.5.0.log and cd-native-0.5.0.log. Temporary generated image/data directories clean themselves up. End-to-end user-game validation is recorded separately below when available.


### User images and direct MP3

- Native Windows launched all six provided images: Sonic CD Europe/USA/Japan in CUE/BIN, The Terminator USA CHD, normal CUE/BIN, and ISO+MP3 CUE. Generated snapshots show actual game title/intro/level screens. Isolated data directories prevent changes to user saves. Game input, state save/load and same-disc rewind restore passed.
- The three Terminator representations reached the same level image at frame 3600. Native CHD and normal BIN produced identical aggregate PCM SHA-256 `2ec845d6d8b793cf80bb70f27ea44242399710f29a27d9a6c1b7ba907f5337f6`; lossy MP3 produced a different hash as expected. The MP3 path delivered 2,987,944 decoded PCM bytes during the 3600-frame check; it did not silently omit CD music. There were no VFS decode errors.
- MP3 seek test passed on Linux and native Windows: fixed 2352-byte sequential reads, eight small retained reference windows, repeated random seeks, odd byte offsets, WAV header boundary and EOF. Only 39,260 bytes of test buffers were retained, separately from decoder/index storage. A Linux single-track check decoded 155.48 seconds in 212 ms; this is a decoder-only measurement, not emulator FPS.
- Final native Windows paired speed benchmark uses 3600 warmup frames, then 12,000 timed frames with no Start presses (uninterrupted CD music), normal video rendering and PCM production, no window or rewind. AB then BA: BIN 1702.769/1695.132 fps versus MP3 1691.357/1690.332 fps. MP3 elapsed-time penalties +0.675% and +0.284%, mean +0.479%. Each timed MP3 run delivered 35,325,484 PCM bytes and matched the corresponding final BIN frame hash. Load/hash time is excluded.
- Normal-speed CPU/RAM comparison uses fresh processes per format, the same warmed game state and 600 paced frames (~10 seconds): BIN 59.886 fps, 10.293% of one logical CPU, 29.482 MiB average working set / 44.768 MiB private; MP3 59.848 fps, 7.481% of one logical CPU, 30.961 MiB working set / 46.363 MiB private. Memory increment is ~1.48 MiB resident / ~1.60 MiB private. The short CPU observation does not establish that MP3 is intrinsically cheaper: clock/power/scheduling/I/O effects matter. It showed no CPU increase in this sample; the paired uncapped measurement quantifies the small throughput cost. These figures describe the core worker, without GUI/audio device/rewind overhead, not total application consumption.
- Evidence: cd-speed-0.5.0.csv, cd-realtime-bin-0.5.0.csv, cd-realtime-mp3-0.5.0.csv and representative screenshots/logs in cd-games-0.5.0. Early timing attempts that toggled the game's own pause were rejected; the final benchmark source stops Start input before timing. Superseded/empty results and intermediate captures were removed.
- The native application also passed an MP3 startup smoke run. The startup plus now has a 152x124 clickable area around its 54x54 mark, a pointing cursor and the shared open-game dialog; it is inactive during play. Windows/Linux final builds passed. No unrelated broad suites were repeated.


## 0.5.1 — audio startup loop and startup click target — 2026-11-01

- Native Windows full-window diagnosis reproduced ~5 swaps/s with the core still producing ~60 frames/s. Cursor removal did not resolve the slowdown. Event timing isolated AudioOutput: an initial device start took ~376–412 ms, then repeated release/start cycles cost ~44–50 ms each.
- The bounded producer queue overflowed during slow device setup. Overflow scheduled another device reset and invalidated the in-progress pump, so the next pump reopened the device and overflowed again. Overflow now only drops stale whole PCM frames using AudioQueue's existing bounded append; explicit clear and format changes still reset. Partial samples remain aligned. Software buffering stays capped at 60 ms, with a 40 ms device high-water target.
- The same native full-window probe after the fix produced ~59–60 GUI deliveries and swaps per second after startup; no recurring audio reset stalls. Evidence: build/verification/fps-before-0.5.1.log, fps-trace-0.5.1.log and fps-fixed-0.5.1.log. The diagnostic CLI --performance-check reports produced, delivered and displayed frames over ten seconds without changing ordinary startup behavior.
- The startup plus target is now 304x248 logical pixels, twice the previous width and height. Removed all hover cursor changes and mouse tracking; clicking still opens the shared dialog only while no game is loaded.
- Windows and Linux builds and the two focused audio queue/output tests passed. The producer burst regression now requires exactly three stream invalidations (two format changes and explicit clear), despite 20,000 queued chunks. The deployed executable with sound enabled sustained ~59–60 swaps/s; fps-deployed-0.5.1.log. Built/deployed executable SHA-256 matched. No unrelated suites were rerun. Temporary probe data and duplicate package staging were removed; user settings/saves and incremental builds were preserved.


## 0.5.2 — deferred checksum policy and mute status — 2026-11-01

- Checksum toggling updates only the host policy. It no longer releases held input, reloads content, clears rewind/audio, resets pacing, or reports a completed fix. A subsequent game load or explicit hard reset rebuilds the in-memory cartridge using the new policy.
- Focused real-core session checks passed on Windows and Linux: toggling both directions while paused preserved byte-identical quick-state files, frame count and pause state, emitted no audio resets, and explicit hard reset still worked. Only the checksum-specific session mode was run.
- Mute is an active upper-left OSD status, including a persisted mute setting. Unmuting removes it without a resume message; disabling OSD hides it. Focused rendered-image checks passed on Windows and Linux. The initial Windows test launcher incorrectly requested an unavailable offscreen plugin; it was corrected to use the installed windows plugin.
- Windows and Linux applications built successfully. No unrelated regression suites were repeated.


## 0.6.0 — battery persistence, device selection, screenshots and resource audit — 2026-11-01

- Focused battery policy and real-core integration passed on Linux and native Windows. Original generated cartridge/disc fixtures verify blank SRAM suppression, two-second settling, 30-second minimum write interval and continuous-change deadline, immediate close flush, exactly one previous .bak, unchanged-data no rotation, and preservation of the primary when backup creation fails. Live SRAM changes and rewind restoration, live internal 8 KiB CD BRAM writes before unload, BRAM reload, and unload not re-rotating the backup passed. Temporary data used imported user BIOS; user games/saves were untouched.
- Compatibility test created a quick state using the previous unmodified platform binary, loaded it with the battery extension, and matched 60 subsequent frames and PCM bytes. Initial literal serialized-buffer comparison was rejected as the wrong criterion: 54 address bytes belonged exactly to the 24 YM2612 DT pointers and 30 connection pointers rebuilt on load. Emulation and state structure did not change.
- Audio queue and output tests passed on both platforms: partial writes/sample alignment, bounded overflow, coalesced wake, no overflow restart loop, missing-device preference retention/default fallback, and unchanged device notifications not resetting the stream. Queue append now reuses its existing storage. Physical hotplug/system-output changes were not automated; selection behavior was checked at the device/notification logic boundary.
- Screenshot-only raster and native GPU checks passed on both platforms: clean game crop, retained shader/aspect, OSD restored, PNG and JPEG encode/decode, NewOnly collision protection. Native Windows used the packaged JPEG plugin and NVIDIA RTX 2060 OpenGL; Linux used llvmpipe. The focused GPU resource check rejects eight 2048x2048 passes before allocation because their combined source/targets exceed 128 MiB.
- Rewind memory tests passed with actual retained QByteArray capacities, including oversized reserved anchors, highly compressible full patches, restore, sparse/dense changes and limits. Source audit confirmed no periodic writes during rewind and no accelerated history recording. A defensive reverse-anchor capacity check also keeps retained memory within budget.
- Forwarded archive metadata tests passed on both platforms; valid selection loads, stale name/size/index rejects without disturbing the current game. The normal GUI archive path now uses two full bounded decoder passes instead of three; extraction CRC/metadata/resource validation remains. Media audit confirmed bounded streaming, no decoded WAV/ISO copies and no growing VFS registry. Settings writes occur on explicit changes/load/close, not in the gameplay polling loop.
- Sequential gamepad capture regression was reproduced before the fix: Up on centered raw axis 3 was mistaken for a trigger endpoint, so Down could not arm until Skip. Linux and Windows virtual-device tests passed after using real mapped-trigger semantics and retaining verified neutral positions. Tests cover known/unknown pads, centered axes 1/3 and a held actual trigger followed by another binding. Capture UI now distinguishes waiting for release from ready-to-press. Physical controller behavior remains for user confirmation. Input polling uses fixed hysteresis storage and static target ranges instead of creating string keys/vectors repeatedly.
- Both applications built. Deployed Windows executable and core hashes match their build outputs. A ten-second actual-window run with sound and 30-second rewind enabled sustained approximately 59–60 GUI deliveries/swaps per second after startup. Evidence: build/verification/native-0.6.0.log and fps-deployed-0.6.0.log. No unrelated broad suites were rerun. Temporary probe data, old-core comparison copies and duplicate package staging were removed; useful build caches and user data remain.


## 0.6.1 — settings dialog — 2026-11-01

- Screenshot saving is a direct File action (F12). PNG/JPEG preference moved to a separate emulator settings dialog with OK/Cancel; unchanged or cancelled selections do not write settings. PNG remains the default.
- Linux and Windows applications built; reviewed the focused UI change and clean diff. No emulation changes or unrelated test reruns. Packaged Windows executable hash matches build output (39be925000d2e1bdb82e2f70270f8d371b4181594093a92edb4aa10bd38b86c1). Existing user data preserved; duplicate package staging removed.


## 0.11.0 — RetroAchievements — 2026-11-02

- Focused SDK-client checks used synthetic replies: constructor emits no HTTP, no anonymous requests, password/token login, game activation, unlocking/completion, context-bound progress, token persistence without the password, logout cleanup, same-game disc change and failed-media validation. No mock awards were sent to the service.
- Real-core generated fixtures checked MD/SMD/SMS/GG/SG hashing and raw memory maps, ordinary state progress wrapping and legacy state payloads, native resets, and ISO/CUE disc-header identification. SG-1000 extended-memory descriptors were corrected during this check. CHD uses the same native decoded-header accessor, but a real CHD was not executed in this task. 32X/CD32X memory interfaces were built; this is not a full per-game compatibility claim.
- Focused `session_tests --achievements` passed with a generated MD ROM and synthetic HTTP: no anonymous traffic, public Hardcore approval gate, account/game activation, real core state containing achievement progress, state restore, and disabling/resuming without requests or worker errors. No unrelated broad suites were repeated.
- One offscreen achievements dialog check covered table text, unlock columns, scrolling and synthetic badges. Source review verified notification preference only changes presentation, while achievement enablement is separate. Explicit login can apply enablement immediately. The global View OSD action shares the existing setting.
- User-authorized live verification through the actual Qt Network adapter and rcheevos logged in successfully and identified Sonic the Hedgehog as game 1 with 35 achievements / 0 unlocked. It ran neither emulation frames nor achievement frames, so no achievement awards were generated. Account/cache storage was temporary and removed; no account secret is retained in source or verification output.
- Linux/Windows applications and native cores built. Windows packaging includes Qt6Network and Schannel TLS, rcheevos MIT notice, and no qt.conf. Built/deployed EXE SHA-256: `48534d9cc53c1ba151b510933a5b79d59e6f69e3dc822c843dfe8ffb6926b4bb`; native DLL: `a752bda027c06aec79d531cfb1f03fbae00000ea938f2100c3ece43d1432c272`. No Windows execution or UI automation was performed; Windows interaction remains for the user.
- Hardcore remains unavailable pending official client approval; no RetroArch identity option is included. Anonymous local definition loading is not supported. Temporary live harness, duplicate research checkout and packaging staging were removed; incremental builds, concise verification evidence and user data remain.


## 0.11.1 — achievement presentation and persistent badges — 2026-11-02

- Focused offscreen table check passed: numeric points (5/10/100) and rarity sorting, no Progress column, correct proxy-to-source badge lookup, and unlock updates preserving row, selection and scroll. SDK extraction now orders rows by stable ID before presentation; header sorting is explicit rather than automatic on unlock.
- Focused popup checks covered the bounded queue, badge arrival without expiry extension, overlay-free screenshots, six screen anchors and drawing above existing action OSD without displacing it. A 60 Hz WSL raster observation around expiry measured maximum rendering before/near expiry at 1.716/1.510 ms, maximum timer gap 16.850 ms; the old text notice measured 3.216/1.487 ms and 16.732 ms. This did not reproduce a dismissal stall and does not establish Windows/OpenGL driver behavior. No Windows UI execution was performed.
- Deterministic PCM check passed for the original 250 ms synthesized chime, stereo saturation, resampling, bounded oversized chunks and clear/mute cancellation, without creating a sound sink. Existing audio_output_tests passed. Notifications use the live audio stream and never open another device.
- Isolated local-cache check passed after moving badge stat/read/decode/write to a private one-worker pool: complete-set prefetch, visible disk/RAM use, persistence across recreated network objects, no HTTP for stored images and no file rewrites. Badge IO plus network jobs are capped at two; valid images are retained without disk eviction. No live account/API calls or synthetic awards were sent for this UI task.
- Focused client test still passes after adding popup metadata and stable row order. Both applications build successfully; native core is unchanged. Packaged Windows EXE SHA-256 is `ab34086f7a9a7f6861429bf7269b063b2ba4f492f302659baf957322bda99ad4` and native DLL remains `a752bda027c06aec79d531cfb1f03fbae00000ea938f2100c3ece43d1432c272`; deployed files match their build outputs. Temporary test harnesses and duplicate packaging staging removed, user settings/saves/caches retained.


## 0.12.0 — settings, localization, diagnostics and optional hardware (2026-11-02)

- Linux and Windows application targets compiled successfully; the native core built for both platforms. Windows was packaged only, not launched or controlled. Destination EXE/core hashes match the built products.
- Focused real-core `expansion_metrics_tests` uses an original generated 68000 ROM and isolated copies of the user's BIOS. All four ordinary-cartridge topologies (MD, MD+CD, MD+32X, MD+CD+32X) execute the cartridge and preserve Mega Drive achievement identity. The test covers actual controller reads versus a no-read loop, skipped video, snapshots, delayed attachment settings, power cycling, incompatible quick-state rejection and restoration of recorded hardware/options.
- Focused `localization_tests` passed: embedded English with no external files, unknown selected language fallback, and partial third-party catalog fallback. Both delivered catalogs cover all 623 current translatable strings; placeholder and Cyrillic-in-English checks passed. The physical Russian keyboard scan-code table remains untranslated.
- Focused virtual SDL background-input check (`input_tests --background`) passed: disabled background input releases/blocks gameplay buttons; enabled background input accepts the pad independently of foreground keyboard/emulator commands. The existing full input suite has a pre-existing stale connection-count assertion (expects two signals, implementation emits three); unrelated tests were not expanded or rerun to chase that issue.
- Offscreen original-ROM UI checks passed for opening/closing a top menu, resuming its automatic pause, and preserving manual pause. Russian and English settings layouts were inspected. The retained `build/verification/settings-general-ru.png` reflects the final line icons and enlarged navigation rows.
- `osd_tests --fps-only` passed: both counters remain visible at the top right with action OSD disabled, turn off cleanly, and are excluded from game screenshots. This is a software/offscreen paint check, not a Windows GPU performance measurement.
- Focused disposable-cache check passed: only owned hash-named image files are removed, progress/unrelated files and external symlink targets remain intact, old queued requests are suppressed, repeated clears finish, and later explicit requests can reload images. No real account progress was reset and no achievements were submitted during these checks.
- The managed core-patch migration passed; a second unchanged invocation left source, patch receipts, sampled objects and core output mtimes unchanged.
- The deployed EXE contains every new ICO image (16, 24, 32, 48, 64, 128, 256 pixels). PNG alpha is present. Native Windows Explorer's icon cache/display was not manipulated.
- Deployed Windows EXE SHA-256: `1c7fac589193562ca1bcc83dc22edaac1d231ac9c4507c08588bc9c946d93ff1`.
- Deployed Windows core SHA-256: `14191adad7713e1685b5ec19595ce9ee472a7a13cb0f7a19542c36f949a1fb2c`.

Game FPS remains a normal-speed estimate from controller polling: games may read input even when their gameplay logic is stalled, or skip reads during menus/cutscenes. CD cartridge Mode 1 is limited by the native core to ROMs up to 8 MiB. These checks do not claim every commercial cartridge/peripheral combination is compatible. Task-specific launchers/cache probes and package staging were removed; source tests, useful incremental outputs, final screenshots and compact logs were retained under `build`.


### FPS presentation follow-up (2026-11-02)

The overlay now contains only one-decimal numbers: emulation `59.9`, game estimate `~29.9`; labels and percentage display were removed. Menu/settings text and both catalogs match. Linux and Windows application builds succeeded; the focused `osd_tests --fps-only` check passed. The refreshed testbuild EXE matches SHA-256 `af89e98b26c2f85f887a9bbe390ddd4e6c378d1d8e527348ce64b705f2ef227b`; the native core is unchanged. Windows was not launched. Temporary package staging was removed.


## 0.13.0 — direct netplay

- TLS-PSK loopback: IPv4 and IPv6, Ed25519 challenge, wrong key rejection,
  reconnect identity, host role assignment, session/persistent ban and unban,
  RTT and liveness. No external service was used.
- `net_file_tests`: bounded manifest/chunk parsing, hashes, interrupted transfer
  resume, no overwrite, CUE/M3U rewriting and outside-root/symlink rejection.
- `netplay_timeline_tests`: original generated 68000 ROM against the real core;
  frame-zero snapshot roundtrip, four delayed frames, corrected and final memory
  digests, merged input and spectator waiting. SRAM digest uses fixed snapshot
  allocation sizes, avoiding the core's first-frame reported-size transition.
- `netplay_session_tests`: two independent processes, real core, initial spectator
  role, host-assigned shared player role, OR input, remote pause/unpause, reset,
  quick save/load, rewind, game change within the same lobby, consented transfer
  and launch of another generated ROM. No copyrighted game data in fixtures.
- Windows is cross-built and packaged with Qt OpenSSL backend and pinned DLLs;
  the Windows application is not launched by automation. Internet paths and
  cross-platform game compatibility remain user-side validation.


## 0.13.1 — automatic invitation address

- `net_address_tests` validates public IPv4 and rejects malformed, private,
  loopback, shared/CGNAT, link-local, multicast and reserved addresses without
  network access.
- The explicit `--live` diagnostic completed from WSL and obtained a public IPv4
  through the HTTPS fallback. The address itself is not written to the log.
  Physical router UPnP and Windows runtime discovery remain user-side checks.
- Linux application and Windows 0.13.1 cross-build passed. Testbuild package
  was compared byte-for-byte; no automated Windows UI or gameplay was launched.
- Discovery is only started on lobby creation or explicit Retry. Cancel/leave
  invalidates old requests; fixed request/body/datagram limits prevent a slow
  or noisy discovery path from blocking the GUI or growing queues indefinitely.


## 0.20.1 — BIOS names, chosen save files, cheat editor and dialog cleanup

- Linux `bios_resolution_tests` passed: content-based discovery with arbitrary names/extensions, invalid canonical fallback, deterministic preference, metadata change/deletion handling, and no duplicate imports. Generated CD fixtures and isolated copies of locally provided 32X BIOS were used; no BIOS bytes are included in the repository.
- Linux real-core `bios_host_tests` passed: Sega CD and attached 32X load and power reset, unchanged recording fingerprints after rename, quick states saved before rename successfully loaded afterward, and no canonical BIOS copies created.
- Linux `core_integration_tests --state-files` passed: chosen `.krs` uses the normal envelope, restores video and working audio, classifies missing files and rejects a different ROM. Like ordinary slots, disk saves do not promise byte-exact immediate PCM because the core resets resampler history. The initial stricter PCM comparison was corrected to the existing state contract.
- Offscreen `cheat_ui_tests` passed: enabled-first stable display, disable all, clear all, preserved patch order and default-size controls. Existing search reply checks in that target also passed; unrelated suites were not rerun.
- Source review confirms explicit paths stay local and host-only during netplay, remote command parsing cannot introduce them, successful host loads use shared snapshot synchronization, recording saves retain the existing prefix/provenance format, and Hardcore still blocks loading.
- Linux and Windows application/core builds succeeded. About text/authorship and close-only dialog flags were checked in source; no Windows desktop control or interactive Windows testing was performed.


## 0.20.2 — controls layout, input capture and host continuation

- Linux `input_tests --capture` passed with virtual mapped and raw SDL devices: held unrelated buttons/hats/axes do not block a new button; sequential opposite stick directions and negative-rest triggers remain distinct; automatic bindings are recognized when replacing conflicts.
- Offscreen `debug_menu_tests --controls-layout` passed: dynamic port/Commands tabs, keyboard/gamepad latest-assignment priority, double-click capture, visible green footer, ordinary layout without outer scrolling, and memory-only selection restoration across Cancel/reopen. Port 2 and waiting screenshots were visually inspected. The English Down translation found during inspection was corrected.
- Real-core `netplay_session_tests --moderation` passed with two processes and an original generated ROM: kick/ban/rejected rejoin finalization, host progress after leaving a manually paused session, scoped offline saves, failed replacement preserving the game, and subsequent game returning to solo save storage. The new check was corrected to wait for subsequent frames instead of requiring them when the load error signal first arrives; its original-game assertion is only checked before the next valid load.
- Linux and Windows application builds succeeded. No new emulation-frame polling or disk activity was added for the controls UI; captured-device state uses reusable buffers. No Windows desktop automation or interactive Windows gameplay checks were performed.
- Investigated the reported Contra: Hard Corps red screen by inspecting the supplied USA/Korea ROM and CHT: `NNCT-AAH4` changes initial lives at 0x5FA from 3 to 99; the startup checksum comparison at 0x384 fails without the accompanying Master Code. `EABT-AA56` skips that check. No emulator workaround or delayed cheat activation was introduced.
- Published Windows EXE SHA-256: `04a0a5415cc91488b6ffbbc01f594a4fea512fcc5383cc494dbf60ef8f3bc25a`. Published EXE, both core modules and language catalogs match build sources; the same five Linux components were compared inside the archive.


## 0.20.3 — network Softcore, complete media transfer and temporary files

- `achievements_host_tests` passed against the real core: bounded immutable snapshot parity for MD/SMD/SMS/GG/SG/CD, null gaps and invalid ranges, allocation reuse, and read-only client SRAM poll/flush/unload preserving an existing save/backup. Normal host writes still persist with the previous backup.
- `netplay_achievement_rollback_tests` passed: two process-isolated real cores with delayed GGPO packets, incorrect predicted RAM observed, matching corrected confirmed-frame streams. No server or real account was contacted.
- `netplay_achievement_tests` passed with original ROM and mocked HTTP: spectator suppression, requested Hardcore downgraded, exactly one confirmed Softcore award, and continued gameplay with no further awards/errors after logout. Fixture metadata and raw Genesis memory addresses were corrected before the passing run.
- `net_file_tests` passed: complete multitrack CUE+MP3 reception, M3U dependencies, Paprium soundtrack inclusion without unrelated MP3, full-file integrity/resume checks, owned/stale cleanup, retained active-owner files and user source ROMs, and idempotent legacy save migration without overwrites.
- `netplay_session_tests --moderation` passed: kick/ban/rejoin and host continuation, ordinary quick-state storage after leaving, failed replacement preserving the loaded game. The fixture report writer now commits atomically so its directory watcher cannot read a partially written report.
- Linux and Windows application builds passed. Invitation fields use password echo mode in both lobby and join dialog; copy buttons still read original invitation data. No Windows desktop automation was used.
- Legacy testbuild cleanup was performed only after confirming no KegaRefusion process was running: 38 old session directories and 3 old download directories were removed; one saved state was copied into ordinary storage and SHA-256 verified before deletion.
- Published Windows EXE SHA-256: `9c6f79d06c3bcfe3b882ec87b3025ecd62ccaef066ce949ad3db4a4bab71eb56`. Application, both core modules and language files were matched against build sources on both platforms.


## 0.21.0 — native video capture/export — 2026-11-03

- `video_recording_tests` passed on Linux: PNG decode matches every RGB byte, AAC has actual generated sound, Unicode filenames work, and existing outputs are preserved. A real-core original ROM records90 PAL input frames; exported pixels across native resolution parts exactly match unfiltered playback despite Blargg being enabled. Regional cadence, background export, live paused stepping with audio, final pause and cancellation were checked.
- `video_ui_tests` passed using offscreen Qt and an original ROM: separate input/video menus, valid action availability, no spurious protected-hardware notice at recording start, real forbidden changes still warn, and clearing the snapshot restores local controls.
- Existing focused `core_integration_tests --video-geometry` and `core32x_video_integration_tests` passed with new raw-capture assertions, including native pixels before Blargg and 32X combined output. User BIOS files were read into isolated test directories, never committed.
- Direct read-only probing of the existing Windows FFmpeg DLL confirmed PNG/AAC encoder availability. Windows and Linux application/core builds passed. Windows interactive validation remains with the user; no desktop control was used.
- An initial fixture comparison covered only the first MOV part. The fixture changes192→224 raster height immediately after power-on; comparison was corrected to concatenate all parts, then exact pixels passed. No core pixel workaround was introduced.
- MOV track timestamps use60000 ticks/second to avoid QuickTime's high-timescale warning while retaining exact regional timing. AAC priming is compensated; a decoder may expose up to one terminal AAC block of padding. Editor-specific import is not claimed tested; codec support is documented for DaVinci Resolve.

- Final archive hash verification caught a stale Linux launch-tree core copy. The packager now takes both core modules from the canonical core build directory; the launch-tree copies were refreshed before publishing.


## Release package size audit and Super Eagle example — 2026-11-03

Both application builds now select CMake Release (`-O3 -DNDEBUG`). Windows already used Release; Linux previously used optimized RelWithDebInfo (`-O2 -g -DNDEBUG`). Core behavior and recording compatibility IDs are unchanged.

Measured sizes exclude user data and the other platform's archive:

| Package | Before (MiB) | Final (MiB) |
|---|---:|---:|
| Windows runtime + packaged documents | 129.47 | 100.72 |
| Linux extracted distribution | 341.73 | 293.49 |
| Linux gzip archive | 138.81 | 120.23 |

- Largest avoidable Windows file: `libstdc++-6.dll` contained debug/symbol data, reduced from25.13 to2.09 MiB. Package stripping also reduces EXE, cores, OpenSSL and compiler runtimes. Only staged copies are modified; original development binaries remain. Signed PE binaries are preserved, and strip preserves dates to avoid rewriting PE timestamp/checksum on every package.
- All allocated code/data/resource/import/export sections of all27 Windows binaries matched before/after stripping. Final deployed files match staging. Linux packaged app/core allocated sections match the current Release originals; an isolated offscreen startup/frame smoke using an original68000 loop ROM exited0. No Windows desktop control or broad repeated gameplay suites were used.
- The original Linux archive contained246 files and206 dependency libraries. Closure analysis found no stray or missing libraries relative to the selected app/plugins. Removing individual linked codec libraries would break loading.
- Candidate follow-up: Qt FFmpeg multimedia plugin and its exclusive Quick/QML/swscale dependencies occupy about16.76 MiB expanded. Qt6.10.2 fallback integration supports our QAudioSink/QMediaDevices use, but removal needs output-device validation on both platforms. The plugin remains in this build.
- Main further opportunity: a purpose-built FFmpeg with PNG/AAC encoders and MOV muxing. About116.69 MiB of existing Linux libraries are exclusive to the full FFmpeg graph, including libraries that need replacement. This is NOT a promised saving; a minimal replacement must be built and measured. VideoRecorder directly requires FFmpeg even without the Qt plugin.
- ICU data (about31.58 MiB) is a real QtCore dependency. Qt UI/network/audio/graphics, SDL controllers and OpenSSL/TLS remain required. Both core modules stay physically separate for Link Cable; hardlink deduplication could merge the loaded modules. Embedded controller PNGs total roughly12.6 MiB; optimizing these is a separate resource-quality task.
- User-supplied Super Eagle GLSL/preset files are included unchanged in external `shaders/super-eagle`, with attribution and GPL text. First custom-shader selection defaults to the packaged shaders folder. A focused `shader_tests --external-preset` run compiled both passes and rendered successfully through llvmpipe OpenGL4.5. Archive/deployment checks confirmed all example files. Original user-supplied testbuild/super-eagle files were preserved.


## 0.21.1 — minimal FFmpeg runtime — 2026-11-03

Both platforms now use checksum-pinned FFmpeg 7.1.2 with PNG/AAC encoders and MOV muxing, including upstream MOV packet/parser dependencies. No decoding, demuxing, protocols, network, devices or filters. NASM and runtime CPU dispatch remain enabled; baseline x86-64, LGPL-2.1-or-later. Three runtime libraries total about 2.1 MiB per platform. Linux shared objects have verified literal `$ORIGIN` RUNPATH.

| Package | Before (MiB) | After (MiB) |
|---|---:|---:|
| Windows runtime/documents | 100.72 | 81.41 |
| Linux extracted | 293.49 | 162.09 |
| Linux gzip archive | 120.23 | 67.96 |

- Removed Qt multimedia playback plugins and their exclusive runtime dependencies. Isolated silent native audio probes on current Windows and WSL Linux passed before/after: same device inventory/default output, 44.1/48 kHz PCM consumed, volume/suspend/resume/reset/reopen functional, no media plugin loaded in fallback. This is audio-output validation, not a Windows 10 compatibility test.
- Linux `video_recording_tests` passed once against minimal FFmpeg: exact RGB round trip, AAC sound, PAL input-recording export, raw pixels despite Blargg, live stepping and cancellation. Windows `--encode-only` generated 65 RGB frames plus AAC; external FFmpeg decoding confirmed every pixel and a non-silent audio track. No desktop or interactive Windows gameplay automation.
- Both Release application builds passed. Packaged Windows files match staging byte-for-byte, with no leftover root DLLs. Linux app and both cores retain identical allocated sections after package stripping; isolated offscreen archive startup exited 0. Archive excludes Qt multimedia plugins, full FFmpeg and QtQuick. Packaging dependency parsing now handles spaces in source paths.
- Compatibility answer derives from Qt 6.10 requirements and renderer code: Windows 10 1809+ x64/Windows 11, desktop OpenGL 2.1 (WGL on Windows, Qt X11/Wayland on Linux). No specific GPU minimum or Windows 10 runtime test is claimed. Linux package is built against glibc 2.43.


## Settings master volume — 2026-11-03

Added a 0–100% master-volume slider to Settings / Sound with a numeric percentage. It mirrors the existing menu slider bidirectionally, reusing immediate output/OSD updates and the existing 400 ms coalesced, changed-only preference save. Connections belong to the dialog widgets and are removed when it closes. Existing Russian/English Volume label is reused. Windows and Linux Release builds passed; Windows deployment matches staging. This small UI-only change was checked by code review/build, without repeating audio or core suites.


## Live audio latency profiles — 2026-11-03

- Normal preserves the original target/prefill/queue/device policy exactly and is default. Low and Stable adjust the software cap, prefill, device write high-water mark and clock feedback target together. GUI selection applies immediately; only the audio sink/PCM queue reset, with existing coalesced wake and generation guards. No core/timing/recording contract changes.
- `audio_queue_tests` passed: all three profiles at 32/44.1/48 kHz and 50/60 Hz, sustained fractional PCM, stalls, bounded overflow, partial-write alignment, rate/profile switches and unchanged-profile no-op. `audio_output_tests` passed: correct new clock targets, one invalidation per actual change and one queued wake for a burst of changes.
- `video_ui_tests --audio-profiles` passed with the real core and an original 68000 fixture, offscreen Qt and native audio fallback. Each profile selected through settings immediately changed audio, persisted through dialog rejection/reopening, and resumed frames/native audio with the new target without reloading the ROM. Initial test was held by focus-loss pausing of the invisible window; that preference is disabled only for this fixture. No application workaround was introduced.
- Linux and Windows Release builds passed. No broad core/gameplay suites or Windows desktop automation were used.


## Public release preparation, v0.21.1 — 2026-11-03

- Native and MinGW builds use Release; integrated Linux/Windows cores were rebuilt/checked through the maintained core builder. Clean Windows ZIP and Linux tar.gz packaging excludes user data, games, BIOS and credentials. Windows ZIP CRC checks passed; the extracted Linux archive completed an isolated offscreen startup with exit 0.
- Scanned all reachable Git objects before publication: 1,380 blobs / 64,742,319 bytes; no matches for the checked private-key/token/account-password patterns, credential file paths or Genesis ROM headers. This records the checks performed, not an absolute guarantee that every possible secret can be detected.
- Added GPL-3.0-or-later for original project code with a specific core linking exception and explicit third-party scope. The existing GDAPT licensing uncertainty remains documented, not relabeled as GPL. About dialog links to the licensing overview.
- Both distributions now include the same integrated-core/Paprium/component notices; Linux packaging previously omitted several of these notices. Source distribution accompanies binary release artifacts.


## Large SSF cartridges / Neverhood 32X — 2026-11-04

- The supplied `neverhood32x_ssf.32x` is 128,974,848 bytes (123 MiB), SHA-256 `c01df54a32ce8468f3891d79dc3b7d16f8f1ce144b615ea7726405507171bc97`. Its header checksum and computed checksum both equal `9AB7`. The author identifies this expanded SSF version separately from the small original demo: https://old-pirate.itch.io/the-neverhood-32x.
- Unified frontend/raw/archive member limit is now 128 MiB; archive total stays 128 MiB, outer-file limit 512 MiB, maximum 4096 entries. Core ROM capacity matches. Small cartridges retain their existing allocation/read behavior; checksum calculation no longer detaches a complete shared QByteArray merely to read it. No per-frame ROM copies or added mapper polling.
- Root cause after lifting the size limit: GPX selected the Mega EverDrive extended MD layout for `SEGA SSF`. This 32X program writes `2` to A130F1 to disable/write-protect SRAM. Treating that as bank-zero selection made SH2 code fetch graphics data. Explicit 32X cartridges now use fixed bank zero, F1 SRAM control, and eight-bit bank registers F3–FF. Ordinary MD extended SSF retains its remappable bank zero. Behavioral reference: [PicoDrive cartridge mapping](https://github.com/notaz/picodrive/blob/26ecb2b6358fefba24e3d68b9eb2efba7f10d5ee/pico/carthw/carthw.c).
- Large SSF states use the existing 160-byte cartridge trailer for full 64 KiB page indices. SRAM switching also retains the hidden ROM page's high byte. State size and other games' state layouts remain unchanged. Only affected SSF content receives a new recording/network compatibility identifier; recording version differences remain advisory.
- Focused original 128 MiB fixture passed: all 64 mapped pages, upper-bank data/code fetch, MD-versus-32X F1 behavior, SRAM read/write/protection, high-bank restoration after SRAM switching and snapshots, soft reset/power reload, and cartridge-type isolation. ZIP/7z boundary and localization tests passed. No full test-suite repeat.
- Actual game reached its Options screen. Its state/rewind roundtrip reproduced subsequent video and PCM; reset and power returned to the introductory sequence. This is startup/input/state evidence, not a full-game completion claim.
- An isolated input probe moved the in-game cursor with a six-button pad. Mega Mouse movement and click did not change the matched menu frame. The ROM contains a generic mouse driver, but the tested game path does not consume it; mouse gameplay support is not claimed.
- Linux and Windows Release builds completed. Windows EXE, both physically separate core DLLs and both language catalogs in `testbuild` matched package SHA-256 values. User BIOS/ROM/settings/saves were preserved; transient diagnostics were removed. No ROM or BIOS is committed.

## Portable Linux build (2026-11-11)

Built the Linux application, updater, both core modules and pinned dependencies from a fresh GitHub checkout with an empty dependency cache, using Debian 12 x86-64, Qt 6.4.2, glibc 2.36, GCC 12, CMake 3.25.1 and Python 3.11.2. The package passed the glibc 2.36 symbol check. Verified all 168 manifest entries after extraction. The packaged launcher reported version 0.22.0 through both the X11 and Wayland platform plugins on WSLg. Mesa EGL/GLX and the OpenSSL and xdg-shell plugins resolved their symbols with the bundled libraries. Steam Deck requirements were checked against Valve’s package metadata only.

## RetroArch GLSL presets (2026-11-11)

On Mesa llvmpipe, 595 of 608 presets in the supplied GLSL collection loaded and drew one frame with a 32×32 source and 64×64 viewport. The preceding renderer drew 201. Pixel checks separately cover LUT orientation, float precision, sRGB conversion, mipmaps, prior passes, history and feedback.

The 13 remaining failures come from three individual presets and two shared sources. `simple_dither.glslp` specifies `shaders = 1_`, `MMJ_Cel_Shader_3dfx.glslp` references missing passes, and `gb-dot-matrix-inverted.glslp` references a missing palette. Eight border presets use an invalid vector assignment in `box-center.glsl`. Two xSal presets use fragment types defined only in the vertex stage.

At 320×224 input and 1280×720 output, median software-rendering times across three trials were 3.661 → 3.696 ms/frame for bilinear, 11.572 → 12.058 for crt-geom and 2.381 → 2.486 for aa-shader-4.0. CRT Royale, NTSC S-Video and HQ4x also rendered multiple frames at that size. These measurements cover the software OpenGL driver.

## Release 0.24.0

The published Linux 0.23.2 updater displayed the new release notes, downloaded 0.24.0 from GitHub and restarted into the new version. All 169 installed package files matched the manifest. A portable setting and a battery save survived the update. The Debian 12 package requires at most glibc 2.36.

The public Windows ZIP matched the updater SHA-256. All 124 manifest entries matched and the packaged executable reported 0.24.0 on Windows. Both production targets built. The separate Windows video UI test target still lacks the Qt private platform header in its include path.

## Release 0.25.0

An isolated installation of the published Linux 0.24.0 build discovered 0.25.0 on startup, downloaded it from GitHub, and restarted. All 169 installed files matched the new manifest. The screenshot format setting and a battery save survived the update. The portable Linux archive retains the glibc 2.36 baseline.

On Windows, the 0.24.0 helper applied the new package to a temporary installation and launched the 0.25.0 executable. Its version check exited successfully, all 124 managed files matched the manifest, and ten user-data markers survived. The public Windows download and update metadata matched the verified release artifacts.

## Release 0.26.0

On Debian 12, the published 0.25.0 application discovered 0.26.0, displayed the grouped release notes, downloaded the Linux archive from GitHub and restarted into the packaged 0.26.0 executable. All 172 installed manifest entries matched. Twelve user-file markers and two settings survived. The portable Linux package retains the glibc 2.36 baseline.

On Windows, the 0.25.0 helper installed the release ZIP in an isolated copy and started the new executable. The installed application reported 0.26.0, all 127 manifest entries matched and ten user-data markers survived.

## Nuked YM2612 output (2026-11-14)

Compared the Linux core against the preceding 0.26.0 core on Sonic the Hedgehog 2 and Doom 32X. PCM, rendered pixels, Nuked state and native save states matched across cold boot, soft and hard reset, state-file replay and the return from fast-forward. Both cores loaded each other's saves and reproduced the recorded frames. The FM channel mask and gain test passed.

On the same WSL CPU with 600 warm-up frames and 2,400 timed frames, drawing one frame in 32, Sonic 2 ran at 658–664 fps before and 701–704 fps after the change. Doom 32X ran at 331–335 fps before and 307–345 fps after. The 32X measurements did not establish a consistent speedup.

## Nuked FM operators (2026-11-14)

Sampled the unmodified Linux release core at 6e8d07a during fast-forward. OPN2_Clock accounted for 37.8% of samples in Doom 32X and 76.8% in Sonic 2. In Doom, operator waveform generation, modulation routing and channel accumulation accounted for 17%, 10% and 9% of chip time respectively. Generated SH-2 code alone accounted for 14.4% of total time. Inline locations were mapped using a debug object with identical machine code.

On one pinned WSL CPU, two runs per version (600 warm-up and 2,000 timed frames, rendering one in 32) gave 342 to 356–357 fps in Doom and 705–709 to 752–759 fps in Sonic 2. A comparison over two million clocks in four modes found identical stereo outputs and chip state. Both games produced identical audio, video and native save states across normal play, fast-forward, resets and cross-version save/load.

## 32X SH-2 cartridge reads (2026-11-15)

On Doom (Japan, USA), samples of the 79e8977 Linux release core during fast-forward put the outer scheduler at 7.1%, SH-2 peripheral timers at 4.0%, generated SH-2 memory helpers at 6.0% and translated guest blocks at 7.5% of total CPU time. ROM callbacks and purity checks accounted for another 3.7%. The build now reads plain cartridge pages directly; handlers and longwords crossing a page still use the cartridge bus. A separate timer experiment was slower and was discarded.

On one WSL CPU, with 600 warm-up frames and the same 2,400 measured frames and input at 1-in-32 rendering, Doom ran at 525–526 to 571–573 fps with MAME and 353–358 to 362–371 fps with Nuked. The DRC component test checks access width, alignment, page boundaries, mirrors, replacing a bank and reverting to a handler.

Doom matched the baseline frame, PCM and native state comparisons through restore, fast-forward and reset. In the short Night Trap (USA, Disc 1) CD32X run, video, PCM and cross-version replay matched. Four bytes in early Sega CD graphics save fields varied across independent processes on both the baseline and the optimized core, including the early hard-reset checkpoint.

Two other cartridges were compared for 1,200 frames with input. Darxide (Europe, En/Fr/De/Es) ran at 49.70146 Hz PAL, from the Sega intro to a moving cockpit scene with HUD. Sangokushi IV (Japan) ran at 59.92274 Hz NTSC, from its animated intro to the Koei title screen. Each matched all captured frames and PCM, sampled states and the final raw state. These scenes cover startup and presentation; player control was not separately confirmed.

## Sega CD graphics state before the first stamp operation (2026-11-15)

Night Trap (USA, Disc 1, CD32X) on 0906c0f produced different raw snapshots in two Linux processes after startup and hard reset. The four differing bytes were the upper halves of two 32-bit Word RAM offsets. The original serializer subtracted the host RAM base from null trace-vector and stamp-map pointers. The fixed writer encodes zero for unset pointers; the reader normalizes old offsets only before the first graphics command. The two fields keep their original widths.

Two independent corrected processes matched all PCM, video and full native state hashes through 600 cold-start and 120 post-reset frames. Old native and .krs saves from frames 0, 8 and 120 replayed for 180 frames. An input recording made on the old build played identically in two corrected processes, including resumes from frames 8 and 120. The generated-disc regression also fails on the old core and passes on the fix.

A controlled comparison using the real network snapshot API found identical emulated RAM digests before and after normalizing the unused pointers. On the old build the serialized snapshot SHA changed; on the fixed build the entire snapshot matched. GGPO does not compare that SHA between peers to detect gameplay divergence. This was a snapshot API comparison, not a two-machine session.

Sonic CD (Europe, PAL) also matched with initialized graphics and when restoring an active stamp operation across core versions. In the cold native-restore then file-restore sequence, the first presented image differs from fresh power-on on both builds. PCM and native state match throughout, and subsequent images match.

## Firecore release and update from 0.26.0 to 0.27.0 (2026-11-15)

Both x86-64 Release packages were built from tag `v0.27.0` at `2de1dea`. The public GitHub latest redirect selected this tag. The English notes in the published update metadata matched the release body. Both public binary downloads matched the archive sizes and SHA-256 digests advertised to the updater. The Linux package retained the Debian 12 / glibc 2.36 baseline.

On Linux, the released 0.26.0 application discovered the update, downloaded it and restarted into 0.27.0 on an isolated display. All 172 installed manifest entries matched. Twelve user-file markers and both settings files survived. The helper left no staging directory or error.

On native Windows, the packaged 0.26.0 helper applied the staged 0.27.0 ZIP contents in an isolated installation. The installed executable reported 0.27.0, all 127 manifest entries matched, and four user files covering battery saves, quick saves, settings and BIOS survived.

## Game Gear M2 Collection cartridge profile (2026-11-15)

The 1 MiB Aleste Collection release of GG Aleste 3 (CRC32 `5AE64731`) produced one black image and silent PCM over 720 frames with the previous core. Its Sega bank registers worked. The M2 profile supplies a sparse CPU-visible ROM view for this release. Its 113 byte differences occupy eight 1 KiB pages. This models the observed software requirements; the internal implementation of M2's emulator is undocumented here. The reference is the [RetroAchievements playability patch](https://retroachievements.org/forums/topic/24600) with the [later PSG channel 3 correction](https://www.dreamcast-talk.com/forum/viewtopic.php?t=16716&start=10), retaining the nine original FE bytes described there.

With Start on frames 480–494 and 650–664, then fire from frame 750, the original reached Wave 1. All 1200 visible frames and stereo PCM blocks matched the corrected reference (CRC32 `2D746EA4`), including 300 first-wave frames. The 100 frames following a state restore also matched between the two images in video, PCM and RAM. Within either image separately, restored pixels differed from uninterrupted playback in the raw-core probe; PCM and RAM matched. The same video discrepancy occurred on the ordinary Sega-mapped reference.

Selecting bank 3 via FFFD kept the first KiB fixed and switched the rest of its window. Both upper windows mapped the expected instructions after writes to FFFE and FFFF. Restoring a state restored the bank map, and the underlying cartridge bytes retained CRC32 `5AE64731`. Both installed Windows core DLLs matched the packaged files.

## GG Aleste 3 restore and BOoM four-player play (2026-11-15)

In Wave 1 of GG Aleste 3, replaying the SMS VDP registers during a restore marked 512 patterns dirty without keeping their queue. VRAM survived, but the renderer had no patterns to draw. Loading a pre-fix frame-1099 KRS now shows the correct paused preview. The next 100 images and PCM blocks match uninterrupted play. Rebuilding the seventh frame from the rewind checkpoint also matches. The SMS/GG rewind fixture now draws an actual tile so it can detect this failure.

BOoM 1.1 (44,920 bytes, CRC32 `143AB50B`) needs a linear ROM window. The old core mapped CPU address $8000 to ROM offset $A000. Mirroring from the start of the upper window restores the title and arena. With Master Tap on port 1, two Right presses and button 1 select 4P MODE. From the same arena checkpoint, 12 frames of movement from each frontend slot changed only that player's sprite area: top left, top right, bottom left and bottom right respectively. The game also reads four independent input bytes at $DFA4–$DFA7.


## Release 0.28.0 — 2026-11-20

Published stable tag `v0.28.0` at `0dbd88b105e952ff90541fa423b6e6a883c82afc`, with English Added/Improved/Fixed/Changed notes. Windows and Linux application/core binaries were built in Release from `2468f907e6d09a51233bc1bea971adad7de6080d`; the final commit changes only Linux packaging. The final Linux package differs from the first candidate only in `run.sh` and its ownership manifest. Windows is unchanged. The corresponding-source archive records both build and packaging provenance.

- Linux: Debian 12, GCC 12, Qt 6.4.2; package symbol audit passed the glibc 2.36 ceiling. Packaged application reports 0.28.0. Windows: MinGW/Qt 6.10.2 Release; packaged EXE contains the new application version. No Windows executable or UI was launched during this release check.
- Prepublication testing found that the old Linux launcher invoked host `env`/`dirname` with inherited portable libraries. On Ubuntu 26.04, `/usr/bin/env` required `LIBSYSTEMD_254` unavailable in the bundled Debian library, so replacement completed but restart failed. The launcher now uses `/bin/sh` and shell builtins before directly executing the application. The actual unmodified 0.27.1 helper successfully restarts the fixed package under the same inherited environment.
- The 0.27.1 archive engine extracted, preflighted and applied both actual release archives in isolated directories: all 69 Windows and 172 Linux manifest entries matched; 12 sentinel user files per platform remained byte-identical. Windows installation logic was exercised under Linux, not as a native Windows process.
- After publication, the real UpdateService/UpdateInstaller code from tag 0.27.1 discovered 0.28.0 through the anonymous latest-release redirect and public update.json, displayed the exact English notes, downloaded and verified all 58,066,689 Linux archive bytes, and handed off to the stock old helper. Restart as 0.28.0 was observed, all 172 installed manifest entries and 12 user sentinels passed, and temporary update staging was removed. No test processes were left running.
- The Windows archive was separately streamed from the public release URL and matched its 38,198,178-byte size and SHA-256. Public update.json and SHA256SUMS.txt matched the authored local files. All five published assets had matching GitHub SHA-256 digests. The release is stable, non-draft and latest.
- Corresponding sources: 580 exact committed project files, 781 prepared-core files shared by both platforms, 87 managed patches and 30 dependency source artifacts; all 1,726 listed file checksums verified after archive creation. No fresh rebuild from the completed source archive is claimed.

Final archive SHA-256:

| Asset | SHA-256 |
| --- | --- |
| Windows x86-64 | `f0a2ab0404724cbd238fd04510b0758715d31cbef812b2cc239bd9f01e6f28c3` |
| Linux x86-64 | `87b38dd16ef2edb9153e45729ca2d43c2a239be05ed02b2df454b60193f0851a` |
| Corresponding source | `88d58da024f8d06ff65a7a37cabfcd9846690a61786601082c5a96f16e3a2fbc` |

Evidence and final assets are retained under ignored `build/release-0.28.0/`, including updater-check-evidence.zip, source-verification.json, draft-verification.json and public-verification.json. The actual hardware/game checks preceding this packaging-only step are recorded in [the platform audit](platform-accuracy.md) and [accuracy investigation history](mega-drive-accuracy.md).

## 0.29.0 debugger release — 2026-11-21

Released tag v0.29.0 at 5300ac1 with matching Windows, Debian 12 Linux and corresponding-source archives. The focused release audit passed 18 registered checks and eight standalone checks, covering debugger UI, CPU execution, linked consoles, physical coverage, tracing, project/watch application, Firecore snapshots and cheat integration. Two confirmed UI regressions were fixed: switching between archive members with the same display name no longer retains the previous debug project, and hiding Memory no longer resets the linked-console selection of other inspectors.

Windows was cross-built in Release mode; the embedded 0.29.0 version and all 72 package manifest entries were verified without running a Windows executable. Linux was built with Qt 6.4.2 in Debian 12.15; all 175 manifest entries and the glibc requirements of 123 ELF files were checked (maximum 2.36). Both isolated core modules include the debugger exports. This targeted batch is not a full-game or physical-device compatibility claim; prior debugger performance measurements and limitations remain in their feature verification records.

The exact v0.28.0 updater archive code successfully installed both packages in isolated directories, preserving twelve user-data sentinels per platform. After publication, the real 0.28.0 UpdateService discovered the public v0.29.0 release, displayed its English notes, downloaded and verified the Linux package, and used the shipped old helper to install and restart 0.29.0. All 175 installed manifest entries and twelve user-data sentinels matched. The public Windows download was independently streamed and checksum-verified. GitHub latest, update.json, all five assets and their SHA-256 metadata matched the release.

The corresponding-source archive contains the exact 674-file project snapshot, 801 prepared core source files, 94 matching patch receipts and thirty dependency archives. All 1,919 internal checksums were verified, with source/runtime provenance matched to both binaries. No ROMs, BIOS images, credentials or user settings were included.

Evidence and recipes are retained under ignored build/release-0.29.0, including audit-verification.json, linux-archive-verification.json, windows-version-verification.json, source-verification.json, draft-verification.json, public-verification.json, updater-verification.json and updater-check-evidence.zip. Temporary package copies, isolated upgrade installations and updater probe binaries/copied sources were removed; incremental build and dependency caches remain.


## 0.30.0 CD32X RAM-cartridge release — 2026-11-22

Released `v0.30.0` at `fb1a123ec9ef7be78bc7ea7260089c428276f7a7` with Windows,
Debian 12 Linux and corresponding-source packages. The English notes describe
the optional 4/8/16 MiB cartridge, Road Rash Rapido setup and the SH-2 translated
code-cache correction. Nuked audio was not changed or reverted.

Windows was cross-built in Release with Qt 6.10.2. Its embedded application
version, both core copies and all 72 managed package files were verified without
executing a Windows program. The Debian 12 package reports 0.30.0, passed the
maximum glibc 2.36 check and contains 175 verified managed files. The generated
RAM-cartridge fixture passed on its final core: all capacities, page mapping,
save restoration/rejection, SH-2 code coherence and the scoped sub-CPU loader
profile. Earlier real-game and preceding-core comparisons remain documented in
[the cartridge verification](cd32x-ram-cartridge.md#verification--2026-11-22).

The unmodified 0.29.0 archive engine installed both final packages into isolated
directories using their respective platform rules; twelve user-data sentinels
per platform remained byte-identical. After publication, the actual 0.29.0
UpdateService discovered 0.30.0 through the public latest redirect and metadata,
displayed the exact authored notes, downloaded and verified the Linux archive,
and handed off to the shipped old helper. Restart into the packaged 0.30.0
application, all 175 managed files and all twelve sentinels were verified. No
update staging or helper process remained. This was a native Linux end-to-end
check; Windows installation logic was exercised on Linux, not as a Windows UI.

All five GitHub asset digests match the final local files. Public `update.json`,
`SHA256SUMS.txt`, the tag commit and the Windows archive download also match.
The source archive verifies 1,929 file checksums, including exact tracked source,
integrated-core sources, patch receipts, dependency archives and runtime notices.
Evidence and final artifacts are retained under ignored `build/release-0.30.0/`;
transient package trees and updater installations are removed.


## 0.30.1 audio-output release — 2026-11-22

Released `v0.30.1` at `2e93731a3fa41ea0e2987c5cf241d0b9dbe724e0` with matching
Windows, Debian 12 Linux and corresponding-source archives. This release adds
the optional Low-pass filter and moves audio-device servicing off the GUI thread.
Core binaries are unchanged from 0.30.0; feature measurements and limitations
remain in [the audio-worker record](audio-worker.md).

Windows was cross-built in Release; the embedded version and all 72 managed
files were verified without launching a Windows executable. Linux reports
0.30.1, uses Qt 6.4.2 and passes the glibc 2.36 ceiling. All 175 managed files and
121 unchanged core/runtime/plugin hashes were checked. The two focused Linux
audio-output and core-integration checks passed.

The unmodified 0.30.0 archive engine installed both packages in isolated
locations, preserving twelve user-data sentinels per platform. After publication,
the actual 0.30.0 UpdateService discovered public 0.30.1, displayed the exact
English notes, downloaded and verified the Linux archive, and used its shipped
helper to install and restart the packaged application. All 175 installed files
and twelve sentinels matched; update staging was removed. Windows installation
logic was exercised on Linux, not as a Windows UI.

Public latest, tagged update.json, all five GitHub asset digests and the release
tag commit match. The public Windows download was independently streamed and
verified. Corresponding source contains the exact 690-file project snapshot,
801 prepared core files, 95 patch receipts and thirty dependency artifacts;
all 1,935 internal checksums passed.

Final archive SHA-256:

| Asset | SHA-256 |
| --- | --- |
| Windows x86-64 | `e6a7b88c015c11ee98e65dacb6cfeed5cb1537551efde0e60aef2721962b5b33` |
| Linux x86-64 | `f0934f7e5518ef20f92dc7be234b210a455bf5e261c588f9c9516d208bccad97` |
| Corresponding source | `d40be717e7a69c4a2ef430e96a965e076a36818528565472554f651c1db3aa19` |

Final artifacts, packaging recipes and compact verification evidence are retained
under ignored `build/release-0.30.1/`. Transient package trees and isolated updater
installations are removed; useful incremental build and dependency caches remain.


### Pause priority over automatic rewind — 2026-11-25

Explicit pause now suspends automatic rewind as it already suspends fast-forward.
Single and held manual frame steps remain available with both requests enabled.
At the oldest history entry, pausing/unpausing keeps that entry rather than
restarting forward execution. Network snapshot metadata reports paused rewind
as inactive, and paused peers can continue servicing frame-step synchronization.

Focused real-core `session_tests --frame-back` and `--fast-forward` passed on
Linux with generated ROMs: exact frame/state comparisons for steps in both
directions, three held repeats each way while both modes are requested, pause
of active rewind, resume, and the oldest-history boundary. Windows Release
was packaged to testbuild and its stripped EXE matched the build; Windows UI
and a two-peer network scenario were not run for this change.

### Mutually exclusive fast-forward and rewind — 2026-11-27

The latest activation from the menu, keyboard or gamepad cancels the opposite
mode, including its menu latch and held-input sources. A cancelled hold cannot
revive on release or block a fresh alternate keyboard binding. Pause priority
and manual frame-step behavior are unchanged; no core or state format changed.

The focused Linux `debug_menu_tests --controls-system` check passed with a real
core and generated SMS ROM: menu switching in both directions, keyboard/gamepad
takeover of opposite menu latches, cross-device held commands reaching the core,
and fresh alternate binding while a cancelled key remains down. Existing menu
open/close, held-input takeover and dual-binding checks passed in the same run.
Windows Release was packaged to testbuild and the deployed EXE matched the
stripped build. Windows GUI testing is left to the user; no Windows application
was launched. Evidence: `build/verification/exclusive-speed-modes/` (ignored).

### Automatic IPS/BPS patch chains — 2026-11-27

Implemented cartridge soft-patching on the worker before hardware/region/hash
selection, with the portable Patches folder, local-file precedence, numbered
chains and per-step BPS preference. Source files remain read-only. Patch failures
leave the running game intact. Patched save/cheat names use a final-ROM hash
suffix. Netplay forwards the loaded bytes and suppresses repeat patching on
received media; existing external soundtrack discovery uses those final bytes.

Focused checks passed:

- Format/discovery tests: IPS records, RLE and final-size extension; all BPS
  commands including overlapping target copies; source/result/patch CRCs;
  malformed/overflow/bounds cases; mixed numbered chains, duplicates, gaps,
  folder precedence and failure rollback. The parser/discovery suite also ran
  under AddressSanitizer and UndefinedBehaviorSanitizer.
- Real-core generated MD fixture: default-off behavior; patched PAL selection
  from an NTSC source; separate SRAM and quick slots; rewind frame/audio equality;
  input recording/replay frame equality; unchanged bytes across power reset and
  changed settings; original-game rejection of patched recording; archive member
  patches; bypass for downloaded content. The initial region assertion exposed
  invalid FF padding in the generated fixture; the fixture now uses a valid
  space-padded region field. No region-emulation change was needed.
- Net file transfer: downloaded bytes equal the effective patched ROM, equivalent
  local content has the same manifest, external tracks follow patched hardware
  detection, and archive overrides retain the selected cartridge extension.
- Existing battery/folder policy and localization checks passed.
- User files: Altered Beast (USA, Europe) IPS, Contra - Hard Corps (USA, Korea)
  IPS, and Phantasy Star IV (USA) BPS each loaded at 59.923 Hz, executed 180 frames
  and passed a save/load roundtrip. Each original file remained identical.
  This is startup coverage, not full playthroughs.

Linux and Windows application builds passed with no new warnings. Windows was
packaged to testbuild; the deployed stripped EXE and en/ru catalogs matched.
Windows GUI and a full two-peer lobby run were not performed for this change.
No emulation-core binaries or serialized-state formats changed. Evidence and
final-ROM hashes are under ignored `build/verification/rom-patches/`.

### MMPX Advanced shader — 2026-11-27

Added MMPX Advanced v3.5 as a separate built-in shader. The existing dither
shader and emulation core are unchanged. The new pass works at 2x source size,
then uses normal presentation scaling/aspect; the GPU requirements remain
OpenGL 2.1 / GLSL 1.20. Both package scripts include the MIT attribution.

The port matched the pinned upstream implementation on all 6,419,920 RGBA8
output pixels across 20 fixtures, including border taps, angled bands and all
65,536 binary 4x4 tiles. The comparison also passed with OpenGL 2.1 / GLSL 1.20
forced. Focused pipeline checks passed for 160x144, 256x192, 320x224, 320x240 and
320x448 inputs, resize reuse, orientation, one-pixel text, flat colors, letterbox
bounds, and switching MMPX -> existing dither -> stock. Existing GPU shader
tests passed on the hardware renderer. The initial high-contrast triangle
fixture incorrectly assumed mixing was unconditional; the corrected moderate-
contrast fixture checks blending within the upstream algorithm's color gate.

Captured 30 consecutive frames from the user's Lion King and Contra slot-0
saves. Only temporary copies of the validated Windows save envelopes had their
platform identity changed for the compatible Linux core; original ROMs and
saves were untouched. Visual review used native-source and filtered captures.
The initial benchmark filled its viewport and produced stretched screenshots;
final captures and timings preserve the aspect supplied by the core.

Renderer: NVIDIA GeForce RTX 2060 via WSLg/Mesa D3D12. Input 320x224; output
1280x720 with a 940x720 active viewport. Median of three 300-frame batches after
120 warmup frames per filter, rotating order, synchronized with glFinish.
Includes frame upload and presentation rendering, excludes emulation, readback,
PNG saving and swap/vsync. Longer warmup removed large initial timing variance.

| Scene | Stock | Existing dither | MMPX Advanced |
| --- | ---: | ---: | ---: |
| Lion King gameplay | 0.123 ms | 0.340 ms | 0.448 ms |
| Contra gameplay | 0.118 ms | 0.356 ms | 0.619 ms |

These are renderer costs on this GPU, not whole-emulator FPS or Windows-native
benchmarks. Windows Release built and was packaged to testbuild; the deployed
EXE matched the stripped build and the packaged MIT notice matched its source.
No Windows GUI session was launched. Evidence is under
`build/verification/mmpx-advanced/` and `build/mmpx-validation/` (ignored).
Temporary save copies, parity output blobs and standalone scratch executables
were removed; logs, source harness and comparison screenshots remain.

### Scoped Road Rash CD32X loader compatibility — 2026-11-27

Restored compatibility only for the supplied original Road Rash Rapido disc.
The native mounted-disc probe gates on the generic loader header, checks
logical sector 36 CRC32 `ea789aae`, then checks the complete 78 KiB APP payload
(LBAs 26–64, CRC32 `7e749148`; observed SHA-256
`39323739fc00d0de9ae5ceb3882d31acb7e96757f88f266f6ba51463aea141b3`).
This is an exact old executable profile, not filename detection or a universal
32X-loader heuristic. The probe is cached per mounted disc, uses one 2 KiB
scratch buffer, and never hashes the complete image or runs per frame.

The matched disc enables 32X in Auto, but Disconnected still wins. Program RAM
remains manual. Only the sub-CPU long data reads at instruction starts D612 and
D61A may align down, with the exact original instruction sequence and address
register relationship verified and effective address within Program RAM. Writes,
instruction fetches, other PCs/opcodes, modified code and unrelated discs retain
normal Address Error. Native recording/network IDs identify the scoped profile;
ordinary game IDs and the serialized state layout are unchanged.

The real-core generated fixture passed all RAM capacities, bank/state/JIT checks
and strict odd-access controls. With the original CUE read in place and an
isolated generated BIOS, Auto/16 MiB permits exactly the two legacy reads; RAM
disabled or 32X Disconnected traps them. Wrong PC/IR, altered surrounding code,
out-of-range address, writes and the identical instructions on another disc
trap. Disc replacement revokes the exception, reinsertion restores it. A
generated same-name CUE does not trigger Auto. Recording/network profile IDs
are present only in the active legacy-disc/RAM scenario.

Using the retail US BIOS and existing menu checkpoint, the actual old game
reaches The City and runs for 3,000 frames. Final video and complete PCM match
the historical broad-workaround run exactly. Native state and rewind restore
the same future video and PCM. This covers the race-start scene, not a complete
game playthrough. Short 600-frame before/after comparisons on Doom (Japan, USA),
59.922743 Hz, and Sonic CD (Europe), 49.701459 Hz, retain identical final video
and complete PCM. Evidence: `build/verification/roadrash-scoped/` (ignored).

### Release 0.31.0 — 2026-11-27

Published stable tag `v0.31.0` at `16c695132ad1d623b2d7fb8d36741dc38d79c8d0`.
Windows and Debian 12 Linux Release packages report 0.31.0. Linux retains the
glibc 2.36 ceiling; its packaged runtime inventory matches the corresponding
source bundle. Both prepared native core trees match reconstruction from the
pinned upstream revision and all 95 managed patches. The source archive
includes 704 project files, 801 prepared core files and 30 dependency artifacts.

Both final archives passed manifest/hash checks and isolated 0.30.1 -> 0.31.0
installation using the unchanged 0.30.1 updater archive code. All twelve user-
data sentinels remained byte-identical for each platform. The Linux package
reported the old/new version before/after replacement. Debian 12 also passed
its native updater checks and the focused MMPX GPU suite under software GL.
Windows GUI and native Windows helper restart were left to the user.

All five assets, their GitHub digests, authored English notes and update.json
were verified before publication. The public latest-release redirect points to
v0.31.0; public metadata and both binary downloads match their final hashes.
An initial WSL upload timed out; the same unchanged archives uploaded through
Windows HTTP/1.1 and were validated before the draft was published.

Evidence and final archives are retained under `build/release-0.31.0/` (ignored).
Temporary package trees, isolated installations and scratch probe executables
were removed. Useful incremental builds remain.


## Lua scripting foundation (2026-11-27)

- Embedded upstream Lua 5.5.1 archive SHA-256:
  1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce.
  All 63 upstream files preserved byte-for-byte. Static C++ build uses protected
  exception unwinding through Qt bindings; notices include the pin and MIT
  license. No baseline OS/Qt version change.
- lua_runtime_tests passed original fixtures for MD, SMS, SG-1000, GG, GG Link
  A/B, 32X, Firecore, Sega CD and CD+32X. Checked native-endian and signed reads,
  bounded ranges, logical peeks/registers, unsupported reads, separate environments,
  error/loop/OOM/native-work limits, drawing, log bounds and inactive Stop ACK.
  Native state bytes stayed identical; debugger, tracing and coverage stayed off.
- lua_session_tests passed frame/overlay identity, paused redraw, forward/back
  step, file restore, runaway isolation, Stop clearing the HUD, queued source
  invalidation including a cold network transition, local netplay observation
  and cleanup on unload.
- lua_window_tests passed nonmodal/no-minimize/no-maximize behavior, async
  source/PNG preparation, machine-token race protection, deferred-start Stop,
  queue rejection recovery, bounded logs, editor/error navigation, retained
  unsaved edits and external-save conflict checks.
- Scoped osd_tests/shader_tests with --script-overlay-only passed raster/GL,
  active viewport/crop, aspect/stretch, 100% and 150% DPR, alpha, OSD off and
  clean capture. Full shader suite passed at 100%. Two unrelated existing
  failures were reproduced in unmodified binaries: live MOV status after a
  transient message expires, and built-in OSD glyph raster/GL comparison at
  150%. Neither is counted as a Lua pass.
- Both Linux and Windows application targets compiled. Windows interactive
  GUI testing is left to the user; no desktop input was captured or injected.

### Lua CPU cost

Three rotated trials of 1,800 frames each, restored to the same checkpoint
after 360 startup frames. Native execution + Lua observation/list creation,
without GUI rendering. The HUD performs three successful RAM reads and emits
text/shapes. Accelerated cadence renders 1/32 frames and omits PCM output;
Lua frame events still run each executed frame.

| Game / actual region | Cadence | Baseline µs/frame | Empty runtime | RAM HUD |
| --- | --- | ---: | ---: | ---: |
| The Lion King / NTSC-U 59.92274 Hz | Full video/audio | 570.21 | 586.02 | 581.65 |
| The Lion King / NTSC-U | 1/32 video, muted | 381.29 | 383.99 | 399.09 |
| Sonic the Hedgehog 2 / PAL 49.70146 Hz | Full video/audio | 635.81 | 650.26 | 661.78 |
| Sonic the Hedgehog 2 / PAL | 1/32 video, muted | 431.99 | 423.95 | 442.04 |

All compared final native-state and video hashes matched; PCM hashes matched
normal-output trials. Different cadences are not compared as PCM.
HUD overhead: 2.0–4.7%; empty-runtime variation: -1.9–+2.8%, so smaller differences
are not separable from run noise. Core SHA-256:
3d14d4022cfc670e45a23b9a2de4634f29b57100a67fa9d57bbd31dff614e187.
Evidence: build/verification/lua-phase1/.


The bundled Sonic 1 REV00 example was also run on the real user ROM in place:
NTSC-U 59.92274 Hz, 2,400 unattended frames, first complete HUD at frame 1,168,
1,233 frames with complete moving bounds. Position/velocity/radii and
camera/mode addresses were checked against the SonicRetro disassembly and
the original Hivebrain RAM documentation; it displays object collision radii,
not inferred universal hitboxes. Sonic 2 was rejected by the revision guard.

### Lua follow-up: attached hardware and Russian UI

The user's original Sonic the Hedgehog (USA, Europe), 512 KiB REV00
(checksum 264A), reproduced the example's rejection when Sega CD was attached.
CD Program RAM and cartridge ROM both advertise base zero and 512 KiB; the
example selected the last matching domain. It now uses the stable inspection
domain IDs for cartridge ROM and main RAM, retaining its revision guard.
The same ROM starts the example with automatic hardware, 32X, Sega CD, and
CD+32X; Sonic 2 is still rejected in all four configurations. A 2,400-frame
CD+32X run at 59.92274 Hz produced 1,233 complete HUD overlays with moving
collision rectangles. No native emulation changes were needed.

Added the missing Russian Lua-window catalog entries and moved the command
to File → Scripts. The existing offscreen window checks now initialize the
shipped English and Russian catalogs and verify translated controls; they pass.
The Windows testbuild executable, example and language files match the staged
package byte for byte. Windows GUI testing remains with the user.

### Lua Phase 2 — input automation and temporary states (2026-11-27)

The real-core Lua runtime checks cover independent input overrides, restart
precedence, before-frame/after-frame ordering, yielded main-chunk continuation,
mouse axes and two Justifiers, local policy rejection, temporary object reuse
and snapshot accounting including queued loads. Existing read-only platform
fixtures still pass. Session checks use an original MD controller-polling ROM:
scripted Right reaches the hardware port and work RAM; a paused coroutine steps
without unpausing; save/load restores the native frame clock and releases overrides;
errors and external reset cancel waiting automation. Script-requested soft/power
reset, pause/resume and rejection of a reset during recording also pass. Recording a branch, restoring
an earlier script checkpoint and replacing the branch produces a two-frame native
recording. Both frames replay with matching controls and polled RAM without the
automation script. The shipped live input-activity graph runs on the real session.

Movie integration tests cover repeated temporary restores, older/newer recorded
branches, read-only playback validation, fork identity, damaged payload failure
atomicity, and clock restoration. Existing slots, rewind, deterministic replay,
resume and battery guards pass. The existing Lua session and English/Russian
window checks also pass. Windows was cross-compiled; no interactive Windows app
was launched for verification. Native core sources and binaries were not changed.

Performance uses the same native core, real ROMs read in place, 360 warm-up frames
and 1,800 measured frames per trial, three rotated trials. All workloads include
the canonical MovieSession input application; active scripts also run the new
before-frame callback and post-commit observation. Automation reads/reapplies
neutral pad fields, exercising the new input path without changing the game.
Each workload retains identical final native state, image and normal-speed PCM
digests. Times are microseconds per emulated frame, medians; GUI rendering is
excluded. These compare workloads in the same build, not two release binaries.

| Game/region | Output | No Lua baseline | Empty runtime | HUD | Input automation |
| --- | --- | ---: | ---: | ---: | ---: |
| Lion King / NTSC-U 59.92274 Hz | Full video/audio | 545.51 | 546.30 | 573.51 | 589.99 |
| Lion King / NTSC-U | 1/32 video, muted | 368.51 | 373.40 | 374.97 | 377.27 |
| Sonic 2 / PAL 49.70146 Hz | Full video/audio | 622.36 | 630.43 | 628.51 | 635.72 |
| Sonic 2 / PAL | 1/32 video, muted | 420.73 | 410.24 | 415.96 | 424.91 |

No-script variation spans -2.49% to +1.33%; automation adds 2.15–8.15% in
full output and 0.99–2.37% when presenting one frame in 32. Evidence is retained
under ignored build/verification/lua-phase2.

### Lua Phase 3 — debugger integration (2026-11-27)

The native CPU event ABI and native emulation were unchanged. The host now
unions debugger and Lua masks into one connection; trace and coverage keep
their independent connections. `debug_execution` passes selected execute/read/
write events, exact event values and ordering, overlapping/duplicate ranges,
no debugger attachment for observation, unchanged machine bytes, blocked writes
from callbacks/partial frames, observer removal with a suspended debugger fiber,
soft/power reset and unload. Linked fixtures verify peer ranges, writes and
teardown. `debug_expansion`, `inspection_core`, `debug_trace_host_tests` and
`debug_coverage_host_tests` passed against the current native core.

Runtime checks passed actual MD access callbacks, endian writes, register edits,
unsupported/out-of-range rejection, callback mutation rejection, quota statistics
and a single truncation notice, stop/error isolation, stale-save revision
rejection, stable annotation handles and independent Game Gear Link A/B access.
The packaged fixture-debugger.lua runs on the generated original cartridge,
including its queued write, label/watch and access-count HUD. Existing read-only
fixtures for MD, SMS, SG-1000, GG, GG Link, 32X, Firecore, CD and CD+32X still pass;
this is not a new full mapper/MegaLD media audit.

Session integration passed deferred memory/register edits without opening the
debugger, rejection of an already queued pre-edit temporary load, annotation
publication and clearing, native callbacks through the normal frame scheduler,
and write rejection during input recording. Existing automation, recording branch,
playback, frame-aligned packets, pause/step/rewind/load and lifecycle checks pass.
While a native frame is suspended, Stop removes an empty observer immediately;
nonempty subscription changes wait for a complete boundary. A before-frame
script failure also removes its hooks before the upcoming native frame.

Three offscreen UI checks (`debug_watch_view`, `cpu_debugger_view_tests`,
`lua_window_tests`) passed. Added assertions cover physical-bank and linked
labels, navigation, saved-project separation and watch-ID collisions. Sampling
is still restricted to open inspector windows and requested addresses.

#### Performance

The preserved pre-change benchmark executable came from `00c5978`. Both
executables used the same core binary and same ROMs read in place, 360 warm-up
frames followed by 1,800 timed frames, three rotated trials per workload.
Measurements cover core/host/script execution, not Qt rendering, disk or device
audio output. Normal mode renders every frame with PCM; accelerated mode renders
one in 32 with audio muted. The new hook workload observes the ROM's VBlank
handler and reads a work-RAM word, without changing the emulated state.

| Game/region | Output | Old baseline | New baseline | New empty runtime | HUD | Automation | VBlank hook |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Lion King, NTSC-U 59.92274 Hz | Full video/audio | 549.61 | 545.09 | 550.46 | 559.94 | 549.57 | 726.03 |
| Lion King, NTSC-U | 1/32 video, muted | 366.77 | 368.26 | 370.57 | 376.08 | 373.66 | 538.68 |
| Sonic 2, PAL 49.70146 Hz | Full video/audio | 628.05 | 684.59 | 690.54 | 740.58 | 696.25 | 851.45 |
| Sonic 2, PAL | 1/32 video, muted | 425.84 | 455.17 | 449.85 | 463.63 | 467.49 | 638.45 |

Values are median microseconds per frame. The Sonic sequence showed host-load
drift; a follow-up run of the **unchanged old executable** measured 685.59 us
normal and 432.64 us accelerated (ranges 668.45–700.95 and 430.20–450.46),
so the initial sequential old/new delta is not an isolated code regression.
The empty runtime in the new build differs from its own baseline by -1.17% to
+0.99%. Explicit execution observation costs 0.17–0.18 ms/frame here, or roughly
24–46% extra core execution work. It is not free; no native access hook is
connected for ordinary HUDs or when no subscriptions exist. Heavy callback
scripts can cost more and are bounded by the documented quotas.

All workloads and both executables retained identical final native state, image
and PCM digests for each game/output mode. Raw measurements are retained in
ignored `build/verification/lua-phase3`. Linux application and Windows cross-build
compile successfully; Windows interactive verification remains with the user.

The final Windows testbuild matches all 83 staged package files byte for byte, including
the executable, languages, example and API guide. No Windows application was
launched or controlled. Generated package staging and the old benchmark copy
were removed after verification; incremental builds and small evidence logs remain.

### Lua Phase 4A: files and native recordings — 2026-11-27

The native core and its deterministic format were not changed. The frontend
adds general asynchronous script storage and control over its existing native
input recordings. File preparation, parsing and explicit-stop persistence are
off-thread. Native configuration/state capture still use the established safe
emulation boundary. No filesystem operation or waiting occurs in Lua callbacks.

Focused Linux checks passed:

- lua_runtime_tests: binary file round trip, partial reads and EOF, per-source
  identity and restart, bounded queues/results, unsafe names/links and oversized
  files, large-string bounded slices, and the shipped file-roundtrip.lua example.
- movie_tape_tests: existing input file readers/writers plus exclusive creation
  that leaves an occupied destination unchanged.
- lua_session_tests on the real core and generated MD fixture: exact recorded
  controls, start/reset continuation, playback, new branch identity and shared
  prefix, resume with backup, finished playback takeover, and stale preparation
  cancellation after reset. A test-only 200 ms worker delay confirmed live game
  frames continue during a detached save. Deliberate output failure retained the
  captured tape, and retry saved the original endpoint rather than later gameplay.
- The same session check executes the shipped recording-activity.lua in record
  and replay modes, including its file data and visible graph aligned to the
  native recording. It does not substitute a simplified implementation.
- lua_automation_tests: existing input merge, paused steps, temporary-state branch,
  script-free playback, error cleanup and timeline cancellation. Its old file
  completion assertion was updated to wait for the new asynchronous save.
- lua_window_tests and a focused folder check: configured Scripts directory,
  changing the directory between opens, portable relative defaults/overrides and
  platform-specific absolute overrides.

Review also corrected the active-recording close race, GUI acknowledgment for
rejected busy movie/export actions, and a sample that could continue stepping
after a failed start. Storage and movie workers have explicit queue/ownership
bounds; scripts never spawn new emulated machines. No broad hardware audit or
Windows interactive testing was repeated for these frontend changes.

Linux and Windows Release applications compile successfully. Windows testbuild
contains both English/Russian API references under docs, the configurable Scripts
path and the two new examples. All 85 package entries match their SHA-256
manifest and staging byte-for-byte. EXE SHA-256:
500bec08f0a189fd8c749f49525198e379a19e313f2788d522355c65db971031.
Build/packaging logs and hash verification remain under ignored
build/verification/lua-phase4; package staging was removed. User files were
retained. No GitHub release or updater version was created for this batch.

## Lua Phase 4B — native script tool windows (2026-11-27)

Implemented separate Qt panels over a bounded worker-owned model. No emulation
core or deterministic state layout changed in this batch.

- lua_runtime_tests passed: typed descriptors and values; nested layouts;
  click/change/close callbacks; accepted-value reads and same-value acknowledgments;
  queued input/core controls; callback context restrictions; stale run/epoch;
  duplicate IDs, invalid/dense arrays, depth, finite values and global resource
  limits; stop/error cleanup. Existing real-core runtime checks pass across MD,
  SMS, SG-1000, GG, GG Link A/B, 32X, Firecore, Sega CD and CD+32X.
- lua_panel_view_tests passed offscreen and under virtual X11: widget creation,
  stable updates, event delivery, edits awaiting acknowledgment, epoch changes,
  queue rejection, close/restart/stop cleanup, combo popup ownership and graph
  extremes. An isolated X11 check confirmed the nonactivating window preserves
  main-window focus. No Windows UI interaction was performed.
- lua_session_tests passed on the generated original MD cartridge: a UI handler
  executes while paused without advancing the emulated frame; memory changes
  are applied before the paused HUD redraw; stale events after rewind, restart
  and a same-batch reset are ignored; close/stop release panels. The actual
  shipped tool-panel.lua ran, produced a graph after a frame, and toggled its
  HUD while paused. Existing native movie/save/retry/cancellation checks passed.

Review found and fixed same-batch control ordering, structural window-update
churn bypassing the publication limit, and script compatibility cleanup after a
failed power reset. Value/structural updates are capped together at 20 Hz; stopping
all scripts clears immediately. A full queue returns to the accepted value.
No scripts means no panel manager allocation, snapshot copies or polling timer.
Existing Lua execution budgets remain in force.

Both Linux and Windows Release applications compiled. English/Russian references
and tool-panel.lua are included by the existing package rules. No new release
or updater version is created for this batch.

Windows testbuild: all 86 files match the package SHA-256 manifest and
verified staging bytes. EXE SHA-256:
04749c824ccd486963223e703b3e91433948a462275c879baceac6e2179efb3f.
Packaged example matches the source. Logs and package verification are retained
under ignored build/verification/lua-phase4b; staging and duplicate standalone
widget-test compilation artifacts were removed. User files were retained.

## Lua Phase 4C — declared data with saves and rewind (2026-11-27)

Added opt-in `timeline.define/get/set/status`. Only explicitly declared values
are saved: not the Lua VM, locals, coroutine stacks, widgets, files or handles.
The identity is declaration ID, schema and exact source SHA-256. Matching active
scripts restore before the load/rewind callback; absent scripts are never started.
Missing or incompatible data initializes declared defaults. Ordinary saves
without Lua retain their previous format; older emulator builds cannot read
the optional attachment. Native input recordings and network snapshots do not
include these values.

Storage is bounded to 16 KiB per script, at most eight scripts, with depth,
node and string limits. Unchanged values reuse a cached aggregate. Local rewind
uses sparse independent reverse patches within its existing memory cap, so
Lua payload length changes cannot force recompression of the native state.
No declared data means no auxiliary per-frame entries or payload allocation;
stopping scripts frees the live declarations. Existing bounded history can
retain previous data until evicted or cleared, allowing a matching restart
to restore it. Temporary Lua states count the attachment against their budget.

Focused verification:

- Runtime codec: deterministic encoding, supported value types, exact integers,
  source/schema matching, missing values, bounds, rejected cycles/metatables,
  malformed attachments, cleanup and cached unchanged captures.
- Real-core session on the generated original cartridge: slot/file save and
  restoration, load without a script, restart of matching source, changed
  source, legacy save, reset, temporary Lua save/load and local rewind. The
  shipped timeline-counter.lua is executed directly, including reverse
  restoration. Native recording playback resets to defaults while a rerecord
  fork retains current declared values.
- File and movie checkpoint tests: framing, checksum, backup, coexistence with
  achievement progress, legacy files, validation before core mutation, and no
  script payload in generic movie/native capture or the .krm tape.
- Rewind tests: sparse changes, empty values, varying sizes, accelerated
  stepping, branch alignment, front eviction, shared memory budget and
  core-only reconstruction of one/two video fields.
- Review corrected duplicate Lua restoration during a failed preview retry
  and stale Lua data when canonical rewind restoration succeeds but preview
  rendering fails. Reset/load with no declarations avoids an empty QMap
  allocation.

Performance harness uses the same core and settings for every workload:
Sunset Riders (USA), NTSC-U 59.92274 Hz, and Aladdin (Europe), PAL 49.70146 Hz.
Each has 120 unattended startup frames and two rotated 600-frame measurements.
Normal rendering includes audio and rewind; accelerated measurements render
every 32nd frame, disable audio and keep no rewind history. These are startup
scenes measured inside the core/runtime harness, not gameplay or whole-window
GPU benchmarks. Native state, final video and PCM hashes matched across all
workloads within each rendering policy.

For 599 retained frames, empty-runtime history was byte-for-byte the same size
as baseline: 1,355,698 bytes for Sunset Riders and 1,745,941 for Aladdin.
A constant 63-byte declaration added 110 retained bytes; an every-frame counter
added 56,416 bytes. The near-limit varying-length case added 183,791 bytes.
An early implementation that wrapped Lua around every native checkpoint had
used 23.4 MB on the same Sunset Riders workload; that design was removed.

Timing is noisy under WSL: ordinary baseline trials varied particularly for
Sunset Riders. Empty-runtime means differed from baseline by -7.75% and +1.77%,
which is not evidence of a speed gain. Small every-frame counters measured
-9.02% and +7.00%. Updating/encoding nearly 16 KiB every frame costs work:
varying-length means were 0.867/0.965 ms versus 0.749/0.828 ms baseline,
about +0.118/+0.137 ms per emulated frame. In accelerated mode the same
stress case added approximately 0.021/0.037 ms (+7.30%/+9.90%).
Do not describe unrestricted Lua computation or large every-frame updates as
free; small explicitly chosen values are the intended use.

Logs and measurements are retained under ignored build/verification/lua-phase4c.

Linux and Windows Release builds passed. The final runtime, real-core session
and movie integration checks passed after the review fixes. Windows testbuild
contains the new example and both API references. All 87 package files match
their SHA-256 manifest and staging; the sample and references match source.
EXE SHA-256: 2eb1fd39b40ec399e7e695f2acf1702524c1ff6a9550f6e8a7a8bec44a3adfcd.
Package staging was removed after verification; user files were retained.
No GitHub release or updater version was created for this batch.
