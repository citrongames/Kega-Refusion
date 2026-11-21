# CPU debugger

Open **Debug → CPU debugger** during play. The modeless window leaves execution
running until a pause command or breakpoint. The list of processors and
available controls follows the loaded hardware.

| Hardware | Execution targets |
| --- | --- |
| Mega Drive and Firecore | Main 68000 and Z80. Firecore uses the same processor types. |
| Master System, Mark III, Game Gear, Power Base Converter and SG-1000 | Z80. |
| Sega CD and MegaLD | Main and sub 68000 plus Z80. The sub-CPU uses the CD scheduler. |
| 32X and CD+32X | The base processors, plus master and slave SH-2. CD+32X also includes the CD sub-CPU. |
| SVP cartridges | Main 68000, Z80 and SSP1601. |
| Local Link Cable | Separate A/B targets for the CPU types present on each console. |

Pico device state remains available through read-only hardware inspection.

## Execution

Pause stops before the selected CPU’s next instruction. Step into executes
one instruction, including Z80 prefixes. Step over follows a call to its
return point. Step out follows a return to the caller. The debugger tracks
stack level or call/return flow as appropriate for each processor. A CPU
waiting for an interrupt or bus grant may require completed frames before it
executes another instruction. Commands remain available while waiting.

The worker suspends the native call stack on a same-thread coroutine and
preserves scheduler order across CPUs and devices. Execute stops occur before
opcode fetch. Read/write stops occur **after the bus access** and can land
inside an instruction. The status line identifies the processor that caused
the stop even while the register view shows another processor. Z80 port I/O
uses a separate path from memory read/write breakpoints.

Armed SH-2 breakpoints and instruction controls use the interpreter so the
hooks can observe instruction boundaries and data accesses, including execution
around branch delay slots. The normal SH-2 backend resumes when those hooks
are cleared. SH-2 addresses use the full 32-bit logical address space.

The SSP1601 uses 16-bit program-word addresses for navigation, disassembly
and execute breakpoints. The instruction bytes are displayed in guest order.
Its hardware return stack is available as `SP` and `S0`–`S5` in Registers.
Step over and step out use this stack. SSP1601 data-breakpoint controls are
unavailable.

Video, audio, rewind history and input recordings receive completed frames
only. Resume completes the original frame with its original input. Saving,
restoring, resetting, rewinding, closing the game or exiting first completes
any pending frame with stops suppressed. The worker commits that frame once
and then performs the requested operation. Closing the debugger releases its
coroutine and hooks. A manually paused game remains paused.

## Local Link Cable

Use **Link Cable** in the debugger for supported local Mega Drive cartridges
and Game Gear games. Enabling the cable restarts the game and creates the
second console. Changing cable mode clears rewind history. The CPU selector
labels each linked target **Console A** or **Console B**. A stop preserves the
state of both consoles and resume continues their coordinated scheduler.
Disabling the cable releases the second console and its debug targets.

Use the console selector in **Debug → Memory** to change the shared snapshot
context for the Memory, Hardware, Graphics and Sound views. This selector
appears while the second console is available.

## Editing registers and memory

Double-click the value of a writable register in Registers. Enter a
hexadecimal value that fits the register width. Click a flag to change that
bit using the current register value. The debugger enables these controls
during a full-frame pause or at a safe execute stop for the selected CPU.
Read/write breakpoints inside an instruction allow inspection.

In **Debug → Memory**, double-click a hex byte or use **Edit…** with the
selected 8-, 16- or 32-bit width and byte order. Each write targets the
selected physical domain and console. It requires a full-frame pause. The
worker checks the original bytes and machine generation before accepting an
edit. Successful edits clear rewind history.

Writable domains include main and Z80 RAM, Sega CD Program, Word, PCM and
Backup RAM, and ordinary VRAM, CRAM and VSRAM. The VDP cache and palette are
updated for subsequent rendering. ROM, VDP registers, cartridge save
domains, expansion-specific buffers, native wide-mode VRAM and MD Mode 4
CRAM remain read-only. Each window shows the capabilities of the active
hardware and mode.

Hardcore, network play and video export block CPU debugging and editing.
Read-only inspection keeps its separate policy. Input recording and playback
allow observation and stepping and block register writes, memory writes and
Link Cable changes.

## Breakpoints and addresses

Up to 128 execute/read/write breakpoints support address ranges, conditions,
ignored hits and one-shot stops. Matching includes accesses overlapping the
range. The hit count advances on matching CPU/event/address accesses before
the ignore count and condition are evaluated.

Conditions are compiled when edited. Supported form:

```text
D0 == $10
AF & $FF == 0
ADDR >= 0xFF0000
VALUE != 0
SIZE == 2
HIT >= 10
```

Use an actual register name or ADDR, VALUE, SIZE, HIT, optionally a bit mask,
then one comparison: ==, !=, <, <=, >, >=. Numbers are unsigned decimal,
$hex or 0xhex. Conditions are not general scripts.

Disassembly uses the CPU’s opcode mapping. Safe stack reads use its logical
data mapping. The viewer shows unavailable bytes as `??` and leaves live
MMIO untouched. The memory inspector uses physical domain offsets. Banked
cartridges can map different physical bytes to the same CPU logical address.

Import a UTF-8 text file with one `HEX_ADDRESS LABEL` entry per line for the
selected CPU. Addresses accept bare hex, `$` and `0x` prefixes. Either `#` or
`;` starts an inline comment or a standalone comment line. Labels, comments and
bookmarks refer to logical addresses for the current game. They are retained
across resets and state loads. Switching banks can change the code at a label.
[Instruction/event tracing](debug-tracing.md) and
[physical code/data coverage](debug-coverage.md) are available separately.
Use [debug projects](debugger-projects.md) to save these logical annotations,
breakpoint definitions and memory watches together.

## Cost and lifetime

Closing the debugger releases the coroutine and stops snapshot collection and
instruction decoding. Native event masks stay clear and hook checks stay
dormant. An open window requests a bounded snapshot about ten times per second,
plus explicit actions and stops. Pending snapshots and refresh requests
replace older copies. File import runs on request.
