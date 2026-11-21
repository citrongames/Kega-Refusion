# Debug projects and memory watches

## Keep a watch list

Open **Debug → Memory Watch** to add a watch. Choose the console, physical
memory domain, byte offset and a width of 1, 2 or 4 bytes. The display supports
hexadecimal, unsigned and signed decimal, and binary. Use the domain's
native byte order or select big-endian or little-endian for interpretation.
The optional name can describe the location's purpose.

From **Debug → Memory**, select a value and use **Add Watch** to carry its
domain, console and offset into the list. **Open in Memory** returns to the
selected watch's physical location. A watch for a disconnected console or
unavailable domain displays an empty value until the location is available.

**Previous** shows the value before the last observed change. **Changes**
counts changes between received snapshots. A reset, state restore or change
of inspection context starts a fresh sample history. **Clear History** clears
the previous values and counters while keeping the definitions.

The visible window uses the shared inspection timer, normally one request
every 100 ms. One sample reads up to four bytes through the safe physical
memory interface. The list is capped at 128 watches and the window stops
requesting watch samples when hidden. Watching memory leaves the emulator
state untouched.

## Open and save a project

Use **Debug → Debug Project** to open, save or save a copy as a `.krdbg`
file. Open a project while a game is loaded. **Save Project** uses the last
selected path. **Save Project As…**
lets you choose another. An asterisk by the project name marks unsaved
changes. File operations run in the background after an explicit command.
Closing a game keeps its project available to save. Loading a different game
starts a fresh set of definitions. Opening a file with a different saved
game title asks for confirmation.

A version 1 project stores the game title and up to 128 breakpoint
definitions, 4,096 addresses with labels, notes or bookmarks, and 128 memory
watches. The file limit is 2 MiB. Breakpoint definitions include their
enabled and one-shot settings, condition, and ignored-hit count. Applied
breakpoints start with fresh hit counts. Watch sample history, ROM contents,
memory contents and native emulation state stay outside the project file.

Labels, notes and bookmarks use logical addresses in the selected CPU
namespace, including separate A and B targets when linked. A bank switch can
change which bytes appear at a logical label. Watches use physical domain
offsets for their selected console. See the
[CPU debugger](debug-execution.md) for conditions and execution tools and
[physical coverage](debug-coverage.md) for bank-aware observations.

Before replacing project definitions, the worker checks breakpoint
conditions and capabilities for currently available CPUs. A rejected
project leaves the active definitions in place. With the debugger closed,
the worker validates the project in its current run or pause state.
Definitions for an unavailable CPU stay dormant until that target is available.
Memory watching follows the read-only inspection policy. Applying
breakpoints follows the CPU debugger's permissions.

## Design references

MAME documents device and address-space selection and side-effect-suppressed
memory access. FCEUX documents named memory watches, display formats and
explicit watch-list files. These references informed target selection and
the separation of watch definitions from sampled values.
[MAME debugger](https://docs.mamedev.org/debugger/#memory-accesses) ·
[FCEUX Memory Watch](https://fceux.com/web/help/MemoryWatch.html).
