# Kega Refusion

- Work in this WSL project. Windows test builds go to `D:\emulation\Sega Mega Drive\emu\KegaRefusion\testbuild`.
- Keep architecture/core synchronization with the lead agent. Independent UI/build tasks may be delegated to Sol at xhigh when requested by the user.
- Keep build outputs, downloaded dependencies and scratch files under ignored `build/` or external caches. Remove obsolete scratch artifacts after work; preserve source, useful incremental builds and user data.
- Never delete or overwrite user ROMs, BIOS files, settings or saves during packaging.
- Publish Windows test builds and Linux archives only at the end of the requested batch, not after each game. Internal incremental builds for focused verification are still allowed.
- Keep the UI compact. Do not present unimplemented features as available.
- One logical libretro session is owned by the emulation thread. Link Cable may own one additional isolated core module and cooperative context; ordinary sessions remain single-instance. Destroy the peer, contexts and paired history on disable/unload/leave. Qt GUI/audio objects live on the GUI thread.
- Changes to states/input/timing require real-core integration checks. Use generated original ROM fixtures; never commit copyrighted game ROMs.
- After context compression continue the last unfinished task from confirmed state, files and verification results. Status questions do not cancel the task.


## In-game OSD borders

- Use a thin gray border for messages that expire on a timer, including quick save/load confirmations, errors and the mouse-capture hint. The slot picker also uses a gray outer border, with turquoise highlighting the selected slot.
- Use a thin turquoise border while a status is active, such as pause, fast-forward, rewind, mute, recording, playback, loading or waiting for a peer. The FPS counters and game timer use this border while displayed.
- Use a thicker turquoise border for achievement cards, including game recognition, unlocks and mastery. An error fetching achievements uses the ordinary gray message.
- Keep controller diagrams and crosshairs free of an enclosing OSD panel.

## Resource discipline

- Loading must acknowledge user actions immediately and keep the UI responsive. Never delay game start, frames or controls for optional network services such as achievements; report readiness/failure asynchronously, once, without a modal interruption.
- Avoid unnecessary disk I/O: compare save data in memory, write only changes, coalesce bursts, and bound write frequency. Keep one previous battery-save backup beside the primary file.
- Keep queues, caches, decoded media and rewind history bounded. Prefer streaming and reuse existing buffers; do not decode or retain entire media unnecessarily.
- Do not add heavy work, file reads/writes, or avoidable allocations to per-frame/input/audio paths. Poll or recompute only when needed; measure concrete regressions.
- Apply these requirements to existing features as well as new ones. Clean task scratch outputs while preserving useful incremental builds and user data.
- After a completed, verified change, commit and push it to the project's private repository.

## Input recording compatibility
- Preserve the recording compatibility contract across compatible rebuilds/optimizations. Change it deliberately when native state layout or deterministic execution changes; retain old file readers where feasible.
- Core-version mismatch is advisory: show the recorded and current version/build and attempt playback/resume. Do not reject solely because a core binary or compatibility ID differs. Validate file bounds/checksums, game identity and the actual native-state decoder independently.
- Save the actual post-power-reset region frame rate in recording metadata. OSD time derives from recorded frames/rate, not wall-clock time or an assumed 60 Hz; legacy files use the restored core rate.

## Audit completion and evidence
- Keep a per-ROM ledger for collection audits. Inventory, database recognition and startup smoke are distinct from individual mapper/protection analysis.
- Never claim all ROMs or all groups are fully reviewed because representative samples passed. Report exact coverage and remaining entries.
- Continue authorized batch work until every requested unique ROM has evidence-backed disposition and confirmed core defects are resolved, or explain a concrete external blocker without claiming completion.

## Release compatibility

- Follow [release versions, assets and update notes](docs/releasing.md) for every release. The CMake project version is the single source; the tag is `vX.Y.Z` and packaged applications must report `X.Y.Z` on both platforms.
- Keep the updater's binary asset contract: `KegaRefusion-X.Y.Z-windows-x86_64.zip` and `KegaRefusion-X.Y.Z-linux-x86_64.tar.gz`, each with a matching `sha256:<64 hex digits>` GitHub asset digest. Include corresponding `KegaRefusion-X.Y.Z-source.tar.gz`, `SHA256SUMS.txt` and `update.json`. Generate the fifth asset with `tools/release-update-metadata.py` from the authenticated draft release ID after the first four uploads; regenerate after body/binary changes and publish only with all five assets. Runtime discovery uses the public latest-release redirect and tagged `update.json`, without REST quota or tokens.
- The updater displays the published release body directly. Write concise, readable Markdown bullets about this version's completed changes; avoid a repeated project overview, long boilerplate or large tables. Record required upgrade actions and material limitations clearly.
