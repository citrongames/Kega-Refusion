# Architecture and current boundary

Kega Refusion is a standalone C++17 / Qt6 Widgets application for Linux and Windows. The initial implementation supports cartridge loading via a pinned Genesis Plus GX libretro core; it is not yet the complete feature list from info.txt.

## Ownership

`MainWindow` owns presentation and routes actions. `InputManager` owns SDL device discovery, per-system/per-player mappings and keyboard/pad state. `GameView` paints copied RGB frames and timed OSD messages with the embedded Cyrillic-capable pixel font. `AudioOutput` owns Qt Multimedia output on the GUI thread. `EmulationSession` runs a dedicated emulation thread. GUI actions enqueue commands; only the emulation thread calls `LibretroHost` and core functions. Input masks are atomic, latched once per frame. Core callback buffers are copied before returning.

`LibretroHost` dynamically resolves the libretro API; this keeps core-specific calls out of UI and leaves room for a second adapter/core for 32X. A process-wide owner prevents concurrent instances of the non-reentrant core. This is an explicit current limitation, not a general-purpose multicore host.

## Timing and persistence

Frames use the core-reported rate and a steady clock. Fast-forward targets the selected 2x/3x/4x/6x/8x rate and suppresses audio. The first version uses a modest audio buffer; adaptive audio-clock synchronization and low-latency tuning remain follow-up work.

Battery RAM is saved atomically every five seconds, on game switch and clean shutdown. Save files are keyed by complete ROM SHA-256, independent of filenames. Invalid existing battery saves are preserved, with writes disabled for that session. Quick states include a format marker, ROM hash, core-binary hash, payload hash and length. Incompatible/truncated/corrupt states are rejected before reaching the core. Slots are 0 through 9. QSaveFile avoids partially overwritten save files.

Genesis Plus GX declares platform-dependent serialization. The first state format deliberately requires the same core binary; Windows/Linux quick-state interchange and compatibility across core updates are not promised. Battery saves use raw core memory and are independent of quick-state envelopes.

## Deferred scope

32X/PicoDrive, Sega CD/BIOS and multidisc identity, specialized peripherals, shaders/Blargg controls, manual region options, netplay, achievements, movies, recording, Lua and debugging tools are not part of the initial foundation. These will be added in testable steps. No promise of full ROM collection compatibility is made from the smoke ROM tests.

Normal core file-state restoration resets sound resampler history, so immediate PCM bytes after loading a state are not guaranteed identical. The core fast-savestate flag keeps audio history only in global process memory and must not be enabled for file states or assumed to support a history of multiple snapshots. A dedicated full-audio serialization solution is needed before claiming audio-exact rollback.

Before netplay or movies: validate input-latched determinism over longer runs and supported platforms, design versioned session metadata, bounded history and resimulation, and define policies for pauses/resets/state loads. Shader rendering must stay separate from original emulation output and timing.

## Input and controls (0.2)

SDL2 runs on the GUI thread and is polled every 8 ms. Known controllers use SDL logical controls; unknown joysticks expose raw buttons, axes and hats. Automatic defaults remain distinct from explicitly assigned raw controls. Stable device keys use GUID, serial where available, and a collision-free ordinal for identical devices. Two player masks are merged from keyboard and pad input and passed atomically to the emulation thread.

Gameplay input is disabled during modal dialogs, menus and loss of focus. Command edges rearm only after release, preventing a held reset/load/fast-forward control from firing again on refocus. Capture waits for neutral input, handles trigger rest positions and times out after ten seconds. Unplugging clears held state. Legacy keyboard settings migrate on first use.

ControlsDialog keeps local drafts per system and player; Save applies them and Cancel discards them. ControllerDiagram paints four system-specific diagrams and supports selecting a target by clicking it. Commands have a separate mapping tab. OSD contains only the latest action result/slot notice and active pause/fast-forward indicators, all in the upper-left corner. Resuming or returning to normal speed removes its indicator silently. General core/device/menu/FPS notifications do not enter OSD. Disabling OSD clears transient notices and suppresses all drawing; enabling restores current statuses only. Reset/save/load notices originate from successful core command completion, not from attempted requests. Fullscreen hides the menu; Esc toggles it without leaving fullscreen.

The chosen ImageGen KR icon is embedded both as a Qt resource and as the Windows executable icon. Source and generation prompt are retained in assets/ and docs/icon-design.md.

## Layout-independent keyboard input (0.2.1)

Keyboard defaults and stored mappings use Qt key identifiers for US physical key positions. Windows Set-1 native scan codes and Linux XKB keycodes normalize printable keys before gameplay or shortcut dispatch; release uses the same identity even after a layout switch. Older Cyrillic key identifiers migrate to their corresponding positions. Non-text/keypad identities remain Qt keys. Controls capture uses the same normalization and displays canonical key labels.

Digits 0–9 select state slots, including keypad digits with Num Lock enabled. They are reserved in the editor, mapping load/save and runtime input. Existing digit bindings cannot shadow slot selection. ShortcutOverride prevents Qt's logical shortcuts from competing with handled physical pause/mute/reset commands.

## Archives, rewind and pause reasons (0.3)

RomArchive lists and validates ZIP/7z entries, then returns the selected ROM bytes in memory. No member path is extracted. The GUI prompts only when multiple supported cartridges exist. LibretroHost exposes retro_game_info_ext during retro_load_game: Genesis Plus GX uses this interface to consume ROM bytes; the older retro_game_info.data alone is insufficient for this core. Archive member names determine the system profile, while save identity remains the SHA-256 of ROM bytes. A packed and unpacked copy therefore share states and SRAM. See docs/archive-support.md for dependency versions and resource bounds.

RewindBuffer records every second emulated frame. Each checkpoint contains a compressed core state, full captured SRAM/RTC and a compressed RGB frame. Cached video corresponds to that checkpoint, so stepping backward restores state and displays the matching image without simulating a new forward frame. Rewind mutes audio and clears queued samples at both boundaries. Normal resume regenerates audio; byte-identical PCM rollback is not claimed. Restoring an older point discards its future. The oldest retained point remains frozen while the rewind control stays held. Load/reset/new ROM/history-option changes clear history. History is capped at 128 MiB plus temporary work buffers and at the selected 10/30/60 seconds; large checkpoints may shorten the duration. No periodic battery-file writes occur while rewinding.

Manual pause and background pause are independent. Returning focus clears only background pause; manual pause is retained. A rewind request can inspect older frames during manual pause, but background pause blocks rewind. R is reserved for rewind and is normalized by physical key position. The command is appended to the gamepad command IDs to preserve saved bindings.

The slot OSD follows FCEUX's filled-versus-empty ten-cell concept at the user's preferred upper-left location. The selected slot is outlined and labelled occupied/empty. The worker queries file presence on selection and refreshes the occupancy mask after successful saves. Occupied means a slot file exists, not that its contents have passed load validation. Slots disappear after two seconds and obey the global OSD switch. No save manager or permanent status panel is added.
