"""Exercise the Korean cartridge decoders through actual Z80 bus operations.

Usage: python3 tests/sms_hicom_hwasung_tests.py path/to/genesis_plus_gx_libretro.so
The test generates original bank-tagged programs. The CRC selects the board.
"""
import argparse
import ctypes
import struct
import tempfile
from pathlib import Path

from sms_codemasters_mapper_tests import Asm, Core, forge_crc


def check_board(core_path, directory, board, crc, banks):
    asm = Asm()
    # Execute from work RAM so every cartridge window can be switched.
    if board == "HiCom":
        for selector in (0, 1, 3, 4, 7, 0xff):
            asm.write(0xffff, selector)
            low = 2 * (selector % (banks // 2))
            asm.read(0x0600, low, "selected lower page")
            asm.read(0x4600, low + 1, "selected second page")
            asm.read(0x8600, 0, "fixed upper page")
            asm.read(0xbfff, 0, "end of fixed upper page")
            asm.read(0xdfff, selector, "mapper write also reaches mirrored work RAM")
    elif board == "Hwasung":
        asm.read(0x4600, 1, "reset middle page")
        asm.read(0x8600, 2, "reset upper page")
        for selector in (1, 0, 2, 3, 0xfe, 0xff):
            asm.write(0x2000, selector)
            low = (selector & 1) * 2 + 1
            asm.read(0x0600, 0, "fixed lower page")
            asm.read(0x4600, low, "game first page")
            asm.read(0xbfff, (low + 1) % banks, "game second page")
    else:
        # Another mapper sharing the 32 KiB helper mirrors the selected page.
        asm.write(0x2000, 1)
        asm.read(0x0600, 2, "selected lower page")
        asm.read(0x8600, 2, "mirrored upper page")

    asm.write(0xd4ff, 0xa7)
    asm.label("done")
    if board in ("HiCom", "Hwasung"):
        # The host can change the selector and inspect the active pages after a load.
        asm.emit(0x3a)
        asm.word(0xd400)
        asm.emit(0xfe, 1)
        asm.jump("change", 0xca)
        asm.emit(0xfe, 3)
        asm.jump("inspect", 0xca)
        asm.jump("done")
        asm.label("change")
        asm.write(0xffff if board == "HiCom" else 0x2000, 0)
        asm.write(0xd401, 1)
        asm.write(0xd400, 0)
        asm.jump("done")
        asm.label("inspect")
        for offset, address in enumerate((0x4600, 0x8600)):
            asm.emit(0x3a)
            asm.word(address)
            asm.emit(0x32)
            asm.word(0xd410 + offset)
        asm.write(0xd401, 3)
        asm.write(0xd400, 0)
    asm.jump("done")
    program = asm.finish()
    assert 0x200 + len(program) < 0x600
    image = bytearray().join(bytes([bank]) * 0x4000 for bank in range(banks))
    boot = bytes.fromhex("f3 31 f0 df 21 00 02 11 00 c1 01")
    boot += struct.pack("<H", len(program)) + bytes.fromhex("ed b0 c3 00 c1")
    image[:len(boot)] = boot
    image[0x200:0x200 + len(program)] = program
    image[0x7ff0:0x7ff8] = b"TMR SEGA"
    image = forge_crc(image, crc)
    path = directory / (board + ".sms")
    path.write_bytes(image)
    core = Core(core_path, path, image)
    try:
        core.frame(2)
        ram = core.ram()
        assert ram[0x14ff] == 0xa7, f"{board}: Z80 diagnostic did not finish"
        failures = [f"{name}: expected {expect}, got {ram[0x1000+i]}"
                    for i, (name, expect) in enumerate(asm.results)
                    if ram[0x1000+i] != expect]
        print(f"{board}: {len(asm.results) - len(failures)}/{len(asm.results)}", *failures[:3])
        if board in ("HiCom", "Hwasung"):
            size = core.lib.retro_serialize_size()
            state = ctypes.create_string_buffer(size)
            core.lib.retro_serialize.restype = ctypes.c_bool
            core.lib.retro_unserialize.restype = ctypes.c_bool
            assert core.lib.retro_serialize(state, size)
            core.command(1)
            core.command(3)
            assert bytes(ram[0x1410:0x1412]) == bytes((1, 0 if board == "HiCom" else 2))
            assert core.lib.retro_unserialize(state, size)
            core.command(3)
            assert bytes(ram[0x1410:0x1412]) == bytes((7 if board == "HiCom" else 3, 0))
        return not failures
    finally:
        core.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        success = [check_board(args.core, Path(tmp), *entry) for entry in (
            ("HiCom", 0x98af0236, 8),
            ("Hwasung", 0x1b3e032e, 4),
            ("11 Hap Gam-Boy", 0x4342db9d, 8),
        )]
    raise SystemExit(0 if all(success) else 1)
