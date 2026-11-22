# Windows audio output comparison

2026-11-22. Requested follow-up to [issue 3](nuked-audio-issue-3.md).

## Method and scope

A native Windows diagnostic runner uses the published core DLL, the corresponding
production Qt audio queue/output/pacer sources, and the release Qt runtime. It
cold-boots Sunset Riders (USA), enters Options, selects SOUND 00 and presses A
with the same frame-indexed controller tape each time. The tape is diagnostic
input data, not a KRM recording. Nuked is verified through the core export;
NTSC-U, 44.1 kHz stereo PCM16, no Low-pass, FM100/PSG150, normal latency,
stream volume 20%. Each run records 1,800 frames (approximately 30.04 seconds).
The runner suppresses output during menu navigation and first opens the device
at capture start. In normal use the emulator opens it during game boot; this
startup finding does not imply reopening or losing samples at every music change.

Three boundaries are captured: raw core PCM, bytes actually accepted by Qt's
output stream, and process-specific Windows WASAPI loopback at 48 kHz stereo
Float32. The loopback excludes other applications and microphones. It measures
the Windows software output path, not a physical DAC or acoustic output.

The machine uses Windows build 26300 and default Realtek headphones, preferred
48 kHz. No device settings or master volume are changed. The runner has no full
emulator GUI/GPU, desktop input hooks or Voicemeeter. Diagnostic file taps add
small overhead; conclusions apply to these controlled runs, not all hardware.

## Published 0.26.0 versus 0.27.0

One 0.26.0 run, a same-version repeat, and one 0.27.0 run all produce identical
raw PCM and identical accepted PCM. Each source contains 5,298,824 bytes. Each
output accepts 5,297,632 bytes: exactly the source with the first 298 stereo
samples removed. This is 6.757 ms of startup-only queue overflow while opening
the Windows sink, not a later music dropout and not a regression unique to 0.27.
The discarded samples are not all silent. After that prefix, every source byte
reaches the output stream in order.

The repeated 0.26.0 and 0.27.0 WASAPI captures are also byte-identical over their
entire aligned active signal after a single constant time offset. No gain
normalization, resampling, filtering or nonlinear time alignment is applied.
The first 0.26.0 capture differs from both of those captures in the same way;
its scope and the current threaded-output measurements are recorded below.

The identical loopback pair covers 1,443,583 active stereo Float32 frames
(30.07465 s, including the output filter tail). The first 0.26 run versus either
later run has exactly the same residual: approximately -63.4 dBFS over the
whole aligned signal and -69.2 dBFS after its first second. Its small residual
continues through the track; it is not merely leading silence. The largest
transient occurs approximately 54 ms into the aligned overlap. Ten local
alignment windows retain the same integer lag throughout the track. These
facts demonstrate run-to-run output/capture variability here, not a change
unique to 0.27. They do not establish the cause of the reporter's captures.

All three loopbacks have no interior run of even two exactly-zero stereo
samples and no WASAPI discontinuity/timestamp error flags. Such checks are
useful evidence against silence gaps; they cannot rule out every possible
analogue or nonzero waveform distortion.

## Published 0.30.1 with the audio worker

Ran the same 1,800-frame sequence with exact published 0.30.1 core/runtime and
its production threaded output sources. Repeated it with 58 main-thread stalls
of 60 ms, scheduled every 500 ms after playback warm-up. This simulates GUI
service delays, not full graphical rendering or machine-wide CPU starvation.

Both runs produce identical source PCM and identical accepted PCM. Each loses
only the first 1,192 bytes (298 stereo samples, 6.757 ms) before the first device
write. Subsequent queue overflow is **zero**, unclassified missing bytes **zero**,
write errors **zero**, and pending bytes after drain **zero**. Each opens the
sink once. The one reported transition to idle occurs after the last source
frame and the output has drained; it is not an interior underrun.

The queue's normal 60 ms bound is 2,646 stereo frames at 44.1 kHz. Four video
frames submit approximately 2,944 audio frames while the cold Windows sink is
opening: the oldest 298 overflow that bound. This is an existing startup edge,
not a defect introduced by separating the audio worker. A worker does not make
cold device initialization instantaneous. No production code was changed here.

The newer core's raw PCM differs from 0.26/0.27, which include different core
timing revisions. The output check compares each version's own input to its
accepted output; it does not mistake core changes for a Windows output defect.

Machine-readable evidence is in [the results](windows-audio-capture-results.json).
All five original loopback WAVs were supplied in the user's audio-comparison
folder, without changing volume, removing silence, resampling or normalizing.

Both current loopbacks contain no interior exactly-zero stereo samples and no
capture discontinuity or timestamp errors. Their small residual after constant
alignment is approximately -62.9 dBFS overall and -68.5 dBFS after the first
second, comparable in magnitude to the old same-version repeat. Because Qt's
accepted PCM is identical, this difference arises downstream of that boundary.
The experiment does not distinguish resampler start phase, Windows processing
or other capture variability, nor prove that GUI load caused the residual.
