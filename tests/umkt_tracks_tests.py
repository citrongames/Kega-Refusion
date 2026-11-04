#!/usr/bin/env python3
"""Real-core packed-ROM and sound-driver handoff checks, original fixture only."""
import argparse
import ctypes as C
import json
from pathlib import Path
from link_hardware_tests import Core, fixture, require, U

class Z80Prefix(C.Structure):
    _fields_ = [("regs", U * 13), ("flags", C.c_ubyte * 11), ("cycles", U)]

def check(path):
    rom = bytearray([0xff]) * 0xd00000
    rom[:65536] = fixture()
    rom[0x1a4:0x1a8] = (len(rom) - 1).to_bytes(4, "big")
    # Logical bus address, packed file offset, unique marker.
    samples = [(0x9fffff, 0x9fffff, 0x21), (0xa80000, 0xa00000, 0x32),
               (0xafffff, 0xa7ffff, 0x43), (0xb00000, 0xa80000, 0x54),
               (0xbfffff, 0xb7ffff, 0x65), (0xc80000, 0xb80000, 0x76),
               (0xd00000, 0xc00000, 0x87), (0xdfffff, 0xcfffff, 0x98)]
    for _, offset, value in samples:
        rom[offset] = value
    core = Core(path, bytes(rom))
    try:
        active = C.c_int.from_address(core.address("md_cart_umkt_tracks_active"))
        require(active.value == 0, "size alone must not match")
        hardware = {b: tuple(core.cpu.mapping[b*5:b*5+5]) for b in (0xa0, 0xa1, 0xc0)}
        active.value = 1  # Test-only ELF injection; no production override.
        core.function("md_cart_umkt_tracks_map", None)()
        require(all(tuple(core.cpu.mapping[b*5:b*5+5]) == m for b,m in hardware.items()),
                "ZRAM, control and VDP windows preserved")
        bank = U.from_address(core.address("zbank"))
        read = core.function("z80_memory_r", C.c_ubyte, U)
        write = core.function("z80_memory_w", None, U, C.c_ubyte)
        zstate = C.c_ubyte.from_address(core.address("zstate"))
        def samples_match():
            for address, _, value in samples:
                bank.value = address & 0xff8000
                require(read(0x8000 | (address & 0x7fff)) == value, f"sample {address:06x}")
                base = core.cpu.mapping[(address >> 16) * 5]
                require(C.c_ubyte.from_address(base + ((address & 65535) ^ 1)).value == value,
                        "68000/Z80 map agrees")
        samples_match()
        for address, _, _ in samples[1:]:
            bank.value = address & 0xff8000
            before = zstate.value
            write(0x8000 | (address & 0x7fff), 0x55)
            require(zstate.value == before, "ROM write must not halt Z80")
        samples_match()
        core.frame()
        saved = core.snapshot()
        expected = core.trace(5)
        core.restore(saved)
        require(core.trace(5) == expected, "deterministic ordinary continuation")
        samples_match()
        # Replace a running program while holding BUSREQ. An IRQ briefly releases
        # and reacquires the bus before the intended reset: old PC must not run.
        z80 = Z80Prefix.from_address(core.address("Z80"))
        require(Z80Prefix.cycles.offset == 64, "native Z80 layout")
        bus = core.function("gen_zbusreq_w", None, U, U)
        reset = core.function("gen_zreset_w", None, U, U)
        run = core.function("z80_run", None, U)
        upload = core.function("z80_write_byte", None, U, U)
        reset(1, 0)
        z80.cycles = 0
        bus(1, 0)
        z80.regs[0] = 0x80
        for index, value in enumerate((0xf3, 0x04, 0x18, 0xfd)):  # DI; INC B; JR INC B
            upload(0xa00000 + index, value)
        bus(0, 0)
        run(300)
        require(z80.regs[0] == 0x80 and z80.cycles >= 300, "handoff defers old PC")
        bus(1, 400)
        require(zstate.value == 3, "BUSACK still works during handoff")
        bus(0, 500)
        core.frame()
        pending = core.snapshot()
        def finish_handoff():
            reset(0, z80.cycles + 300)
            reset(1, z80.cycles + 600)
            require(z80.regs[0] == 0, "reset starts at new vector")
            run(z80.cycles + 600)
            require(z80.regs[0] in (1, 2), "new program runs")
            return tuple(z80.regs), z80.cycles
        finished = finish_handoff()
        core.restore(pending)
        run(z80.cycles + 300)
        require(z80.regs[0] == 0x80, "pending survives saved state")
        core.restore(pending)
        require(finish_handoff() == finished, "handoff restoration deterministic")
        for hard in (0, 1):
            core.function("gen_reset", None, C.c_int)(hard)
            require(active.value == 1, "profile survives reset")
            samples_match()
        for offset, word in ((0, 0x5345), (2, 0x4741)):
            core.function("gen_tmss_w", None, U, U)(offset, word)
        samples_match()
        # The old global-tolerance workaround is gone, even in this profile.
        core.function("zbank_lockup_r", U, U)(0x7f11)
        require(zstate.value == 0, "illegal sound bus remains strict")
    finally:
        core.close()
    core = Core(path)
    try:
        require(C.c_int.from_address(core.address("md_cart_umkt_tracks_active")).value == 0,
                "ordinary ROM clears profile")
    finally:
        core.close()
    return {"status": "passed", "checks": ["size-only rejection", "packed sample map",
            "read-only ROM", "hardware windows", "state continuation",
            "IRQ handoff/BUSACK", "pending state restore", "soft/hard reset", "TMSS", "strict bus", "unload"]}

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True)
    parser.add_argument("--output")
    args = parser.parse_args()
    result = json.dumps(check(args.core), indent=2) + "\n"
    if args.output: Path(args.output).write_text(result)
    print(result, end="")
