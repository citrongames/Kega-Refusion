# Independent audio output and Low-pass

Implemented on 2026-11-22 following the [output-load investigation](audio-output-load.md).

`AudioOutput` remains the GUI-facing object. It owns one joined `QThread` and
an `AudioOutputWorker` whose timer, sink, stream writes, reopen retries and
volume changes run independently of GUI events. The emulation producer still
passes PCM directly into the bounded queue under a mutex; it never queues a
copy per frame to the GUI. Wakeups remain coalesced. Clock feedback still
reaches the session directly, with the existing generation checks and pacer.
No new resampling, filter, audio delay or core timing change is involved.

Qt global multimedia initialization and device enumeration remain on the
application thread. Discovery sends device snapshots to the worker. Initializing
all multimedia discovery from a transient worker exposed a Linux plugin/TLS
cleanup crash during the lifecycle test, so that approach was rejected.
The final ownership keeps the global backend lifetime as before while moving
the latency-sensitive sink servicing.

Controls retain the same device fallback and latency profiles. A missing saved
device uses the default without erasing the preference. Same-device snapshots
do not reopen the sink. Shutdown closes PCM ingress, releases the sink on its
own thread, stops its timer and joins the thread; direct producer access cannot
race worker destruction. Errors from an obsolete device generation cannot trim
PCM belonging to a replacement or delay its retry.

## Verification

- Focused output lifecycle test: audio worker makes progress with no GUI event
  processing, producer invalidates feedback immediately, controls update,
  shutdown races a producer safely, and eight immediate create/destroy cycles
  terminate. Native tests pass without thread-affinity warnings.
- Private system-output captures: 1,200 Batman & Robin USA frames each, standard
  profile, no input, Nuked, NTSC-U, 44.1 kHz/S16 into Qt; monitor captured at
  48 kHz. Baseline and 34 GUI delays of 60 or 250 ms each discarded **zero**
  PCM bytes and had **zero** interior all-zero monitor gaps of at least 4 ms.
  Each accepted all 3,532,548 bytes, matching previous core PCM SHA-256
  `cb0c0ff3ede29037b02d9dea63ad29d869b05434feb683310c59fae871a12b7d`.
  Before the change, the 60 ms scenario lost 232,648 bytes and captured 34 gaps
  totaling 1.429 seconds. Startup/tail silence is excluded by the same metric.
- Initial 10,800-frame combined load (two CPU burners on two CPUs, 60 ms GUI
  delays, simultaneous compilation) preserved the raw core stream but dropped
  3,316 output bytes (829 stereo frames, 18.8 ms) cumulatively. This heavier
  run is retained rather than hidden; independence from the GUI cannot promise
  immunity to arbitrary scheduler/backend starvation.

- Controlled repeat of the same 10,800-frame CPU/GUI stress without concurrent
  compilation: zero dropped bytes, zero write errors; all 31,792,936 bytes
  accepted with SHA-256 `7c9ec57ed3df46a1b309a1d5afc640daea2d9925ef84b6742b79edaf298424ad`,
  identical to the raw core and earlier normal-speed run. No drop events occurred
  during 355 injected GUI delays over 180.465 seconds. This does not independently
  identify the timing of the initial run's loss.

The exact report is [audio-worker-results.json](audio-worker-results.json).
These captures cover Linux/WSL and a virtual system output, not physical
Windows WASAPI or Voicemeeter. The Windows Release application is cross-built
for user testing. Core binaries are checked unchanged by SHA-256.

## Low-pass setting

Settings → Sound now exposes the existing core Low-pass filter, off by default.
`audio/lowPass` persists the choice. It is applied on the emulation thread on
the next frame without reset, history clearing or game reload. The tooltip
explains the softer high frequencies associated with Mega Drive Model 1's
analog output. Existing kernel/filter coefficients are unchanged; no EQ or
new automatic filter has been added.

The filter is a local output preference across movie and netplay configuration
changes. A generated original PSG test ROM confirms live high-frequency
attenuation, restoration when disabled, unchanged sample/video rates and
preservation of local selection through movie configuration prepare/restore.
The focused native `core_integration` test passes. Russian and English labels
are included.

Windows Release build was packaged to the user's `testbuild` directory and
the deployed EXE, core and both language files matched the staging bytes.
EXE SHA-256: `ab0411a6fa8f6370a87b98e6abaaeed041b70af3ae4e7da8228af58abbf6cd0d`.
No Windows program was launched for validation. Disposable package staging
was removed; user settings, saves and games were retained.
