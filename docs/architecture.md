# Architecture and current boundary

Kega Refusion is a standalone C++17 / Qt6 Widgets application for Linux and Windows. The initial implementation supports cartridge loading via a pinned Genesis Plus GX libretro core; it is not yet the complete feature list from info.txt.

## Ownership

`MainWindow` owns presentation and keyboard mappings. `GameView` paints copied RGB frames. `AudioOutput` owns Qt Multimedia output on the GUI thread. `EmulationSession` runs a dedicated emulation thread. GUI actions enqueue commands; only the emulation thread calls `LibretroHost` and core functions. Input masks are atomic, latched once per frame. Core callback buffers are copied before returning.

`LibretroHost` dynamically resolves the libretro API; this keeps core-specific calls out of UI and leaves room for a second adapter/core for 32X. A process-wide owner prevents concurrent instances of the non-reentrant core. This is an explicit current limitation, not a general-purpose multicore host.

## Timing and persistence

Frames use the core-reported rate and a steady clock. Fast-forward currently targets 4x and suppresses audio. The first version uses a modest audio buffer; adaptive audio-clock synchronization and low-latency tuning remain follow-up work.

Battery RAM is saved atomically every five seconds, on game switch and clean shutdown. Save files are keyed by complete ROM SHA-256, independent of filenames. Invalid existing battery saves are preserved, with writes disabled for that session. Quick states include a format marker, ROM hash, core-binary hash, payload hash and length. Incompatible/truncated/corrupt states are rejected before reaching the core. Slots are 0 through 9. QSaveFile avoids partially overwritten save files.

Genesis Plus GX declares platform-dependent serialization. The first state format deliberately requires the same core binary; Windows/Linux quick-state interchange and compatibility across core updates are not promised. Battery saves use raw core memory and are independent of quick-state envelopes.

## Deferred scope

32X/PicoDrive, Sega CD/BIOS and multidisc identity, archives, gamepad discovery and peripheral UI, shaders/Blargg controls, manual region options, rewind, netplay, achievements, movies, recording, Lua and debugging tools are not part of the initial foundation. These will be added in testable steps. No promise of full ROM collection compatibility is made from the smoke ROM tests.

Normal core file-state restoration resets sound resampler history, so immediate PCM bytes after loading a state are not guaranteed identical. The core fast-savestate flag keeps audio history only in global process memory and must not be enabled for file states or assumed to support a history of multiple snapshots. A dedicated full-audio serialization solution is needed before claiming audio-exact rollback.

Before netplay or movies: validate input-latched determinism over longer runs and supported platforms, design versioned session metadata, bounded history and resimulation, and define policies for pauses/resets/state loads. Shader rendering must stay separate from original emulation output and timing.
