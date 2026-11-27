# Kega Refusion Lua API 1

Open **Lua scripts** in the debugging tools while a game is loaded. Open a
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
| `"frame"` | After a completed frame, before presentation. Callbacks also run for completed fast-forward frames that the display skips. |
| `"reset"` | After a reset that keeps the current game and hardware. |
| `"loadstate"` | After loading a saved state with compatible hardware. |
| `"rewind"` | While presenting a restored frame. Print output is suppressed. |
| `"draw"` | When the current frame is redrawn after starting or stopping a script or selecting the presented console. The emulated clock stays at the current frame and print output is suppressed. |

Scripts run in start order in separate Lua environments. Restarting a running
script keeps its place and creates a fresh environment. An error stops the
script that raised it. Stopping a script clears its callbacks and drawing,
and game or hardware changes stop scripts bound to the old machine. Lua
variables retain their current values on reset, load and rewind. A HUD can
rebuild by reading game memory in each event. Restart the script when it
requires a fresh Lua state. The emulator skips scripting during internal
replay and rollback reconstruction. Hardcore disables script execution.

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
network, bytecode, dynamic code loading, coroutines, Lua metatable access,
protected call functions, string patterns and binary string packing are
unavailable.

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
