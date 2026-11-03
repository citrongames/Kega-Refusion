# Local display and audio diagnostics (0.17.0)

The Debug menu is present only for a loaded system with supported output controls.
It opens separate modeless Graphics and Sound Channels windows; focusing these
windows does not pause the game. Other settings dialogs and application focus
loss retain their existing pause behavior.
Checked means visible/audible. Each audio channel also has an independent 0–100%
output level, initially 100%; a zero level does not change the checkbox. New game loads reset disabled masks and channel gains; closing a game
shows an empty placeholder in already open windows. The backdrop color is session-local, defaults to #FF00FF,
and is used only while video layers are hidden. No per-toggle settings writes.

The emulation thread owns mask updates. They are local presentation commands,
not network commands, cheats or recording input. Both isolated Link Cable cores
receive the same local masks. Video hiding is unavailable during achievement
Hardcore, and enabling Hardcore restores the video layers.

Video: Mega Drive/CD/Pico expose planes A and B, window and sprites; the 32X
attachment adds its composed layer. SMS/Mark III/GG/SG-1000/PBC expose background
and sprites. Original sprite evaluation still updates collision/overflow flags.
Background masking happens before priority mixing; hiding the window does not
reveal plane A where hardware substitutes the window. IM2 clipping uses original
scanline coordinates. The temporary output palette never changes CRAM.

Audio: PSG tones/noise; YM2612 FM and DAC; enabled SMS YM2413 melody/rhythm;
CD PCM channels and CDDA stereo outputs; 32X PWM stereo outputs; Paprium external
music. Muting suppresses output contributions without stopping oscillators,
envelopes, PCM address advancement, drive reads or music progression. Chip/native
state layouts and recording compatibility IDs are unchanged. Audio resampler
history may differ between listeners with different local masks, as expected;
these are not gameplay memory differences.

Normal rendering has no additional row copy. Sprite hiding uses one bounded
512-byte row copy, and the replacement palette is bounded. Additional audio
output caches are fixed-size and outside serialized hardware structs.

See [debug-controls-verification.json](debug-controls-verification.json) for executed checks,
and [video-aspect-audit.md](video-aspect-audit.md) for Console aspect calculations
and exact native geometry changes.

Blargg composite/S-Video/RGB and GLSL now share one exclusive Shader menu.
The Blargg choice is local output processing, so recording/network configuration
restoration preserves it. It is not sent as a gameplay command to other peers.
Changing display processing does not alter the recording input timeline.

Slider commands are coalesced per channel until the next emulation boundary.
There is no settings write or audio-device reset for each slider movement.

Master volume is independent of diagnostic channel gains: `+`/`=` and `-`
(including keypad) change it by 5 percentage points and show an OSD notice.
These keys are reserved from controller bindings. The normal master-volume
preference is saved after a 400 ms quiet period or on exit; diagnostic masks,
gains and backdrop are never written to settings.
