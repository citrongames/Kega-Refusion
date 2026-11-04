#!/usr/bin/env python3
"""Focused extended Z80 sample-bus checks on a generated, original ROM.

The fixture deliberately does not match UMKT's executable fingerprint. The
local ELF profile flag is enabled only by this test to exercise bus boundaries,
reset/state restoration and unload isolation without bundling a game ROM.
"""
import argparse
import ctypes as C
import json
from pathlib import Path
from link_hardware_tests import Core, fixture, require, U


def check(path):
    rom = bytearray([0xff]) * 0xd00000
    rom[:65536] = fixture()
    rom[0x1a4:0x1a8] = (len(rom) - 1).to_bytes(4, "big")
    samples = {0x9fffff: 0x21, 0xa00000: 0x32, 0xa81234: 0x43,
               0xb8ffff: 0x54, 0xc00004: 0x65, 0xcfffff: 0x76}
    for address, value in samples.items():
        rom[address] = value
    core = Core(path, bytes(rom))
    try:
        active = C.c_int.from_address(core.address("md_cart_umkt_tracks_active"))
        require(active.value == 0, "13 MiB size alone must not match")
        active.value = 1  # Test-only profile injection; no production override.
        bank = U.from_address(core.address("zbank"))
        read = core.function("z80_memory_r", C.c_ubyte, U)
        maps = bytes(core.cpu.mapping)
        def samples_match():
            for address, value in {**samples, 0xd00000: 255, 0xdfffff: 255}.items():
                bank.value = address & 0xff8000
                require(read(0x8000 | (address & 0x7fff)) == value,
                        f"Z80 sample at {address:06x}")
        samples_match()
        require(bytes(core.cpu.mapping) == maps, "68000 mappings unchanged")
        # Local Z80 VDP reads still use the real VDP, even when the sample bank
        # points at ROM occupying the same numerical 68000 address.
        status = C.c_ushort.from_address(core.address("status"))
        before = status.value
        expected = core.function("zbank_read_vdp", U, U)(0x7f04)
        status.value = before
        require(read(0x7f04) == (expected & 255), "local VDP remains hardware")
        core.frame()
        saved = core.snapshot()
        expected_trace = core.trace(5)
        core.restore(saved)
        require(core.trace(5) == expected_trace, "deterministic state continuation")
        require(active.value == 1, "profile survives state restore")
        samples_match()
        for hard in (0, 1):
            core.function("gen_reset", None, C.c_int)(hard)
            require(active.value == 1, "profile survives reset")
            samples_match()
    finally:
        core.close()
    core = Core(path)
    try:
        require(C.c_int.from_address(core.address("md_cart_umkt_tracks_active")).value == 0,
                "next ordinary ROM clears profile")
    finally:
        core.close()
    return {"status": "passed", "checks": ["size-only rejection", "sample boundaries",
            "68000 mapping unchanged", "local VDP", "state continuation",
            "soft/hard reset", "unload isolation"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True)
    parser.add_argument("--output")
    args = parser.parse_args()
    result = json.dumps(check(args.core), indent=2) + "\n"
    if args.output:
        Path(args.output).write_text(result)
    print(result, end="")
