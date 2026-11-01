# Kega Refusion

- Work in this WSL project. Windows test builds go to `D:\emulation\Sega Mega Drive\emu\KegaRefusion\testbuild`.
- Keep architecture/core synchronization with the lead agent. Independent UI/build tasks may be delegated to Sol at xhigh when requested by the user.
- Keep build outputs, downloaded dependencies and scratch files under ignored `build/` or external caches. Remove obsolete scratch artifacts after work; preserve source, useful incremental builds and user data.
- Never delete or overwrite user ROMs, BIOS files, settings or saves during packaging.
- Keep the UI compact. Do not present unimplemented features as available.
- The libretro core is a single process-wide instance, owned by the emulation thread. Qt GUI/audio objects live on the GUI thread.
- Changes to states/input/timing require real-core integration checks. Use generated original ROM fixtures; never commit copyrighted game ROMs.
- After context compression continue the last unfinished task from confirmed state, files and verification results. Status questions do not cancel the task.


## Resource discipline
- Avoid unnecessary disk I/O: compare save data in memory, write only changes, coalesce bursts, and bound write frequency. Keep one previous battery-save backup beside the primary file.
- Keep queues, caches, decoded media and rewind history bounded. Prefer streaming and reuse existing buffers; do not decode or retain entire media unnecessarily.
- Do not add heavy work, file reads/writes, or avoidable allocations to per-frame/input/audio paths. Poll or recompute only when needed; measure concrete regressions.
- Apply these requirements to existing features as well as new ones. Clean task scratch outputs while preserving useful incremental builds and user data.
- After a completed, verified change, commit and push it to the project's private repository.
