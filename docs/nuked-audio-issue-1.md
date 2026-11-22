# Nuked YM2612 audio investigation — issue #1

Investigated [issue #1](https://github.com/citrongames/Kega-Refusion/issues/1)
on 2026-11-22. **The two Nuked optimizations do not change the measured PCM.**
The published 0.26.0 and 0.29.0 cores do produce different PCM for this game;
the start of that difference was isolated to the VDP FIFO/DMA timing correction,
not the Nuked changes. No audio implementation was reverted.

## Controlled comparison

Game: The Adventures of Batman & Robin (USA), SHA-256
`9343edb48b8c6afcf44536875d5430548737599bbc49ea93cc5cf430b18ed0aa`.
A headless Linux libretro harness ran cold boot with no input, NTSC-U
59.92274340431231 Hz, Nuked YM2612, high-quality FM resampling, no output
filter, FM preamp 100% and PSG preamp 150%. It hashes signed 16-bit stereo
PCM directly from the core callback, before Qt, the OS mixer or Voicemeeter.
All runs use identical options and an isolated temporary data directory.

- Current core versus an isolated copy with **both** `nuked-output` and
  `nuked-operators` patches reversed: all 32,400 per-frame hashes and the whole
  PCM stream match. There are 23,844,702 stereo sample frames at 44,100 Hz,
  approximately 540.696 seconds. No sample differences were detected.
- Published Linux 0.29.0 matches that current output over all 32,400 frames.
- Published 0.26.0 differs from 0.29.0, beginning at frame index 19. Output
  length and rate are identical. Their common queried options match.
- Over the first 1,200 frames, published 0.27.0 (already containing the Nuked
  optimizations) matches 0.26.0; published 0.28.0 matches 0.29.0.
- An isolated native build at `e868cfa`, the first Mode 5 FIFO/active-display
  DMA timing correction, reproduces the **exact 0.28.0 output** on those same
  1,200 frames. This identifies the cause of the measured early difference.
  That correction changes CPU waits and bus timing, which can change the
  times at which game code programs the audio chip. It does not replace the
  FM synthesis algorithm or resampler.

The nine-minute optimization A/B PCM SHA-256 is
`c2179d56f37c3043162cb00d97a5e25697c5c0805c83008bd4b382c7c37d2bd9`.
Binary/ROM identities and compact results are in
[nuked-audio-issue-1-results.json](nuked-audio-issue-1-results.json).

## Output-path review and limits

`AudioOutput`, `AudioQueue` and `FramePacer` are unchanged between the compared
releases. Archived Windows 0.26.0 through 0.29.0 carry identical Qt 6.10.2
Core/Multimedia/platform DLLs. At normal game speed the application passes the
core's 44.1 kHz/S16 PCM to QAudioSink. The application speed resampler runs only
when game speed differs from 100%. There was no switch to a cheaper IIR filter
or new audio polling interval as part of either Nuked optimization.

The issue's 48 kHz/24-bit Voicemeeter recordings include a format conversion
beyond this core comparison. Their original WAVs and exact settings are not
attached, so the reported Windows capture jitter, level drops, inversion versus
RetroArch and perceptual degradation have **not** been reproduced or disproved.
Different PCM caused by corrected emulated timing is not, by itself, evidence of
poorer synthesis. The first 20-second timing isolation is not a measurement of
the reported capture discontinuity at 2:47. No Windows desktop session was run
or controlled for this investigation.

## Reproduce the core comparison

The bounded probe retains hashes, not the complete audio stream:

```sh
python3 -B tests/nuked_pcm_probe.py --core /path/to/core.so \
  --rom /path/to/game.md --frames 32400 --output build/pcm-result.json
```

Run each core in its own process; compare `pcm_sha256`, sample counts and
`per_frame_pcm_sha256`. The optimization control was built from the same source
and dependencies, reversing only the two Nuked patches and rebuilding
`sound.c`/`ym3438.c` plus relinking. No ROM contents, timings or input were
changed in that control. Temporary diagnostic builds are removed after the
investigation; source releases and user game files remain untouched.
