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
