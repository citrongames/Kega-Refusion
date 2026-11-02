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


## Resource discipline
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
