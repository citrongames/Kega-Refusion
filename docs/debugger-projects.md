# Debug projects and memory watches

## Keep a watch list

Select a result in **Cheats → Cheat search…** and choose **Add watch** to open
**RAM Watch** with that address and value size. The list shows the watch name,
address and current value. A changed value is highlighted briefly. Hover over
the value to see its previous value and how many changes were observed.

Open **Debug → RAM Watch** or **Cheats → RAM Watch** to add an address by
hand. Enter a hexadecimal address, choose 8, 16 or 32 bits and optionally give
the watch a name. The editor starts with the active console and memory area or
the console's work RAM. **Advanced options** contains the console, physical
memory domain, display format and byte order.

From **Debug → Memory**, select a value and choose **Add watch** to carry
its location into the list. **Open in Memory** returns to the selected watch's
location. Hover over the watch address to see its console, domain and physical
offset. A watch for a disconnected console or an unavailable domain shows an
empty value until the location is available.

The change count comes from received snapshots. Resetting, restoring a state
or changing the inspection context starts a fresh sample history. **Clear
history** in the watch list context menu clears previous values and counters
while keeping the watch list.

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

A version 2 project stores the game title and up to 128 breakpoint
definitions, 4,096 addresses with labels, notes or bookmarks, and 128 memory
watches. The file limit is 2 MiB. Breakpoint definitions include their
enabled and one-shot settings, condition, and ignored-hit count. Applied
breakpoints start with fresh hit counts. Watch sample history, ROM contents,
memory contents and native emulation state stay outside the project file.

New labels, notes and bookmarks bind to the physical source and byte offset
shown by the disassembly map, separately for each CPU and console A/B. Bank
switches can therefore show different labels at the same CPU address; aliases
of one physical byte share its label. The address tooltip identifies the source.
Bookmark/name navigation checks that the saved logical address still maps to
that source; it reports an unavailable bank rather than changing game hardware.

Version 1 projects still load with their original logical labels, as do plain
text symbol imports. Logical labels are the fallback when an exact source is
unavailable. In particular, an already prefetched 68000 opcode is not assigned
to the newly mapped bank: it uses existing coverage provenance when available,
otherwise remains unqualified. No per-instruction provenance tracking is enabled
merely for labels. Project files retain both the physical identity and a logical
navigation hint. Watches use physical domain offsets for their selected console.
See the
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
