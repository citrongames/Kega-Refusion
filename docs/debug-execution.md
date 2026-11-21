# CPU debugger

Open **Debug → CPU debugger**. Opening the modeless window does not pause play.
This stage supports the main 68000 and Z80 in ordinary Mega Drive cartridges,
and the Z80 in Master System, Game Gear and SG-1000. The CPU list follows the
loaded hardware. Expansion CPUs, Firecore and Link Cable debugging are the
next stage; they are not exposed as working controls here.

## Execution

Pause stops before the selected CPU's next instruction. Step into executes
one instruction, including its prefixes. Step over follows a call to its return
address and stack level. Step out follows a return to the caller's stack level.
Other devices retain their normal scheduler ordering. A CPU waiting for an
interrupt or bus grant may require completed frames before it executes another
instruction; commands remain available while waiting.

The worker suspends the entire native call stack on a same-thread coroutine.
It does not return early from a CPU interpreter, discard a partly executed
instruction, or run other processors independently after a stop.
Execute stops are before opcode fetch. Read/write stops are **after the bus
access**, potentially inside an instruction. Registers and the status line
identify this distinction. Z80 port I/O traps are not part of the memory
read/write breakpoint types.

No partial frame is sent to the display, sound output, rewind history or input
recording. Resume completes the original frame with its original input.
Saving, restoring, resetting, rewinding, closing the game or exiting completes
any pending fraction first with stops suppressed, commits that frame once,
then performs the requested operation. Saved files never contain a suspended
host stack. Closing the debugger releases its coroutine and hooks; a manually
paused game remains paused.

Hardcore, netplay and video export block CPU debugging in the worker as well
as the interface. Read-only inspection windows retain their separate policy.
Input recording/playback supports stepping and observation; there are no
unrecorded memory/register editing controls in this stage.

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

Disassembly uses the CPU's opcode mapping; safe stack reads use its logical
data mapping. Unavailable bytes are shown as ?? without calling live MMIO
handlers. The memory inspector's physical domain offsets are distinct from
CPU logical addresses, especially for banked cartridges.

Import a UTF-8 text file with one `HEX_ADDRESS LABEL` entry per line for the
selected CPU. Addresses accept bare hex, `$` and `0x` prefixes. Either `#` or
`;` starts an inline comment or a standalone comment line. Labels, comments and
bookmarks refer to logical addresses for the current game. They are retained
across resets and state loads. Switching banks can change the code at a label. Persistent debug
projects and trace/CDL tools remain in the later phase.

## Cost and lifetime

With the debugger closed there is no coroutine, snapshot timer or instruction
decoding. Native event masks are zero; the execution path only contains dormant
hook checks. An open window requests a bounded snapshot at most ten times per
second, plus explicit actions/stops. Pending snapshots and refresh requests
replace older copies. No automatic files or traces are written.
