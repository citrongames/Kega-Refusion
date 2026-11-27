# Lua scripting implementation plan

Agreed for preservation on 2026-11-21, following research into FCEUX, Mesen 2,
BizHawk, Gens-rr and mGBA. Baseline: release 0.29.0, repository 9000292.
Implementation of Phases 1–3 was authorized on 2026-11-27. On the same day the
user approved continuing Phase 4 with a native API and bilingual documentation,
explicitly dropping compatibility adapters. Undelivered capabilities remain
planned work and are not advertised as shipped functionality.

## Progress and direction

- [x] Research reference APIs and inspect the current host architecture.
- [x] Preserve the proposed architecture, delivery order and integration rules.
- [x] Phase 1: runtime, script window, read-only memory and drawing.
- [x] Phase 2: input automation, frame control and temporary save objects.
- [x] Phase 3: memory/register editing and debugger integration.
- [x] Phase 4: general script files, recording control, custom panels and declared timeline data.
  - [x] General asynchronous file access and native input-recording control.
  - [x] Custom panels with controls and graphs.
  - [x] Explicit script-data restoration with save/load and local rewind.
  - [x] Bilingual native API reference and practical examples; keep these current with each batch.

Agreed baseline: one embedded Lua 5.5 library and a native versioned API.
Do not implement Gens/FCEUX compatibility adapters. Existing third-party scripts
are references for useful capabilities and can be ported against our documented API. Pin a concrete
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

Documentation must describe exact names, argument/return types, defaults, errors,
limits, event order and timeline behavior in English and Russian. Package both
guides under executable-adjacent docs. Examples must use only shipped APIs;
include concise working recipes suitable for humans and code assistants. Do not
invent compatibility aliases or imply that game-specific memory layouts are
portable across ROM revisions.

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

Default to drawing after shaders. The global OSD switch affects only emulator indicators, not script overlays.
Stopping a script removes its output. Keep built-in crosshair behavior unchanged.
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
| Save/load and rewind | Restore explicitly declared script data before the corresponding Lua callback. The script can redraw using the restored value. |
| Internal preview/replay | Distinguish normal execution from rewind preview, export and network replay. Do not accidentally duplicate input mutations, logs or file output during internal reconstruction. |
| Netplay | Permit local read-only observation/drawing. Authoritative reset, load and remote rewind reinitialize declared local data. Shared script mutation remains separate work that requires a verified rollback contract. |
| Achievement Hardcore | Preserve existing debugger restrictions; scripting must not bypass them. |

Declared data is bounded and tied to the precise script source, its declared
ID and schema version. Each snapshot carries the data needed to restore that
point in the local timeline. Rewind data uses independent Lua and core
compression within one memory budget. A coroutine that resumes after its
own temporary-state load reads the restored value through `timeline.get()`.
Its previous local copies remain in the running environment. Frame-bound
writes and cycle-level mutations have separate acceptance requirements.

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
4. **Extended native tools.** General asynchronous files and input-recording
   control, small script panels, explicit script-data restoration, and then any
   separately designed network or recording mutation support. Do not treat deferred items as available merely
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
recording control and file access are not shipped yet. The user subsequently authorized native file and recording work on 2026-11-27.
This scenario is one example, not a restriction on the general file API.

## Research references

- [FCEUX Lua functions](https://fceux.com/web/help/LuaFunctionsList.html): frame
  control, memory, drawings and save callbacks.
- [FCEUX example scripts](https://fceux.com/web/help/OverviewofIncludedScripts.html).
- [Mesen 2 current API definitions](https://github.com/SourMesen/Mesen2/blob/master/UI/Debugger/Documentation/LuaDocumentation.json): CPU/domain selection, memory callbacks and draw surfaces.
- [Mesen legacy Script Window](https://www.mesen.ca/docs/debugging/scriptwindow.html): editor, completion, log and external-editor workflow. This older UI reference is not the current Mesen 2 API specification.
- [BizHawk Lua API](https://tasvideos.org/Bizhawk/LuaFunctions): memory domains,
  event callbacks, forms and explicit compatibility differences.
- [Gens-rr Lua documentation](https://github.com/TASEmulators/gens-rerecording/blob/master/Gens-rr/Output/LuaSamples/gens-lua.html).
- [Sonic Gens HUD](https://github.com/flamewing/sonic-gens-hud): a reference for
  useful game tools; compatibility adapters were explicitly removed from scope.
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
read-only scripts. Mutations and input override were delivered in later phases; compatibility
adapters were subsequently removed from the plan.

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
and custom panels were future work at this milestone; compatibility adapters
were subsequently removed from the plan. Focused tests
exercise the real-core/session pipeline and branch replay; performance numbers
and exact tested scope are recorded in verification.md.

## Phase 3 delivered — 2026-11-27

Lua can queue supported 1/2/4-byte physical memory writes and register edits at
full frame boundaries. Writes reuse native writable-domain/register validation
and cache/device invalidation. Effective changes clear rewind history and
temporary script saves, including already queued old save loads. Recording,
playback, netplay, Hardcore and export cannot accept script mutations.

Selected read/write/execute callbacks share the existing native CPU connection
with the debugger. Subscription masks and merged address ranges are rebuilt
only when the subscription set changes. No access observer exists without
subscriptions. Basic HUDs still use non-invasive inspection. Explicit CPU
callbacks run before debugger suspension and cannot mutate, yield or replace
bus values. Read/write events describe completed bus operations; execute is
before opcode fetch. Reset initialization, internal previews/replay and forced
debugger-frame completion suppress observation. Normal debugger Continue
preserves observation in the interrupted frame.

Callbacks have per-frame instruction/native/time budgets, 32 invocations per
script and 128 globally. They are tools for selected observations, not an
unbounded full instruction trace. The first truncated frame produces one
console notice per script run; memory.hookstats() explicitly reports truncation.
Use the native trace window for exhaustive traces.

Temporary script labels and RAM Watch rows appear in the existing windows.
Labels pin available physical code-bank identity when declared; both linked
consoles are distinguished. Script metadata has independent limits, disappears
on stop/fault/game change and never modifies the saved debugger project. Watch
sampling uses the existing window-visible inspection request, not new polling.

Focused validation covers native-event ordering, overlap, coexistence with
breakpoints/trace/coverage, error and stop cleanup, link A/B isolation, mutation
policy, deferred commands and UI persistence. Packaged fixture-debugger.lua
uses an original generated cartridge. Detailed results and two-game normal/fast
measurements are recorded in verification.md. Panels, file output, movie control and declared script-data restoration
remained Phase 4 work at this milestone. Gens adapters were subsequently removed.


## Phase 4A delivered — 2026-11-27

The user chose the native API and comprehensive English/Russian documentation
instead of emulator compatibility adapters. Both references now ship under
executable-adjacent docs. Settings > Folders includes Scripts, with a portable
default beside the executable, shared relative overrides and platform-specific
absolute overrides.

General script files use storage.read/write/result, not a measurement-specific
format. A lazy serial worker performs reads and atomic replacement. Each source
has a separate portable identity under scripts-data; requests/results are bounded,
abandoned runs cancel pending work, and no file reads/writes occur inside Lua
callbacks. Bounded string slices can inspect file data without rescanning it.
The script decides its data format and when a write is necessary.

The movie library uses the native .krm implementation for status, record, play,
resume, branch, stop and takeover. File preparation/parsing and explicit stop
writes run outside emulation. Applying native core/media configuration and capture
still happen at the existing safe boundary. Stopping a recording detaches a
bounded tape, so gameplay continues while it is saved. Failures retain the tape
for retry. New Lua recordings cannot overwrite an occupied filename. Existing
GUI Stop uses the same detached-save path; ordinary game close retains its
existing final save path.

Examples demonstrate a generic file round trip and native recording/playback
with a separately stored input-activity graph. The latter uses actual recording
refresh, identity and frame position. It is a controller-poll estimate, not a
universal game-update measurement. Source identity and graph parsing are tested
through the shipped script itself. Exact evidence is in verification.md.

At this milestone, custom panels and declared timeline restoration remained
Phase 4 work.

## Phase 4B delivered — 2026-11-27

The native ui library creates separate, nonmodal Qt windows: vertical groups,
horizontal rows, labels, buttons, checkboxes, text/numeric inputs, selectors and
small graphs. Descriptors use layout rather than fixed pixel coordinates. Qt
supplies platform theme, fonts and DPI scaling. Opening/focusing a panel or its
selector does not engage background pause; typed keys stay out of game input.
The existing auxiliary-window policy disables minimize/maximize.

Lua owns a bounded value model on the emulation thread. The GUI only consumes
coalesced snapshots (at most 20 Hz) and sends bounded typed events. There are no
per-panel timers or native controls in the game's OpenGL overlay. Limits cover
windows, controls, nesting, strings, graph points and pending events. Unchanged
values do not publish snapshots; widget trees remain intact across value updates.

UI callbacks run between completed core frames, including on ordinary pause.
They cannot yield, draw directly or obtain invented frame-poll statistics. The
existing Draw event can refresh a HUD after a callback. Pending debugger frames
and automatic rewind defer callbacks. Script control/memory permissions still
apply. Each handler's queued core controls are applied before the next event, so
a reset/load cannot carry subsequent queued clicks into a different timeline.

Owner/run/window/epoch identities reject stale events. Stop, restart, error or
incompatible hardware changes free owned windows and handlers. Closing detaches
the model before invoking the handler once; programmatic close sends no callback.
The GUI keeps edits pending until acknowledged and reconciles rejected requests.

Bundled tool-panel.lua works with any loaded game and demonstrates all leaf
controls, pause/resume, HUD options and a bounded input-poll graph. This estimate
is explicitly not universal game FPS. Both packaged API references document
the schema, callbacks, policy, lifecycle, limits and precision caveat.

Focused runtime, Qt widget and real-core session evidence is recorded in
verification.md. At this milestone, declared timeline restoration with saves
and local rewind remained Phase 4 work.

## Phase 4C — Declared timeline data

One bounded Lua value per script follows slot and file saves, movie-position
checkpoints, temporary Lua saves and local rewind. The emulator matches the
declaration by explicit ID, exact-source SHA-256 and schema version. A matching
script must already be running when the snapshot is restored. Missing and
incompatible entries restore the default and expose the reason to the script
before its restore callback. The emulator validates the optional saved data
before restoring the core. Lua locals, closures, stacks, widgets, files and
temporary handles remain part of the running environment.

The runtime validates and encodes a replacement at `timeline.set`. Captures
reuse encoded values. File and temporary saves embed the optional timeline
data in the same save. Files without timeline data retain the existing
format. Older files remain readable, and files with Lua data require an
emulator version that supports the attachment.

Local rewind stores Lua data in an independent compressed chain inside the
same rewind buffer memory cap. Core-state deltas can continue across changes
in Lua payload length. A session without declared data keeps the ordinary
rewind path. Size, string, depth, node and script-count limits bound work and
retained data. Lua observation runs at real frame boundaries and in the
existing restore callbacks.

The native input recording continues to carry final controls and core
checkpoints. Starting a recording resets declared Lua data. Playing or resuming
a recording from its file starts with the declared initial value. Authoritative
netplay rebases reinitialize local data. Netplay participants keep their own Lua
environments, and rollback reconstruction skips Lua execution.

The packaged English and Russian references document the API and limits.
The `timeline-counter.lua` example uses declared data for the frame and
input-poll counters and reads the restored value when redrawing. See
[verification.md](verification.md) for focused integration and performance
evidence.


## Example pack delivered — 2026-11-27

The current package replaces the initial standalone samples with eight numbered,
bilingual examples and two game tools. [English guide](lua-examples.md) /
[Russian guide](lua-examples.ru.md) cover drawing, controllers, memory watches,
events, a reversible input sequence, declared timeline counters, input recording
with measurements, and native tool windows.

Sonic 1 Lab targets the verified original REV00 cartridge: movement, collision
probes and profiles, touch boxes, an object inspector, boss fields, bounded trails
and comparison attempts. Contra Combat Lab verifies the original USA/Korea code
layout: live collision tests, players and projectile lists, verified HP above the
corresponding hitboxes, and one temporary practice checkpoint. Unsupported revisions get
an explicit message; the tools do not pretend to decode arbitrary hacks.

Lua overlays are independent of the emulator OSD switch. Stopping the script
removes its output. All bundled in-game text has a dark background. Scripts read
emu.language once for English/Russian captions. No compatibility layer for
another emulator was added.

See the 2026-11-27 example-pack entry in [verification](verification.md) for exact
native-game coverage, UI checks and costs. Game-specific routines not exercised
through gameplay are distinguished from source-derived support in verification notes.
