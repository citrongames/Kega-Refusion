# Lua scripting implementation plan

Agreed for preservation on 2026-11-21, following research into FCEUX, Mesen 2,
BizHawk, Gens-rr and mGBA. Baseline: release 0.29.0, repository 9000292.
Implementation of Phases 1 and 2 was authorized on 2026-11-27. The remaining phases
remain planned work; they are not advertised as shipped features.

## Progress and direction

- [x] Research reference APIs and inspect the current host architecture.
- [x] Preserve the proposed architecture, delivery order and integration rules.
- [x] Phase 1: runtime, script window, read-only memory and drawing.
- [x] Phase 2: input automation, frame control and temporary save objects.
- [ ] Phase 3: memory/register editing and debugger integration.
- [ ] Phase 4: custom panels, Gens compatibility and advanced timeline integration.

Recommended baseline: one embedded Lua 5.5 library, a native versioned API,
then explicit compatibility adapters, starting with Gens. Pin a concrete
upstream release during implementation and include corresponding sources and
notices in releases. Do not require a system Lua installation or raise the
existing Windows/Linux distribution baseline. LuaJIT is not an initial dependency;
consider it only if measured workloads justify it.

The recommended UI includes a collapsible editor. The alternatives discussed
were an external-editor-only console and complete imitation of another
emulator's API. Neither full API compatibility nor every advanced operation
is promised by the initial implementation.

## Intended capabilities

- Draw text, lines, rectangles, circles, transparent fills and cached images.
- Read values and bounded ranges, select CPU or physical memory domains,
  inspect registers, and later edit supported values and subscribe to accesses.
- Read actual input and override selected controls; support the existing
  logical topology, including multitaps, J-Cart, guns, mouse and paddle.
- Advance frames, pause, reset, use reusable in-memory save objects and automate
  sequences of actions without writing a save file every frame.
- Build small script tools with buttons, checkboxes, values and graphs later.

Hitboxes, object positions and game-specific semantics belong in scripts.
The emulator must not pretend to infer these universally from arbitrary RAM.
Initial examples should include a position/speed HUD, hitbox display for one
identified game/revision, and a simple input automation script as Phase 2 lands.
Do not bundle copyrighted ROMs with examples.

## Shared runtime architecture

EmulationSession remains the sole owner of the running LibretroHost and its
optional linked peer. Use one independent Lua environment per active script,
owned and executed on that emulation thread. Do not add another emulated console
or separate Lua implementation per hardware platform.

Reuse and extend the existing inspection, execution, trace and address-mapping
interfaces. Expose stable instance, CPU, domain and device identifiers through
runtime capability discovery. Mega Drive, SMS, GG, SG-1000, CD, 32X, CD+32X,
MegaLD, Firecore and special cartridges expose only operations actually provided
by their current hardware. Linked consoles use explicit A/B context.

Keep scripts independent of the inspector windows' 10 Hz refresh. Frame events
run at specified emulation boundaries, including when no debugger window is open.
The frame sequence must define input sampling/override, recording, core execution,
post-frame observation and presentation. A frameadvance-style script yields to
the existing scheduler; it must not recursively call the core or pump Qt events.

Qt widgets stay on the GUI thread. File loading, image preparation and log/file
output must not block a frame callback. Pass bounded commands and immutable
results between threads. A shared subscription dispatcher must coexist with
breakpoints, tracing and coverage instead of replacing their native callbacks.

Native read/write observation currently reports completed accesses; in particular,
writes are reported after reaching the bus. Mesen-style pre-write replacement
needs a separately designed native interface. Do not simulate it by claiming an
after-write callback can undo all device side effects.

## API contract

Use a small, documented API organized around emulation/events, memory, input,
drawing, save objects, recordings and logging. Exact names and signatures remain
an implementation design task; examples in the proposal are not a frozen ABI.

Memory access must distinguish logical CPU addresses from physical domain offsets,
identify console and CPU, and state width, signedness, byte order and address unit.
Reuse side-effect-free peeks for ordinary inspection. Report unsupported reads or
writes explicitly. Do not expose private core pointers or duplicate mapper logic.
Batch range reads rather than creating a full-memory snapshot for each Lua call.

Enable address hooks only for requested processors, event types and ranges.
Document callback ordering and whether an event is before or after an operation.
Prevent recursive event handling and defer unsafe lifecycle operations to a safe
boundary. Keep code/data bank identities consistent with the debugger.

Compatibility adapters must explicitly translate argument order, colors, button
names, addressing, byte order and callback timing. Identical function names in
FCEUX, Gens and BizHawk do not guarantee identical behavior. Gens is the first
candidate because useful Mega Drive scripts already exist; publish the supported
subset and limitations rather than claiming that all old scripts run unchanged.

## Drawing and presentation

Attach each immutable drawing list to its machine generation, console and exact
video frame. Publish image and script drawing together so skipped GUI updates,
fast-forward and repainting cannot display a hitbox over the wrong game frame.
Reuse the latest-frame mailbox pattern and bounded buffers; do not accumulate
all intermediate drawing lists or copy screenshots into rewind history.

Provide native-game-pixel coordinates for object overlays and window/HUD coordinates
for readable text. Use the existing video geometry transformations for aspect,
scaling, crop, DPI, fullscreen and changing resolutions. Include linked display
selection and MegaLD's composed picture in the coordinate contract.

Default to drawing after shaders. The global OSD switch hides script overlays
without stopping script logic. Keep built-in crosshair behavior unchanged.
Normal screenshots and video remain clean; including script drawings is a separate
explicit capture feature. Cache fonts/images and release script-owned resources
on stop. Do not decode images or read their files on every frame.

## Script window and lifecycle

Add a compact script window reachable through File → Scripts, with script
list, Open, Run, Stop, Restart, bounded log and a collapsible editor. The editor
should offer syntax highlighting, line numbers, API hints and error navigation,
plus an action to open the source in an external editor. Avoid a heavyweight IDE.
Support multiple scripts with separate environments and a documented execution
order. Error output should identify script and line without repeated modal dialogs.

Opening/focusing the window must not pause the game by itself. Explicit script
execution commands may pause it. Follow existing auxiliary-window conventions and
supply Russian/English interface text. Custom script panels come later and use Qt
through queued messages, not Lua calls directly into widgets.

On stop, disconnect callbacks and release drawings, saves, images and input overrides.
On game or hardware changes, invalidate old memory handles and pending results;
stop incompatible scripts with a clear reason. Reset and state-load events are
explicit. No stale reference may survive destruction of a linked peer.

## Recordings, rewind, netplay and achievements

| Scenario | Contract |
| --- | --- |
| Input recording | Record final effective controls after permitted script overrides. Playback uses those recorded controls without running the input automation again. |
| Memory mutation during a recording | Ordinary input records cannot reproduce it. Initially reject this combination clearly. A deterministic mutation journal is separate later work, not silently enabled. |
| Save/load and rewind | Restore explicitly declared serializable script data and rebuild drawings for the restored frame. Do not claim arbitrary Lua locals, closures or coroutine stacks are restored. |
| Internal preview/replay | Distinguish normal execution from rewind preview, export and network replay. Do not accidentally duplicate input mutations, logs or file output during internal reconstruction. |
| Netplay | Start with local read-only observation/drawing. Mutating scripts need a separately verified shared ordering and rollback contract before being permitted. Do not automatically distribute or run another participant's scripts. |
| Achievement Hardcore | Preserve existing debugger restrictions; scripting must not bypass them. |

Declared script data must be bounded and tied to script identity/schema. Avoid
serializing an entire Lua environment or adding unbounded per-frame blobs to the
existing compressed history. Define restart/load behavior for coroutine scripts
explicitly; arbitrary continuation rewind is not an initial promise.

Before timeline integration is complete, expose its limits rather than silently
continuing a stateful script with incorrect future counters. Read-only HUDs can
rebuild from restored game memory. Frame-bound writes, script continuation state,
and cycle-level mutations are different problems and require separate acceptance.

## Resource discipline

With no scripts loaded, allocate no Lua environments and enable no additional
CPU access hooks or per-instruction observation. Basic HUDs must not attach the
invasive debugger or force the SH-2 interpreter just to read a few values.

Bound script memory, callback work, temporary saves, drawing commands, images,
log history and queues. Interrupt runaway Lua execution and contain an individual
script error without crashing the emulator. Lua instruction hooks alone do not
bound long native calls: every exposed native operation must also be bounded or
asynchronous. Establish concrete budgets through focused measurements.

Do not add unrestricted blocking OS/network calls to frame callbacks. Define file
and module access deliberately when implementing the runtime; preserve portable
paths and avoid unnecessary writes. Share existing storage and update mechanisms
where suitable instead of adding periodic full-memory scans or redundant caches.

## Delivery and focused acceptance

1. **Runtime and visual foundation.** Script window/editor, lifecycle, read-only
   memory, frame events, drawing and HUD examples. Check matching image/drawing
   frames, DPI/fullscreen/shader geometry, failure/stop behavior and game switching.
2. **Input and automation.** Effective controller overrides, pause/reset,
   frameadvance-style scheduling, bounded reusable in-memory saves and a small
   automation example. Verify recorded input plays without the automation script,
   and restoration limits are explicit.
3. **Debugger integration.** Safe supported memory/register writes, selected
   access hooks, labels and RAM Watch integration. Verify debugger subscriptions
   coexist and mutations preserve cache/device invariants at supported boundaries.
4. **Extended tools and compatibility.** Small script panels, a tested Gens subset,
   explicit script-data restoration, and then any separately designed network or
   recording mutation support. Do not treat deferred items as available merely
   because the runtime can call Lua.

Use generated fixtures for deterministic checks and a small number of representative
real scripts/games. Cover the different hardware families and linked-console
lifecycle without repeating completed broad audits. For hot-path changes compare
no-script baseline and representative HUD/automation/hook workloads, at normal
speed and fast-forward, on distinct games/developers/regions. Record measurable
costs and limits. Windows interactive testing remains with the user.

After each authorized completed batch, update this checklist and evidence, clean
temporary artifacts, commit and push. Build delivery follows the user's requested
batch boundary; recording this document requires no new binaries.

## User reference: FPS recording and replay graph

The provided FCEUX scripts (2026-11-27), fceux_fps_record_by_rom.lua and
fceux_fps_replay_by_rom.lua, demonstrate an acceptance scenario for the remaining
work. The recorder stores per-frame controls and a lag flag in TSV, plus one
FPS sample per 60 frames. Its estimate is nominal refresh times the fraction
of non-lag frames. The replay overrides controls and plots the recorded samples;
it does not measure new FPS during playback. Input polling is an estimate of
game activity, not a universal measure of completed game updates.

For Kega Refusion, reuse native input recordings and attach measurement data
by emulated frame instead of duplicating input storage in the example. Pending
APIs include recording control and bounded asynchronous result-file access.
Nominal refresh and per-frame lag observations are available in Phase 2. Obtain refresh from the actual console/region,
not a fixed NTSC value. Define rewind/load/re-record behavior before promising
aligned historical graphs. A game-specific update counter can supply a better
metric when its meaning is known. Existing frame callbacks and drawing primitives
cover the live observation/graph portion, with a bundled live graph in Phase 2;
recording control and file access are not shipped yet. This is a recorded use case, not implementation authorization.

## Research references

- [FCEUX Lua functions](https://fceux.com/web/help/LuaFunctionsList.html): frame
  control, memory, drawings and save callbacks.
- [FCEUX example scripts](https://fceux.com/web/help/OverviewofIncludedScripts.html).
- [Mesen 2 current API definitions](https://github.com/SourMesen/Mesen2/blob/master/UI/Debugger/Documentation/LuaDocumentation.json): CPU/domain selection, memory callbacks and draw surfaces.
- [Mesen legacy Script Window](https://www.mesen.ca/docs/debugging/scriptwindow.html): editor, completion, log and external-editor workflow. This older UI reference is not the current Mesen 2 API specification.
- [BizHawk Lua API](https://tasvideos.org/Bizhawk/LuaFunctions): memory domains,
  event callbacks, forms and explicit compatibility differences.
- [Gens-rr Lua documentation](https://github.com/TASEmulators/gens-rerecording/blob/master/Gens-rr/Output/LuaSamples/gens-lua.html).
- [Sonic Gens HUD](https://github.com/flamewing/sonic-gens-hud): a candidate for
  practical compatibility work, not yet tested in Kega Refusion.
- [mGBA development scripting API](https://mgba.io/docs/dev/scripting.html):
  canvas, images and storage; development documentation, not a stable-release claim.
- [Lua versions](https://www.lua.org/versions.html) and
  [Lua 5.5 manual](https://www.lua.org/manual/5.5/manual.html): runtime,
  allocator and instruction-hook facilities. Recheck the pinned release when implementation starts.


## Phase 1 delivered — 2026-11-27

Embedded Lua 5.5.1 is pinned to the official source archive and built statically
without a system Lua dependency. The [API guide](lua-api.md) defines the native
versioned subset, limits and timeline behavior. Packages include examples,
their guide and the Lua MIT notice.

The emulation owner runs independent bounded environments; the GUI prepares
source and PNG assets asynchronously. Scripts read existing physical-domain
and logical-CPU inspection interfaces without attaching the debugger. Drawing
packets share the video mailbox and carry generation/frame/console identity.
There are no new emulated machines or CPU hooks for scripting.

The nonmodal window supplies a script list, editor, syntax highlighting, line
numbers, API hints, external-editor action and bounded logs. Source preparation
carries the original machine revision, so late results cannot start against a
replacement game. Script startup waits at a CPU breakpoint; Stop can cancel it.

Events include completed frames, reset, state loading, rewind and a separate
presentation-only draw event. Lua locals are not rewound. Internal replay
does not re-run ordinary callbacks. Predictive file-state previews and replaced
rollback pictures are shown clean until matching observations are available.
Remote rewind images are never paired with stale local memory.
Hardcore and video export reject execution; ordinary netplay permits local
read-only scripts. Mutations, input override and compatibility adapters remain
in the later phases.

Focused checks covered MD, SMS, SG-1000, GG and GG Link A/B, 32X, Firecore,
Sega CD and CD+32X using original fixtures. State bytes stayed unchanged by
inspection. MegaLD media-specific scripting and every special cartridge were
not individually replayed in this batch. Rendering checks covered raster/GL,
100%/150% scale, clean capture and OSD visibility. Linux session tests covered
step/back/load/stop, lifecycle generations and local netplay observation.
Windows was cross-compiled; interactive Windows verification remains with the user.

Core execution plus representative RAM HUD/list generation measured 2.0–4.7%
above baseline on Lion King NTSC-U and Sonic 2 PAL; no-script comparisons varied
from -1.9% to +2.8%. This is not an end-to-end GUI benchmark. Exact measurements,
hash comparisons and existing unrelated test limitations are in verification.

## Phase 2 delivered — 2026-11-27

The native API now discovers logical controllers, reads effective input and
merges per-script button/analog overrides before input recording. Playback
ignores overrides and does not dispatch input automation. Controller topology
comes from the existing host, including adapters, J-Cart, guns, mouse, paddle
and linked console players; no hardware-specific second input pipeline exists.

The main Lua chunk can yield through frameadvance to the normal scheduler.
Pause and reset are deferred boundary commands; no recursive core execution
or GUI event pumping is used. Script errors/stops discard their queued controls
and overrides. External seeks cancel suspended continuations instead of resuming
a future coroutine stack. Read-only callbacks remain available on playback and
netplay; scripting still cannot bypass Hardcore or mutate netplay.

Reusable save objects hold native machine/clock/battery and achievement progress.
During input recording they also retain a validated input prefix, allowing a
saved branch to be restored after later edits. Eight objects per script, 32 MiB
per object and 64 MiB aggregate limit retained snapshots, including pending
loads. Snapshot creation is explicit and does no disk I/O. Playback checkpoints
validate the existing tape rather than modifying it. Changes of recording
context, game or hardware invalidate old snapshot payloads. Arbitrary Lua
locals/coroutine stacks are not part of saved state.

The shipped input-automation example demonstrates 30 input frames and a temporary
restore; input-activity draws a bounded live graph from nominal refresh and
per-frame controller-poll observations. This is an activity estimate, not a
universal game-update detector. File output, Lua movie start/stop, debugger writes,
custom panels and compatibility adapters remain future work. Focused tests
exercise the real-core/session pipeline and branch replay; performance numbers
and exact tested scope are recorded in verification.md.
