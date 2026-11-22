# Audio recordings in issue #3

Investigated on 2026-11-22 after [issue #3](https://github.com/citrongames/Kega-Refusion/issues/3).
The reporter supplied [two WAV excerpts and a DeltaWave report](https://github.com/citrongames/Kega-Refusion/issues/3#issuecomment-7080223705).

## Findings

The submitted recordings differ. This is confirmed independently and is not
dismissed as a subjective listening claim. Their difference does not identify
which version is more accurate, or establish that the Nuked optimizations caused
it. The origin of the difference in these particular captures remains unresolved.

A new 32,400-frame run of the published Linux 0.27.0 core reproduces the retained
0.26.0 raw PCM result exactly: 23,844,702 stereo sample frames at 44,100 Hz,
540.696 seconds. Both whole-stream SHA-256 values are
`038c3014e09a771e539f09598ac5c676ba344999ae0df45edcaeddd09305aa0f`.
This extends the earlier direct 0.26/0.27 comparison from 1,200 frames to nine
minutes. It uses Batman & Robin USA, cold boot, no controller input, NTSC-U,
Nuked, HQ FM resampling, filter disabled and identical gains. The earlier 0.26
measurement was reused; this investigation did not repeat its capture.

The source of AudioOutput, AudioQueue and FramePacer is identical between the
0.26.0 and 0.27.0 release commits. Packaged Windows Qt6Core, Qt6Multimedia and
qwindows DLL hashes also match. No faster replacement resampler or IIR filter
was introduced in that change. This is source/package evidence, not a Windows
WASAPI/Voicemeeter reproduction. No Windows program was launched.

## Submitted captures and interpretation

Each WAV contains 380,783 stereo sample frames, 48 kHz, 24-bit PCM: approximately
7.933 seconds. DeltaWave analyzed only the left channel. Its settings enable
gain, DC removal and subsample alignment, but disable both linear and nonlinear
drift correction. Its displayed zero ppm is not evidence that drift was measured
and found absent. The report separately records an initial integer alignment of
-10 samples and a final fractional offset of -1.202 samples.

The left-channel original RMS and peak levels reproduce the report. The recordings
are not a simple polarity inversion. A single delay does not explain their
waveform difference; fitting arbitrary equalization or nonlinear time warping
would not establish correct emulation and was not used to claim equality.

Independent stereo analysis found approximately -0.021 dB (left) and +0.022 dB
(right) changes in overall RMS. Delay/DC/RMS-level alignment left residuals of
-20.27/-20.39 dBFS; a diagnostic linear time adjustment did not materially reduce
them. Aggregated mid-channel band energies from 50 Hz to 16 kHz differed by at
most 0.084 dB, but this must not be interpreted as perceptual equality. These
ordinary residual measurements are not a reproduction of DeltaWave's PK metric.

The source-matching method found a known scaled core excerpt at its exact
60-second location and matched the two reporter clips to one another. Neither
clip had a reliable match in the full controlled trace. This does not prove
that the reporter used a different game.

The phase and null measurements quantify differences between the captured
signals. They do not, on their own, locate a defect in FM synthesis, measure a
physical audio-clock fault, or make another emulator an original-hardware
reference. The prior [hardware comparison](nuked-audio-issue-1.md#comparison-with-original-consoles)
found no consistent winner between old and timing-corrected Batman PCM on the
sampled passages, and must not be described as proof of complete audio accuracy.

## Remaining reproduction gap

The new comment does not identify the exact ROM/hash, playback scene or track,
input sequence, emulator settings, installed core binary, or a same-version
repeat capture. The old issue identifies Batman's title track, but that does not
establish the provenance of these newly cropped files. The supplied excerpts
could not be reliably located within the controlled nine-minute trace.

A useful next submission would identify the exact scene and settings and include
two repeat captures of one version alongside the cross-version pair. Comparing
raw core PCM with post-output capture for that same scenario would then locate
the first differing boundary. Another rollback of Nuked, or a filter chosen to
make one recording resemble another, is not justified by the current evidence.

The separately confirmed GUI-stall output problem was addressed in
[0.30.1](audio-worker.md). That mechanism has not been established as the cause
of these two files. No synthesis, timing, filter or output code was changed in
this follow-up, and no new build was needed.

Compact identities and measurements are in [the results](nuked-audio-issue-3-results.json).
Diagnostic scripts and fuller evidence remain under ignored `build/audio-issue3/`.
