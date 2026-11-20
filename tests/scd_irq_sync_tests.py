#!/usr/bin/env python3
"""Check Sega CD IFL2 synchronization on a local unstripped Linux core.

The harness schedules Main CPU bus accesses at specific cycle counts. An
original Sub CPU program enters the core's real polling-idle state and serves
level 2 through the real interrupt handler. No retail BIOS or game is needed.
"""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from sms_region_tests import Core, Game, P, U, Var


ROOT = Path(__file__).resolve().parents[1]
IFL2 = 0xA12000
RESET_BUSREQ = 0xA12001
IRQ_COUNT = 0xA12028
IRQ_MASK = 0xFF8032
MAILBOX_POLL = 3 << 16
HALT = 2

# Derive native fields from the same headers used by the tested core.
NATIVE_SOURCE = r"""
#include "shared.h"

unsigned cpu_pc(const m68ki_cpu_core *cpu) { return cpu->pc; }
unsigned cpu_sp(const m68ki_cpu_core *cpu) { return cpu->dar[15]; }
unsigned cpu_cycles(const m68ki_cpu_core *cpu) { return cpu->cycles; }

void set_cpu_sr(void (*set_reg)(m68k_register_t, unsigned), unsigned value)
{
  set_reg(M68K_REG_SR, value);
}
unsigned cpu_stopped(const m68ki_cpu_core *cpu) { return cpu->stopped; }
void set_cpu_cycles(m68ki_cpu_core *cpu, unsigned cycles) { cpu->cycles = cycles; }
int prg_ram_accessible(const m68ki_cpu_core *cpu)
{
  return !cpu->memory_map[0x42].read16 && !cpu->memory_map[0x42].write16;
}
void load_sub_program(m68ki_cpu_core *cpu, const uint16 *words, unsigned count)
{
  memcpy(cpu->memory_map[0x42].base, words, count * sizeof(*words));
}
"""


def main_cartridge():
    rom = bytearray(0x10000)
    struct.pack_into(">II", rom, 0, 0xFFFFF0, 0x200)
    rom[0x100:0x110] = b"SEGA GENESIS    "
    rom[0x120:0x130] = b"ORIGINAL CD TEST"
    struct.pack_into(">III", rom, 0x1A4, len(rom) - 1, 0xFF0000, 0xFFFFFF)
    rom[0x1F0] = ord("U")
    struct.pack_into(">HH", rom, 0x200, 0x4E71, 0x60FC)  # NOP; BRA.S -4
    return bytes(rom)


def sub_program(poll):
    words = (C.c_uint16 * 0x200)()
    words[0:4] = (0, 0x1000, 0, 0x200)
    words[0x68 // 2:0x68 // 2 + 2] = (0, 0x300)  # Level 2 autovector
    words[0x100:0x102] = (0x46FC, 0x2000)  # Unmask interrupts.
    if poll:
        # MOVE.W ($FF8010).L,D0; BRA.S -8
        words[0x102:0x106] = (0x3039, 0x00FF, 0x8010, 0x60F8)
    else:
        words[0x102:0x104] = (0x4E71, 0x60FC)  # NOP; BRA.S -4
    # ADDQ.W #1,($FF8028).L; RTE
    words[0x300 // 2:0x300 // 2 + 4] = (0x5279, 0x00FF, 0x8028, 0x4E73)
    return words


def build_native(source, directory):
    shim = directory / "scd_irq_test.c"
    module = directory / "scd_irq_test.so"
    shim.write_text(NATIVE_SOURCE)
    includes = [source / "core", source / "libretro", source / "libretro/libretro-common/include"]
    includes.extend(path for path in (source / "core").rglob("*") if path.is_dir())
    subprocess.run(
        ["gcc", "-O2", "-shared", "-fPIC", "-DLSB_FIRST", "-D__LIBRETRO__",
         "-DKEGAREFUSION_32X", "-DM68K_OVERCLOCK_SHIFT=20", "-DZ80_OVERCLOCK_SHIFT=20",
         "-DINLINE=static inline", *["-I" + str(path) for path in includes],
         str(shim), "-o", str(module)],
        check=True,
    )
    native = C.CDLL(str(module))
    for name in ("cpu_pc", "cpu_sp", "cpu_cycles", "cpu_stopped", "prg_ram_accessible"):
        function = getattr(native, name)
        function.argtypes, function.restype = [P], U
    native.set_cpu_sr.argtypes, native.set_cpu_sr.restype = [P, U], None
    native.set_cpu_cycles.argtypes, native.set_cpu_cycles.restype = [P, U], None
    native.load_sub_program.argtypes = [P, C.POINTER(C.c_uint16), U]
    native.load_sub_program.restype = None
    return native


class Fixture:
    def __init__(self, core, native):
        self.core = core
        self.hard_reset = self.function("gen_reset", None, C.c_int)
        self.native = native
        self.main = self.address("m68k")
        self.sub = self.address("s68k")
        self.read8 = self.function("ctrl_io_read_byte", U, U)
        self.read16 = self.function("ctrl_io_read_word", U, U)
        self.write8 = self.function("ctrl_io_write_byte", None, U, U)
        self.write16 = self.function("ctrl_io_write_word", None, U, U)
        self.sub_run = self.function("s68k_run", None, U)
        self.sub_write = self.function("scd_write_word", None, U, U)

    def address(self, name):
        return self.core.base + self.core.syms[name]

    def function(self, name, restype, *args):
        return C.CFUNCTYPE(restype, *args)(self.address(name))

    def reset(self, poll=True):
        self.hard_reset(1)
        self.place_main(1024)
        self.write8(RESET_BUSREQ, 0)
        assert self.native.prg_ram_accessible(self.main)
        words = sub_program(poll)
        self.native.load_sub_program(self.main, words, len(words))
        self.write8(RESET_BUSREQ, 1)
        self.sub_write(IRQ_MASK, 4)
        self.sub_run(1024)
        assert 0x204 <= self.native.cpu_pc(self.sub) <= 0x20A
        assert self.native.cpu_stopped(self.sub) == (MAILBOX_POLL if poll else 0), (
            "Sub CPU did not reach expected run or mailbox polling state"
        )
        assert not self.native.prg_ram_accessible(self.main)
        assert self.read16(IRQ_COUNT) == 0

    def place_main(self, cycles):
        self.native.set_cpu_cycles(self.main, cycles)

    def request(self, width):
        if width == 8:
            self.write8(IFL2, 1)
        else:
            self.write16(IFL2, 0x0101)

    def status(self, width):
        return self.read8(IFL2) if width == 8 else self.read16(IFL2)

    def count(self):
        return self.read16(IRQ_COUNT)

    def checks(self, width):
        def status_word(high, low=1):
            return high if width == 8 else (high << 8) | low

        self.reset()
        for cycle in (1024, 2048, 3072, 4096):
            self.place_main(cycle)
            self.request(width)
            if cycle == 1024:
                poll_woke = self.native.cpu_stopped(self.sub) == 0
            self.place_main(cycle + 768)
            self.status(width)
        repeated_count = self.count()

        # Independently test the status read with a Sub CPU that is running.
        # The first read is too soon to acknowledge. By the next read the
        # elapsed clocks are sufficient, with no other bus access in between.
        self.reset(poll=False)
        self.request(width)
        immediately_busy = self.status(width) == status_word(0x81)
        self.place_main(2048)
        fresh_status = self.status(width)
        fresh = fresh_status == status_word(0x80) and self.count() == 1

        self.reset()
        self.sub_write(IRQ_MASK, 0)
        self.place_main(1024)
        self.request(width)
        disabled_status = self.status(width)
        disabled_poll_idle = self.native.cpu_stopped(self.sub) == MAILBOX_POLL
        self.sub_run(2048)
        disabled = disabled_status == status_word(0) and disabled_poll_idle and self.count() == 0

        self.reset()
        self.place_main(1024)
        if width == 8:
            self.write8(RESET_BUSREQ, 3)
            self.request(width)
        else:
            self.write16(IFL2, 0x0103)
        held_state = self.native.cpu_stopped(self.sub)
        held_status = self.status(width)
        busreq = (held_state & HALT and held_status == status_word(0x81, 3)
                  and self.native.prg_ram_accessible(self.main))
        # Clearing reset must keep the Sub CPU halted and Main CPU's PRG RAM mapping open.
        if width == 8:
            self.write8(RESET_BUSREQ, 0)
            self.request(width)
        else:
            self.write16(IFL2, 0x0100)
        held_reset = self.status(width)
        reset = (self.native.cpu_stopped(self.sub) & HALT
                 and held_reset == status_word(0x81, 2)
                 and self.native.prg_ram_accessible(self.main))
        return {
            "width": width,
            "poll_woke": poll_woke,
            "immediately_busy": immediately_busy,
            "fresh_status": fresh,
            "fresh_status_value": fresh_status,
            "repeated_count": repeated_count,
            "disabled_request_ignored": disabled,
            "busreq_preserved": bool(busreq),
            "reset_preserved": bool(reset),
            "pass": all((poll_woke, immediately_busy, fresh, repeated_count == 4,
                         disabled, busreq, reset)),
        }


    def rte_boundary(self, runner_name):
        self.reset(poll=False)
        run = self.function(runner_name, None, U)

        def cycles():
            return self.native.cpu_cycles(self.sub)

        def registers():
            return self.native.cpu_pc(self.sub), self.native.cpu_sp(self.sub)

        resumed_pc, stack = registers()
        self.request(8)
        run(cycles() + 1)
        assert registers() == (0x300, stack - 6), "First IRQ did not stack the interrupted program"
        run(cycles() + 1)
        assert registers() == (0x306, stack - 6), "First IRQ handler did not reach RTE"
        assert self.count() == 1

        self.request(8)
        assert self.status(8) == 0x81, "Second request must wait behind the first IRQ mask"
        rte_start = cycles()
        deadline = rte_start + 4
        run(deadline)
        rte_end = cycles()
        state_after_rte = registers()
        status_after_rte = self.status(8)
        held = (status_after_rte == 0x81 and state_after_rte == (resumed_pc, stack)
                and self.count() == 1 and rte_end - rte_start == 80)

        # Both implementations overshoot the run budget by completing RTE.
        # Only the corrected implementation postpones the next autovector.
        run(cycles() + 1)
        state_after_ack = registers()
        status_after_ack = self.status(8)
        acknowledged = status_after_ack == 0x80 and state_after_ack == (0x300, stack - 6)
        run(cycles() + 512)
        completed = self.count() == 2 and registers()[1] == stack
        return {
            "runner": runner_name,
            "rte_start": rte_start,
            "deadline": deadline,
            "cycles_after_rte": rte_end,
            "status_after_rte": status_after_rte,
            "pc_sp_after_rte": state_after_rte,
            "held_across_deadline": held,
            "acknowledged_on_next_entry": acknowledged,
            "returned_twice": completed,
            "pass": held and acknowledged and completed,
        }

    def external_sr_write(self):
        # The state loader also uses the register setter outside of either CPU runner.
        self.reset(poll=False)
        set_reg = self.address("s68k_set_reg")
        self.native.set_cpu_sr(set_reg, 0x2200)
        self.request(8)
        assert self.status(8) == 0x81
        stack = self.native.cpu_sp(self.sub)
        self.native.set_cpu_sr(set_reg, 0x2000)
        return (self.status(8) == 0x80 and self.native.cpu_pc(self.sub) == 0x300
                and self.native.cpu_sp(self.sub) == stack - 6)


def execution_abis(core):
    values = {}
    for purpose, symbol in (("recording", "retro_kegarefusion_recording_abi"),
                            ("network", "retro_kegarefusion_network_state_abi")):
        getter = getattr(core.lib, symbol)
        getter.argtypes, getter.restype = [], C.c_char_p
        value = getter()
        assert value, f"Missing {purpose} execution ABI"
        values[purpose] = value.decode("ascii")
    return values


def load_fixture(core_path, directory):
    rom = main_cartridge()
    rom_path = directory / "original.md"
    rom_path.write_bytes(rom)
    (directory / "bios_CD_U.bin").write_bytes(rom.ljust(0x20000, b"\0"))
    options = {
        "genesis_plus_gx_add_on": "none",
        "genesis_plus_gx_32x_add_on": "disconnected",
        "genesis_plus_gx_region_detect": "ntsc-u",
    }
    core = Core(str(core_path), rom, str(rom_path), options)
    assert core.lib.retro_kegarefusion_system_hw() == 0x80, "ordinary MD hardware path inactive"
    md_abi = execution_abis(core)
    core.lib.retro_unload_game()
    old_environment = core.keep[0]
    system_directory = str(directory).encode()

    @C.CFUNCTYPE(C.c_bool, U, P)
    def environment(command, data):
        if (command & 65535) in (9, 30, 31):
            C.cast(data, C.POINTER(C.c_char_p))[0] = system_directory
            return True
        if (command & 65535) == 15:
            variable = C.cast(data, C.POINTER(Var)).contents
            if variable.key == b"genesis_plus_gx_add_on":
                variable.value = b"sega/mega cd"
                return True
        return old_environment(command, data)

    core.keep.append(environment)
    core.lib.retro_set_environment(environment)
    game = Game(core.path, C.cast(core.raw, P), len(rom), None)
    assert core.lib.retro_load_game(C.byref(game)), "original Mode 1 fixture did not load"
    assert core.lib.retro_kegarefusion_system_hw() == 0x84, "Sega CD hardware path inactive"
    return core, md_abi


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=ROOT / "build/deps/genesis-plus-gx-32x-linux")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    core_path = args.core.resolve()
    scratch = ROOT / "build/verification/scd-cdc-accuracy"
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="irq-test-", dir=scratch) as temporary:
        directory = Path(temporary)
        native = build_native(args.source.resolve(), directory)
        core, md_abi = load_fixture(core_path, directory)
        try:
            cd_abi = execution_abis(core)
            fixture = Fixture(core, native)
            cases = [fixture.checks(8), fixture.checks(16)]
            rte = [fixture.rte_boundary(name) for name in ("s68k_run", "kegarefusion_s68k_run")]
            outside_runner = fixture.external_sr_write()
        finally:
            core.close()
    compatibility = {
        "ordinary_md": md_abi,
        "sega_cd": cd_abi,
        "pass": all(value.endswith("-md-vdp-v6") and cd_abi[purpose] == value + "-scd-irq-v1"
                    for purpose, value in md_abi.items()),
    }
    passed = all(case["pass"] for case in cases + rte) and compatibility["pass"] and outside_runner
    report = {"pass": passed, "core_sha256": hashlib.sha256(core_path.read_bytes()).hexdigest(),
              "results": cases, "rte_boundary": rte, "external_sr_write": outside_runner,
              "compatibility": compatibility}
    summary = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(summary)
    print(summary, end="")
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
