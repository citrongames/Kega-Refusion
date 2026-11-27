# Kega Refusion Lua API 1

Open **File → Scripts…** while a game is loaded. Open a
UTF-8 `.lua` source file and choose **Run**. You can edit the code in the
script window or in an external editor, then restart it. The console shows
`print(...)` output and Lua errors for each script. Both packages include
examples in the `scripts` folder.

## Events and lifecycle

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
script that raised it. Stopping a script releases its callbacks, drawings,
input overrides and temporary saves. Game or hardware changes stop scripts
bound to an incompatible machine. Callbacks stay registered when a script's
main chunk finishes.

Reset, load and rewind clear input overrides. A frame-advance sequence waiting
to resume is stopped when the timeline changes externally. Scripts that stay
active keep their current Lua variables and may use the restore events to
rebuild from game memory or clear accumulated observations. A script's own
queued reset or temporary load can preserve its waiting sequence. Restart a
script to initialize its Lua variables again.

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
  `size` in bytes, the associated `base` address and `endian` (`"be"` or
  `"le"`). Availability, size and order can change with the active video mode.
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
  `addressUnit` (bytes per logical address) and `endian`.
- `memory.peek(cpu, address[, width[, endian[, signed[, instance]]]])` reads
  the CPU's logical map using the same integer options. Native order comes
  from the CPU record. Addresses are byte-based for 68000, Z80 and SH-2,
  and word-based for SVP SSP1601 program memory. A width still counts bytes.
  Addresses outside the CPU's range, unmapped bytes and regions without a
  side-effect-free peek return an unavailable result.
- `memory.registers(cpu[, instance])` returns `pc`, `sp`, `cycles` (within the
  current emulated frame) and a `values` table keyed by native register names.

The core's existing inspection API supplies the bytes. Starting a script
does not attach the debugger or enable CPU hooks. Operations available on a
given cartridge, add-on or console depend on the active core.

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
recovers the machine and its clock. The requesting sequence continues from
its current Lua state and can reuse its handles. Restore clears input overrides.

## Drawing

Drawing from a callback follows its exact presented video frame. Game space
uses native game pixels from the top-left of the active picture where the
core supplies an active viewport, and from the full raster otherwise.
`emu.screen()` returns the current game space `width` and `height`.
HUD space uses device-independent pixels from the top-left of the video view,
including any letterbox bars. The default space is `"game"`. Use `"hud"`
for window-sized labels. Appearance follows video scaling, aspect and crop
after shader processing. The OSD switch hides the drawings. Screenshots and
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
Table sequence helpers are limited to 256 entries. Filesystem, process,
network, bytecode, dynamic code loading, the general coroutine library, Lua metatable access,
protected call functions, string patterns and binary string packing are
unavailable.

## Included examples

`input-automation.lua` finds a pad with a right direction, captures a
temporary checkpoint and holds Right for 30 emulated frames. It restores
the checkpoint and steps once with live input before freeing the handle.
Run it in local play. Pause the game first to inspect the result of the
step without advancing subsequent frames.

`input-activity.lua` charts a live estimate over windows of 60 emulated
frames. Each sample is the nominal refresh rate multiplied by the fraction
of frames with a controller poll. The graph starts over on reset, load or
rewind. Its history is held in RAM during execution. Start and stop native
input recordings through the emulator menu.

`memory-hud.lua` reads the available work RAM on any supported console.
`fixture-hitbox.lua` draws a hitbox and position/speed HUD using the original
Kega Lua Overlay test cartridge, revision `KR-LUA1`. Generate this cartridge
with `lua_runtime_tests --write-fixture /path/to/overlay.md`; the command
creates a new file. The test source defines the ROM and the known RAM layout.


For a real-game example, sonic1-hud.lua supports Sonic the Hedgehog REV00
(serial GM 00001009-00, 512 KiB, header checksum 0x264A). It shows player/camera
position, velocity and the player's current collision radii. The example
checks the ROM metadata before interpreting RAM; other revisions/translations
need their own verified addresses. Source references are in the script.
