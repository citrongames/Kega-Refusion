# Kega Refusion Lua API 1

[Русская версия](lua-api.ru.md)

Open **File → Scripts…** while a game is loaded. Open a
UTF-8 `.lua` source file and choose **Run**. You can edit the code in the
script window or in an external editor, then restart it. The console shows
`print(...)` output and Lua errors for each script.

The file picker starts in the `scripts` folder beside the emulator, which
contains the included examples. To choose a different starting folder, open
**Settings → Emulator Settings… → Folders → Scripts**. Relative paths are
resolved from the folder containing `settings.ini`. Absolute paths are stored
separately for Windows and Linux. Both language versions of this guide ship in
the emulator's `docs` folder.

## Events and lifecycle

`emu.language` is the effective application language code (for example `"en"` or
`"ru"`) captured when the script starts. Use it for script captions.

`emu.apiVersion` is the integer `1`. Use `emu.on(event, function)` to register
one callback per event in the current script. A new callback replaces the old
one. Pass `nil` to remove a callback. Each callback receives a table with
`event`, `frame` and `instance`. The frame counter comes from the emulated
clock, and may go backwards on restore. Console IDs are `0` for A and `1` for B.
`emu.instances()` returns the current number of consoles.

| Event | When it runs |
| --- | --- |
| `"beforeframe"` | After sampling live controls, before overrides, recording and core execution. Runs during ordinary local play and recording. Drawing and frame control are unavailable here. |
| `"frame"` | After a completed frame, before presentation. Callbacks also run for completed fast-forward frames that the display skips. |
| `"reset"` | After a reset that keeps the current game and hardware. |
| `"loadstate"` | After loading a saved state with compatible hardware. |
| `"rewind"` | While presenting a restored frame. Print output is suppressed. |
| `"draw"` | When the current frame is redrawn after starting or stopping a script or selecting the presented console. The emulated clock stays at the current frame and print output is suppressed. |

Scripts run in start order in separate Lua environments. Restarting a running
script keeps its place and creates a fresh environment. An error stops the
script that raised it. Stopping a script releases its callbacks, drawings, input overrides, temporary
saves, declared timeline data, labels and watches. Game or hardware changes
stop scripts bound to an incompatible machine. Callbacks stay registered when
a script's main chunk finishes.

Reset, load and rewind clear input overrides. A frame-advance sequence waiting
to resume is stopped when the timeline changes externally. Scripts that stay
active keep their Lua environment and can use restore events to read
[timeline data](#timeline-data) or rebuild observations from game memory.
A script's own queued reset or temporary load can preserve its waiting
sequence. After it resumes, call `timeline.get()` for the restored data.
A table copied before the restore keeps its previous contents.

The emulator skips scripting during internal replay and rollback
reconstruction. Netplay permits read-only observation and drawing. Hardcore
and video export disable script execution.

## Read-only inspection

Inspect the supported hardware before choosing domain or CPU IDs. The optional
`instance` argument defaults to the presented console. Invalid argument
types, unsupported widths and limit violations raise a Lua error. An
unavailable console, domain, address or register set yields `nil, reason`.
A console with no native domain or CPU catalog returns an empty list.

- `memory.domains([instance])` gives a list of records with `id`, `name`,
  `size` in bytes, the associated `base` address, `endian` (`"be"` or
  `"le"`) and a `writable` flag. Availability, size and order can change with
  the active video mode.
  Domain offsets start at zero. The base is metadata and does not
  imply that the entire domain is mapped into the CPU's current bus.
- `memory.read(domain, offset[, width[, endian[, signed[, instance]]]])`
  reads one integer. Width is `1`, `2` or `4` bytes and defaults to `1`.
  Endian is `"native"` (default), `"be"` or `"le"`. Native means the byte
  order listed for that domain. Signedness defaults to `false`.
- `memory.range(domain, offset, length[, instance])` returns up to 4,096
  bytes in a Lua string with embedded zeros preserved. Use `string.byte`
  to interpret the result. A request that crosses the end fails as a whole.
- `memory.cpus([instance])` gives a list of `id`, `name`, `addressBits`,
  `addressUnit` (bytes per logical address), `endian` and a `hooks` table
  with the hardware capability flags `exec`, `read` and `write`.
- `memory.peek(cpu, address[, width[, endian[, signed[, instance]]]])` reads
  the CPU's logical map using the same integer options. Native order comes
  from the CPU record. Addresses are byte-based for 68000, Z80 and SH-2,
  and word-based for SVP SSP1601 program memory. A width still counts bytes.
  Addresses outside the CPU's range, unmapped bytes and regions without a
  side-effect-free peek return an unavailable result.
- `memory.registers(cpu[, instance])` returns `pc`, `sp`, `cycles` (within the
  current emulated frame) and a `values` table keyed by native register names.

The core's existing inspection API supplies the bytes. CPU access observation
is enabled only for explicit subscriptions. Available operations depend on
the active core and session.

## Editing memory and registers

- `memory.write(domain, offset, value[, width[, endian[, instance]]])`
  queues a write to a writable physical domain. Width is `1`, `2` or `4`
  bytes and defaults to `1`. The byte order is `"native"`, `"be"` or `"le"`
  and defaults to the domain's order. Values may range from the signed minimum
  to the unsigned maximum for the selected width.
- `memory.writableregisters(cpu[, instance])` returns a table of supported
  native register names and their widths in hexadecimal digits.
- `memory.setregister(cpu, name, value[, instance])` queues a write to a
  writable register. Use its exact native name and an unsigned integer that
  fits the reported width.

Writes are available at startup, in the `"frame"` callback and after
`emu.frameadvance()` resumes in local play. Input recording and playback
disable writes. A write returns `true` when queued or `nil, reason` when
unavailable. Writes share the existing 32-request control queue and take
effect at the next safe frame boundary. A read in the same callback still
sees the current state. If the core rejects a queued write, the script stops
and the console reports the error. A change invalidates rewind history and
captured temporary saves.

## CPU access callbacks

- `memory.on(cpu, event, first, last, callback[, instance])` subscribes to
  CPU accesses intersecting an inclusive logical address range. Pass `nil`
  for `last` to observe a single address. It returns a positive handle or
  `nil, reason`.
- `memory.off(handle)` removes one of the current script's subscriptions.
  It returns `true` on success or `nil, reason` for an unknown or unavailable
  handle.
- `memory.hookstats()` returns `{callbacks, truncated}` for the current
  script's last completed frame. `truncated` is true when a budget caused a
  matching event to be skipped. Read it in the `"frame"` callback or after
  `emu.frameadvance()` resumes. It returns `nil, reason` when the completed
  frame has no subscription statistics.

Subscriptions can change at startup or after a completed frame in local
play and during input recording or playback. Their callbacks receive a table
with `event`, `frame`, `instance`, `cpu`, `address`, `value` and `size`.
The event's frame number identifies the frame being executed, matching
the subsequent `"frame"` event. During a CPU callback, `emu.framecount()`
still returns the last completed frame. The event `"exec"`
runs before opcode fetch, with `value` and `size` both zero. The events
`"read"` and `"write"` report completed bus accesses. Their `value` is the
native bus value and `size` is its width in bytes. Access ranges use the
native event addresses. For example, SVP instruction addresses count words
and its data accesses count bytes.

Callbacks run in script start order and then in registration order. They
can inspect memory and registers and write bounded console output. Collect
counts or values for use in the next `"frame"` callback. Each script can
register up to 32 access callbacks, with 128 subscriptions in total. A frame
runs at most 32 access callbacks per script and 128 in total, within a shared
wall-time budget of approximately 4 ms. Additional matching accesses are
skipped when the budget is exhausted. The console reports the first such
truncation per script run. An error retires the affected script at a frame
boundary.

## Debugger labels and RAM Watch

- `memory.label(cpu, address, name[, instance])` adds a temporary label to
  the CPU debugger. Use the CPU's logical instruction address, with the same
  address units as `memory.peek`. Where the native mapping is available, the
  label follows the bank resolved when the script declares it.
- `memory.watch(domain, offset, width, name[, instance])` adds a temporary
  RAM Watch row. Use a physical domain offset and width `1`, `2` or `4` bytes.
  The row displays an unsigned integer in the domain's byte order.
- `memory.unlabel(handle)` and `memory.unwatch(handle)` remove that script's
  declarations. Creation returns a positive handle. Removal returns `true`.
  An unavailable address, full list or unknown handle returns `nil, reason`.

Declare entries at startup or after a completed frame. Names are nonempty
UTF-8 strings of up to 128 bytes. Each script can declare up to 32 labels and
32 watches, with 128 labels and 64 watches in total. The CPU debugger and
RAM Watch mark the entries with `(Lua)`. RAM Watch samples its displayed
values while its window is open. Temporary entries disappear when the
script stops and project files contain only the user's saved entries.

## Frame observation and control

- `emu.framecount()` returns the current emulated frame count, which follows
  the game clock across state loads and reset. Before core execution, the
  count still identifies the last completed frame.
- `emu.fps()` returns the nominal refresh rate of the current console and
  region, in frames per second.
- `emu.lagged()` returns `true` for a completed frame without a controller
  poll and `false` for one that polled. It is available in the `"frame"`
  callback and after `emu.frameadvance()` resumes. A core without a poll
  indicator or a call outside those contexts yields `nil, reason`. Polling
  measures input activity and is only a proxy for game updates.
- `emu.pause([paused])` requests pause when the argument is `true` or
  omitted, and resume when it is `false`.
- `emu.reset([hard])` requests a soft reset by default or a power reset
  with `true`. Finish any active input recording or playback before resetting.
- `emu.frameadvance()` suspends the main chunk until the scheduler completes
  one real frame. It returns `true` after the `"frame"` callback for that
  frame. A paused game stays paused after the step. Use this operation in the
  main chunk or a function called by it. Event callbacks cannot yield.

Pause and reset accept requests during startup and after a completed frame.
Scripts share a queue of up to 32 control requests.
A request returns `true` when queued, or `nil, reason` when unavailable.
An operation that fails after being queued stops the requesting script and
reports the error in its log. The scheduler completes pending reset or load
requests at a safe boundary before a subsequent frame advance. Malformed
arguments raise a Lua error. The `"reset"`, `"loadstate"`, `"rewind"` and
`"draw"` events allow observation and drawing only.

## Controller input

- `input.devices()` lists connected logical devices as records with `player`
  (one-based logical player), `instance` (console ID), `kind` and `buttons`
  (an array of valid button names). The list follows the configured topology,
  including additional players on adapters and J-Cart. For Game Gear Link,
  players 1 and 2 belong to consoles 0 and 1 respectively.
- `input.get(player)` returns a table of button booleans and any analog
  fields. In `"beforeframe"` it reads the sampled live input. In `"frame"`
  and after a main chunk resumes from `emu.frameadvance()` it reads the
  effective controls used for that frame. With no sample or device available,
  it returns `nil, reason`.
- `input.set(player, controls)` updates that script's override. Pass named
  fields such as `{left = false, right = true}`. Unspecified fields retain
  any existing override from the script or fall through to the sampled input.
  An override set in `"beforeframe"` affects the frame about to run. Startup
  and post-frame changes take effect on the next frame.
- `input.clear([player])` releases overrides for that player, or for all
  players when omitted.

Overrides stay active until cleared, the script stops or the timeline changes.
The last active writer in script start order wins for each field. Recordings
store the resulting effective input. Playback uses the stored input and skips
`"beforeframe"`. `input.set` and `input.clear` are available in startup,
`"beforeframe"` and after a completed frame in ordinary local play or
recording. Mutating controller calls return `true` on success or
`nil, reason` when unavailable. Read-only frame callbacks can observe playback
or netplay. Local pause and stepping can also control playback while frames remain.

| Device kind | Buttons in addition to directions, where present | Analog fields |
| --- | --- | --- |
| `md3` | `a`, `b`, `c`, `start` | |
| `md6` | `a`, `b`, `c`, `x`, `y`, `z`, `start`, `mode` | |
| `sms` | `button1`, `button2`, `pause` | |
| `sg1000` | `button1`, `button2` | |
| `gg` | `button1`, `button2`, `start` | |
| `lightphaser` | `trigger`, `reload` | `x`, `y`, `offscreen` |
| `menacer` | `trigger`, `reload`, `auxA`, `auxB`, `start` | `x`, `y`, `offscreen` |
| `justifier` | `trigger`, `reload`, `start` | `x`, `y`, `offscreen` |
| `mouse` | `left`, `right`, `middle`, `wheelDown` | `x`, `y` (relative motion) |
| `paddle` | `button` | `x` (absolute position) |

Directional fields on pads are `up`, `down`, `left` and `right`. Gun axes
are absolute normalized screen coordinates from −32,767 to 32,767, with the
minimum at the left or top and `offscreen` as a separate boolean. Mouse
deltas and paddle position use integers from −32,768 to 32,767. A persistent
mouse delta is applied each frame until cleared or replaced.

## Temporary save objects

- `savestate.create()` returns an integer handle owned by the current script.
- `savestate.save(handle)` captures the current completed machine state.
  It replaces that handle's previous snapshot and is limited to one capture
  per script at startup or per completed frame.
- `savestate.load(handle)` queues a restore at the next safe boundary, then
  invokes the `"loadstate"` callbacks. Follow it with `emu.frameadvance()`
  to resume the main chunk after running one frame from the restored state.
- `savestate.free(handle)` releases the object and invalidates its handle.

Temporary saves live in memory. Each script can have eight objects. A captured
object can use up to 32 MiB, with a 64 MiB combined budget across scripts.
Replacing a populated object must fit the combined budget while the old snapshot
is still retained; free it first if no replacement fits. Pending loads remain charged
until the scheduler releases their snapshot.
The save functions are available during startup and after a completed frame
in local sessions. Capture, load and free return `true` on success or
`nil, reason` when unavailable. A load can fail after queuing, as with
other deferred operations.

During an input recording or playback, a temporary save includes the
recording's input checkpoint. Restoring a checkpoint while recording starts a
branch from that saved prefix. Power reset and changes to recording mode,
hardware, controller topology or cheats can discard snapshots. A restore
recovers the machine, its clock and matching [timeline data](#timeline-data)
before the `"loadstate"` callbacks. The requesting sequence can reuse its
handles and call `timeline.get()` after it resumes. Restore clears input
overrides.

## Timeline data

Use `timeline` for a small value that should follow the emulated timeline, such
as a counter or the current sample of a measurement. The emulator saves the
value alongside machine state. Locals, closures, coroutine stacks, drawings,
windows and widget values, file requests and temporary save handles remain
part of the running script.

- `timeline.define(id, schemaVersion, initialValue)` declares one value per
  script, during initial startup before the main chunk yields. The ID uses
  1–64 printable ASCII characters and must be unique among running scripts.
  The schema version is an integer from 1 to 2,147,483,647.
- `timeline.get()` returns the current value. Each call produces a fresh copy
  of any table. A declared `nil` is returned as a single `nil`.
- `timeline.set(value)` validates and encodes a replacement immediately.
  Editing a Lua table affects the stored value only after a successful
  `set`. Call it from the main chunk, `"beforeframe"`, `"frame"` or a
  script window handler. Restore, Draw and CPU access callbacks can read
  the value.
- `timeline.status()` returns one of the strings below.

| Status | Meaning |
| --- | --- |
| `"current"` | The value was just declared or set by the script. |
| `"restored"` | A matching saved value was restored. |
| `"missing"` | The restored state had no entry for this ID. The initial value is active. |
| `"incompatible"` | The saved entry had a different source or schema version. The initial value is active. |
| `"reset"` | A reset restored the initial value. |

Successful declarations and updates return `true`. A duplicate declaration
or ID, an unavailable call context, or access before declaration returns
`nil, reason`. Invalid arguments, unsupported values and limit violations
raise a Lua error. Pass `nil` explicitly when it is the intended value.

Values may contain `nil`, booleans, 64-bit integers, finite floating-point
numbers, binary strings and tables with integer or string keys. Tables must
be acyclic and have no metatables. Shared tables are copied by value at each
occurrence. The root counts as depth one. A value is limited to eight levels
and 512 nodes, counting the root, every table key and every table value.
Each string is limited to 4,096 bytes. The encoded value can use up to 16 KiB
per script, with at most eight active scripts. The usual native call and byte
budgets apply. Keep values small in frame callbacks. Captures reuse the
encoded value until it changes.

Save slots and files, saves tied to an input-recording position and Lua
temporary saves carry these values inside the save. Opening a file with
timeline data requires a version of the emulator that supports it. This
version also reads earlier saves. Saves without timeline data use the
existing file format and restore the initial value with status `"missing"`.
Malformed timeline data causes the load to fail before restoring machine
state.

Local rewind compresses Lua data independently of the core state within
the shared rewind memory budget. Sessions without declared values use the
ordinary core history. Rewind keeps its data in memory.

Start the matching script before loading or rewinding. Identity uses the ID,
schema version and SHA-256 of the exact source bytes, and works across
different folders and platforms. Even a source comment or line-ending change
changes the hash. Loading a snapshot never starts scripts or runs code from
the snapshot. Stopping a script removes its active value.

On load or local rewind, the value is available before the corresponding
callback. A reset restores the initial value before `"reset"`. Use
`timeline.get()` in these callbacks to redraw from the restored value.
Internal preview and replay frames do not run Lua observation callbacks.
In netplay, timeline data is local to each participant. Authoritative
resets, loads and remote rewind transitions restore the initial value
locally. Timeline values can track local observations in live callbacks for
netplay and movie playback. Network rollback reconstruction does not run Lua.

## Input recordings

Use the `movie` library to control the same `.krm` recordings as the Input menu.
The control functions are available in local play at script startup, in the
`frame` callback and after `emu.frameadvance()` resumes. Netplay permits status
inspection. Hardcore and video export disable Lua.

### Recording status

`movie.status()` returns a table. The frame is the number of movie inputs already
applied. In a `frame` callback it includes the frame that just completed.

| Field | Value |
| --- | --- |
| `mode` | `"off"`, `"recording"`, `"playback"` or `"finished"` |
| `frame` | Current position in the input tape. Zero before its first frame. |
| `length` | Number of input frames in the tape. |
| `fps` | Recorded refresh rate when a tape is active and the current console rate otherwise. |
| `path` | Absolute path of the active movie, or `nil` when off. |
| `id` | Native movie identity as 32 lowercase hexadecimal characters, or `nil` when off. A resumed recording keeps its identity. A branch receives a new one. |
| `pending` | True while a queued request is being prepared or a stopped recording is being written. |
| `error` | Reason the last operation failed, or `nil` while pending or after success. |

### Recording commands

- `movie.record(name)` begins a recording from power-on and sends a `reset` event.
- `movie.play(name)` opens an existing movie at its first frame and sends a
  `loadstate` event. The emulator pauses when playback reaches the end.
- `movie.resume(name)` restores the saved continuation point of an existing movie
  and continues recording into its identity. An older file without a continuation
  point can be played and branched. Resume sends a `loadstate` event.
- `movie.rerecord(name)` begins a new recording with the input prefix through the
  current playback position, including at the end of a movie. It sends a
  `loadstate` event. Pause before requesting a branch at an exact frame.
- `movie.stop()` ends the current recording or playback. When stopping a recording,
  the emulator captures the completed prefix and its continuation state, then
  releases the game to live input while the file is saved. The pause setting is
  preserved.
- `movie.takeover()` ends playback and resumes the game with live input. It is
  available during playback and at the end of a movie.

Names are UTF-8 filenames within the configured **Movies** folder, up to 200
bytes. The `.krm` suffix is optional. Absolute paths, directory separators and
symbolic links at movie filenames are rejected. Record and rerecord require
fresh filenames. A movie started by Lua is written when it is stopped or the
game closes.

Each command returns `true` when queued, or `nil, reason` if the command is
unavailable. Only one movie operation can be pending at a time. Preparation and
movie file I/O run separately from emulation. The game can continue running
during preparation. The native transition runs on the emulation worker between
completed frames and preserves pause, except at the end of playback or on
takeover. A top-level `emu.frameadvance()` following an asynchronous movie
command resumes after that command and the next eligible frame. A successful
transition releases Lua input overrides and clears temporary Lua save states.

A later failure appears in `movie.status().error` and is reported to the
requester's console. If a stopped recording cannot be saved, the emulator
keeps its captured tape in memory. Call `movie.stop()` again once the file can
be saved. The retry also appears in the Input menu. The application waits for
the outstanding recording before closing.

The native movie stores controller inputs, hardware metadata and core states.
Starting a new recording resets declared timeline values. Starting playback
or resuming a movie from its file restores their initial values with status
`"missing"`. A save made at a movie position can carry timeline data as
described above. To replay a script's measurements from the start of a movie,
save them to script storage and use the movie's identity and frame position
to align them.

## Drawing

Drawing from a callback follows its exact presented video frame. Game space
uses native game pixels from the top-left of the active picture where the
core supplies an active viewport, and from the full raster otherwise.
`emu.screen()` returns the current game space `width` and `height`.
HUD space uses device-independent pixels from the top-left of the video view,
including any letterbox bars. The default space is `"game"`. Use `"hud"`
for window-sized labels. Appearance follows video scaling, aspect and crop
after shader processing. Script drawings stay visible when built-in OSD is
disabled; stopping the script removes them. Screenshots and
recordings use clean video. The preview generated when loading a state from
a file has no script drawings until the next completed frame. In netplay, a
corrected video frame clears the drawings that belonged to the prior image.

Colors are `0xRRGGBBAA` Lua integers. The default stroke or text color
is opaque white, and the default fill is transparent. A stroke with alpha zero
allows fill-only shapes.

- `draw.text(x, y, text[, color[, space]])` positions the top-left of the label.
- `draw.line(x, y, x2, y2[, color[, space]])` draws between two points.
- `draw.rect(x, y, width, height[, color[, fill[, space]]])` draws a rectangle
  from its top-left.
- `draw.circle(x, y, radius[, color[, fill[, space]]])` draws a circle about
  its center.
- `draw.image(name, x, y[, space])` draws a prepared PNG from its top-left,
  using its original dimensions in the selected space. It returns `true`
  on success or `nil, reason` for an unavailable image.

To prepare an image, put the PNG beside the script and add a line like
`-- @image marker.png`. Use its exact filename in `draw.image("marker.png",
x, y)`. The app reads and decodes these files before execution. Up to 16 images
per script are allowed, each up to 1,024 pixels per side and together up to
8 MiB decoded.

## Script files

`storage.write(name, data)` queues replacement of a file with the given Lua
string. Embedded zero bytes and empty strings are supported. The worker writes
a temporary file, then replaces the destination atomically.

`storage.read(name[, offset[, length]])` queues a read. The offset is zero-based
and defaults to zero. Length defaults to 1,048,576 bytes and must be between
1 and 1,048,576. Offsets from zero through 1,048,576 are accepted. At or past
end of file, the result is an empty string. A read that reaches the end early
returns the remaining bytes.

Both functions return a positive request ID or `nil, reason`. Poll
`storage.result(id)` in a `"frame"` callback or after `emu.frameadvance()`:

- `nil, "pending"` while the worker is still handling the request.
- `true` after a successful write or a Lua byte string after a successful read.
- `nil, reason` if the operation failed or the current script has no such request.

A completed result is consumed when returned. One write submission or successful
read result can transfer data per Lua invocation. If a completed read exceeds that
invocation's transfer limit, its result remains available on a later invocation.
File operations can be requested and polled at startup and after completed frames.
Stopping or restarting a script drops its results and cancels pending work. A
write already committing can finish.

Files live under `<data folder>/scripts-data` in a separate directory per source
script, identified by its filename stem and a hash of its path. For sources
inside the configured Scripts folder, that identity follows the relative path
within that folder. Use a filename of up to 96 ASCII characters beginning with
a letter or digit. Letters, digits, periods, dashes and underscores are
permitted. Trailing periods, reserved Windows device names and filesystem links
are rejected. Each file can be up to 1 MiB. The runtime allows eight outstanding
requests per script, 32 total and 8 MiB across pending data and results.

Large file strings can be inspected in bounded pieces with string.sub and
string.byte. These two functions accept a large source string and charge only
the selected range (at most 4,096 bytes per call). The other guarded string
helpers still accept at most 4,096 bytes per argument. Use #data for the length
of a large string. Parsing incrementally keeps a script within its frame budget.


## Script tool windows

The `ui` library creates separate, nonmodal Qt windows. These are ordinary
desktop controls, not drawings over the game. Qt handles layout, fonts, theme
and display scale. Opening or focusing a tool does not automatically pause
gameplay. Typing in its fields does not also send keyboard controls to the game.
The OSD switch affects neither script drawings nor these windows. Stop the
script to remove its output.

```lua
local panel = assert(ui.window("Tools", {
    {type="row", children={
        {type="button", id="pause", text="Pause"},
        {type="button", id="resume", text="Resume"}
    }},
    {type="checkbox", id="hud", text="Show HUD", value=true},
    {type="label", id="status", text="Ready"}
}))
ui.on(panel, function(e)
    if e.event == "close" then panel = nil; return end
    if e.control == "pause" then emu.pause(true)
    elseif e.control == "resume" then emu.pause(false)
    elseif e.control == "hud" then
        ui.set(panel, "status", e.value and "HUD enabled" or "HUD disabled")
    end
end)
```

- `ui.window(title, controls)` returns a window handle. The controls are a dense
  array of descriptor tables. The root is vertical; use `row` for a horizontal
  group or `group` for a titled vertical group. Both take a `children` array.
- Each leaf needs a unique `id` within its window. Containers may optionally
  have an ID, for example to enable or disable an entire group.
- `ui.on(window, callback)` installs one handler, replacing the previous one.
  Pass `nil` to remove it. Events contain `window`, `event`, `control` and
  `value`. The event is `"click"`, `"change"` or `"close"`.
  On click, value is the button caption; on change it is the accepted value.
  Close has no control/value and the window is already detached.
- `ui.get(window, id)` reads the accepted model value. A graph returns a fresh
  array of points; a container has no value.
- `ui.set(window, id, value)` changes a value; it does not call the handler.
  For labels and buttons it changes their text. The descriptor/layout stays fixed.
- `ui.enable(window, id, boolean)` enables/disables a control or a group.
- `ui.close(window)` closes a script-owned window without calling its handler.

| Type | Descriptor fields and value |
| --- | --- |
| `label` | `text`; read/set a string |
| `button` | `text`; read/set its caption, click event |
| `checkbox` | `text`, boolean `value` (default false) |
| `text` | `text` label, string `value` (default empty); edits commit on Enter or focus loss |
| `number` | `text` label, finite `min`/`max`/`step`/`value`; defaults 0/100/1/min |
| `select` | `text` label, nonempty `items` string array, one-based integer `value` (default 1) |
| `graph` | `text` label, dense finite-number `value` array (default empty); vertical range follows current points |
| `row` / `group` | `children`, optional `id`; `group` also uses `text` for its title |

A number must stay within its range and step must be positive. The number editor
uses Qt decimal precision; extreme subnormal doubles may round in the field.
For exact hexadecimal addresses or binary/decimal strings, use a text field and
validate in Lua. Graphs are read-only: use `ui.set` with your bounded point list.

Create/change/close windows from startup, a `frame` callback or a UI handler.
Read accepted values elsewhere with `ui.get`. UI callbacks run at a completed
emulation-frame boundary, including when normal gameplay is paused. They cannot
yield, call `emu.frameadvance`, draw directly or claim a completed-frame lag
observation. A CPU debugger stopped inside a frame defers UI callbacks until that
frame finishes. Automatic local rewind also defers them until it is released or paused; remote
rewind defers them until live memory is available.
Control and memory/input requests keep their usual netplay/recording restrictions;
a window does not bypass these policies. Handle unsuccessful requests normally.

After a UI handler, the current frame can receive a `draw` event without advancing
emulation. Register a `draw` callback as well as `frame` when your overlay must
reflect UI edits while paused. Do not sample game activity from `draw` or a UI
callback. The GUI never executes Lua or reads the core directly.

Values and window changes coalesce into snapshots at most 20 times per second;
stopping all scripts clears their windows immediately. There is one pending GUI
notification, not a per-frame update backlog. The input queue holds 64 events,
coalesces adjacent edits to the same field, and processes at most four events per
worker iteration within a shared 2 ms dispatch budget (an individual callback
still has the standard 8 ms runtime limit). A full queue rejects the edit and
restores the last accepted value. New/reset/restored timelines reject stale events;
restart, stop, fault and incompatible game/hardware changes destroy owned windows.

Limits: two windows per script, eight total; 64 descriptor nodes per window,
256 total; four nesting levels. IDs use 1–64 printable ASCII characters; titles
use up to 128 UTF-8 bytes, captions/text up to 1,024 bytes, selectors up to
32 items of 128 bytes each, and graphs up to 128 points. The usual native-call,
byte and Lua allocation budgets also apply. Invalid descriptors or typed values
raise errors. A closed/unknown target, unavailable event context or full global
resource pool returns `nil, reason`; successful mutations return `true`.
Window positions and widget values stay with the running script. Use
`timeline.get()` to read the restored value of a separately declared
timeline variable.


## Runtime limits

Eight scripts can run at once. Each uses up to 4 MiB of Lua allocations,
in addition to its prepared images. Source files can be up to 256 KiB and must
contain Lua text. The runtime caps VM instructions at roughly 200,000 during
startup and 40,000 per event, checks elapsed time (50 ms startup, 8 ms per
event), and also caps native calls (192) and processed bytes (64 KiB) per
invocation. Each script can submit up to 128 drawing commands per presented
event and print up to eight 1 KiB lines per callback and 32 lines per second.
The log shows one truncation notice per window in which the limit is reached.

The runtime provides selected Lua 5.5 base, math, string, table and UTF-8
functions. Use ordinary tables, functions and loops. The available string
helpers include formatting, slicing and repetition of bounded strings.
Table sequence helpers are limited to 256 entries. General filesystem, process,
network, bytecode, dynamic code loading, the general coroutine library, Lua metatable access,
protected call functions, string patterns and binary string packing are
unavailable. Use the asynchronous storage API for script-owned files.

## Included examples

The eight numbered files in `scripts` cover drawing, controller input,
memory watches, emulation events, temporary saves and input overrides, timeline
data, input recording with measurements, and tool windows.
[Guide to the included examples](lua-examples.md)

The `scripts/games` folder contains tools for specific cartridge revisions.
[Sonic 1 Lab](games/sonic1-lab.md) · [Contra: Hard Corps Combat Lab](games/contra-hard-corps-lab.md)
