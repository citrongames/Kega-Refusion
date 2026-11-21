# Debug project and memory watch verification

Verified on 2026-11-21 against `76a4131`. The native core is the same build
used for physical code/data coverage.
[Usage](debugger-projects.md) · [Machine-readable evidence](debugger-projects-verification.json).

| Linux check | Evidence |
| --- | --- |
| Project file | UTF-8 text, A/B logical addresses, all 64 ignored-hit bits and watch definitions round-trip. Runtime hit counts start at zero. Strict file, entry, type and address limits reject malformed and oversized input. Failed loads keep the destination, and rejected saves preserve the previous file. |
| Real-core watches | The original Mega Drive program returned exact 1-, 2- and 4-byte values and a safe VDP register mirror without advancing the clock or changing serialized state. Invalid spans, widths, domains and missing peers return unavailable samples. Actual linked Game Gear RAM values stay distinct at the same offset on A and B. |
| Real-core breakpoint configuration | A real one-shot stop populated a runtime counter. Rejected configurations preserved the active list and reported their request ID and error. Project replacement applied the requested enabled flags with fresh hit counts. A command for a previous game was rejected. Validation with the debugger detached left it detached. |
| Qt offscreen | The watch dialog applied signed, hexadecimal and binary formats, byte order, change highlighting, history reset and physical navigation. An unchanged snapshot emitted zero watch model change notifications. Tests covered the Memory window watch action and CPU project application, logical symbols, target capabilities and Link Cable. |
| Full application | Final Linux and Windows application builds passed. All 73 staged Windows files matched their deployed SHA-256 hashes in testbuild. Windows runtime was left to the user. |

An isolated host measurement took **14.817 µs per snapshot** across 1,000
snapshots of 128 four-byte watches on the Mega Drive fixture. Each request
asked for no table page and no watch catalog. The payload was 512 guest bytes
per snapshot. This measurement covers host requests and snapshot destruction.
The visible UI normally schedules requests at 100 ms intervals.

The final CPU-window rerun passed, including cancellation during unload,
late acknowledgement rejection and error display while the CPU window is
hidden. The same native core and firmware were used throughout. Bank-qualified
symbols, pixel provenance and profiling remain deferred.

Local logs are under `build/verification/debugger-project`.
