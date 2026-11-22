# Audio output under load — Batman & Robin

Investigated on 2026-11-22 after [issue #1](https://github.com/citrongames/Kega-Refusion/issues/1).
The current core and accuracy fixes were unchanged. This is an output-path
investigation, separate from the [Nuked/timing comparison](nuked-audio-issue-1.md).

## Follow-up

The later [audio-worker change](audio-worker.md) removes the GUI-thread dependency.
The measurements below describe the pre-fix implementation at `dcb4999`.

## Findings

**Sustained GUI stalls can discard PCM and cause audible gaps without a Qt audio
error.** The ordinary and CPU-loaded runs were clean at the application/device
write boundary. This is a demonstrated failure mechanism, not a reproduction
of the reporter's Windows/Voicemeeter setup or proof of a new 0.29 regression.
The output/queue/pacer implementation itself is unchanged since 0.26.

`AudioOutput::pump()` runs on the GUI thread. The core produces PCM on its own
thread, but a stalled GUI stops replenishing the device. The normal software
queue is bounded at 60 ms; overflow intentionally discards old whole samples
rather than accumulating unbounded latency. This preserves memory bounds but
cannot preserve continuous sound during a long consumer stall.

| Scenario | Emulated frames | Discarded stereo sample frames | PCM accepted by Qt |
| --- | ---: | ---: | --- |
| Normal, approximately 180 s | 10,800 | 0 | Identical to raw core |
| Two CPU burners, all test threads on two logical CPUs, approximately 180 s | 10,800 | 0 | Identical to raw core |
| GUI blocked 10 ms every 500 ms, approximately 40 s | 2,400 | 0 | Identical to input |
| GUI blocked 25 ms every 500 ms, approximately 40 s | 2,400 | 0 | Identical to input |
| GUI blocked 60 ms every 500 ms, normal latency | 2,400 | 70,548 (1.600 s) | Samples discarded |
| Same 60 ms stall, stable latency | 2,400 | 0 | Identical to input |
| Positive control: GUI blocked 250 ms every 500 ms, approximately 20 s | 1,200 | 321,325 (7.286 s) | Samples discarded |

Stalls begin after three seconds. The two 180-second streams match each other
and a separate raw-core run exactly, including the reported 2:47 area:
`7c9ec57ed3df46a1b309a1d5afc640daea2d9925ef84b6742b79edaf298424ad`.
They contain 7,948,234 stereo sample frames. Exact run data and identities are in
[audio-output-load-results.json](audio-output-load-results.json).

Qt's `IdleState` is **not a reliable underrun detector on this backend**: even
the positive control did not report an active-to-idle transition. Initial
measurements also saw the queued startup Idle notification after the first
write; the final diagnostic observes transitions directly to avoid that false
count. No claim of uninterrupted physical output is based on this counter.

## System-output capture and other emulators

A second check used a private PipeWire/PulseAudio stereo null sink and captured
its monitor as 48 kHz/S16 WAV, without touching the user's default output.
Our probe was unmuted only into that isolated sink. All games used the same USA
ROM, cold boot and no controller input. Interior exact-zero intervals of at
least 4 ms were counted after the first second of detected music and before
the final 100 ms of music, excluding launch/stop silence.

- The 55-second Kega control captured no such interior silence and lost no
  application samples. Source and accepted SHA-256 also match a raw-core run.
- With 34 GUI stalls of 60 ms during a 1,200-frame run, the normal profile
  discarded 58,162 stereo sample frames (1.319 s). The monitor captured
  **34 gaps totaling 1.429 s**, mostly 42.667 ms long with the imposed 500 ms
  cadence. Qt still reported no idle transitions or output errors.
- Repeating that imposed load with the stable profile discarded no application
  samples, but the monitor still contained one 21.333 ms gap. Larger buffers
  mitigate the test; they do not establish guaranteed gap-free system output.
  The origin of that remaining isolated monitor gap was not separately isolated.

The application sample loss and monitor silence duration are different
measurements; resampling and backend buffer scheduling prevent treating them
as the same count.

Also captured ares 147 (Ubuntu `147+dfsg-3`), jgenesis 0.15.0 and BlastEm 1.0.0,
55 seconds each. Seven matched five-second music windows were compared with
Kega (35 seconds of analysis, not every sample of the entire track).
Their default output is filtered, unlike this Kega filter-off comparison:

| Output | 4–8 kHz energy relative to 0.5–2 kHz |
| --- | ---: |
| Kega actual Qt output | −11.98 dB |
| ares | −16.35 dB |
| jgenesis | −16.16 dB |
| BlastEm | −17.07 dB |

These are descriptive spectral measurements, **not accuracy scores**. Overall
levels also differed; no subjective listening ranking is inferred. ares and
jgenesis were captured concurrently; BlastEm and Kega separately. This virtual
run itself contained 126 short zero intervals in ares and one in jgenesis,
versus none in BlastEm. Those external capture/host artifacts were not diagnosed
as core defects, and the ares capture is unsuitable as a continuous timing
reference. No cross-emulator null test or original-hardware verdict is claimed.

jgenesis previously documented audible Batman differences and changes to its
filtering: [issue #108](https://github.com/jsgroth/jgenesis/issues/108) and
[the v0.8.2 notes in its changelog](https://github.com/jsgroth/jgenesis/blob/v0.15.0/CHANGELOG.md).
This provides a concrete reason not to equate differences between default
emulator outputs with a Nuked regression. Capture identities and matched-window
measurements are retained in the accompanying JSON; copyrighted audio is not
committed.

## Scope and reproduction

The probe runs the actual `EmulationSession`, rewind (128 MiB), CPU video
renderer, `FramePacer`, `AudioOutput`, `AudioQueue`, and `QAudioSink`. Batman USA
runs from cold boot without input at NTSC-U 59.922743 Hz, Nuked YM2612, default
FM/PSG gain, audio filter disabled, 44.1 kHz stereo S16. Tests used Qt 6.10.2 and
the WSLg PulseAudio RDP sink; the probe's stream was muted after its measured
PCM writes. It does not exercise GPU rendering, the full window interface,
Windows WASAPI, or Voicemeeter. Artificial stalls model GUI unavailability.

Diagnostic counters and hashes are compiled **only** into the explicit probe;
normal application builds have no new counters, hashing or polling. The
existing queue/output tests also pass. The probe is not part of automatic CTest.

```sh
cmake --build build/linux --target audio_load_probe
build/linux/bin/audio_load_probe CORE ROM OUTPUT_JSON 10800 0 0 0
# Arguments after frame count: GUI stall ms, CPU burner count, latency profile.
# Profiles: 0 normal, 1 low, 2 stable. Optional final volume defaults to zero.
# Restrict the CPU-loaded test to two logical CPUs, e.g. taskset -c 6,7:
build/linux/bin/audio_load_probe CORE ROM OUTPUT_JSON 10800 0 2 0
build/linux/bin/audio_load_probe CORE ROM OUTPUT_JSON 2400 60 0 2
```

## Consequence

The stable profile is an available mitigation for the tested 60 ms stalls,
at the cost of latency. It is not a guarantee against arbitrary stalls.
The architectural fix to evaluate separately is servicing the audio sink
independently of the GUI event loop, with bounded queues and explicit shutdown
and device-change handling. Changing synthesis, emulated bus timing, or adding
an inaccurate audio mode would not address this mechanism. No production
output behavior or core accuracy was changed in this investigation.
