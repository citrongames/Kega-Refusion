#!/usr/bin/env python3
"""Run independent bank-tagged Z80 diagnostics against the supplied libretro core."""
import argparse
import struct
import tempfile
from pathlib import Path

from sms_codemasters_mapper_tests import Asm, Core, forge_crc


def slots(asm, expected, label):
    for index, page in enumerate(expected):
        asm.read(index * 0x2000 + 0x1800, page, f"{label}, slot {index}")


def xor_board():
    asm = Asm()
    default = (0, 0, 0x60, 0x61, 0x62, 0x63)
    slots(asm, default, "power on")
    for address, value in ((0x2000, 1), (0x2001, 2), (0x3fff, 3),
                           (0xa000, 4), (0xbff3, 5), (0xbffc, 6), (0xbfff, 7)):
        asm.write(address, value)
        pages = (0, 0, *(value ^ mask for mask in (31, 30, 29, 28)))
        slots(asm, pages, f"write to {address:04x}")
    for address in (0x1fff, 0x4000, 0x6000, 0x9fff, 0xc000, 0xfff3, 0xfffc):
        asm.write(address, 0x7f)
        slots(asm, pages, f"unselected address {address:04x}")
    asm.write(0x2000, 0x80)
    slots(asm, (0, 0, 0, 0, 0, 0), "unpopulated pages")
    asm.write(0x2000, 0x7f)
    slots(asm, default, "valid pages after unpopulated pages")
    return asm


def super_game():
    asm = Asm()
    slots(asm, (0, 1, 0, 1, 255, 255), "power on")
    # Page offsets for the six cartridge windows, from the published hardware table.
    # Values 0 through 3 are relative to the selected bank. BIOS/filler pages are absolute.
    for mode, even, odd in (
        (0x00, (0, 1, 0, 1, 255, 255), (2, 3, 2, 3, 255, 255)),
        (0x20, (0, 1, 2, 3, 255, 255), (0, 1, 2, 3, 255, 255)),
        (0x40, (128, 129, 0, 1, 255, 255), (128, 129, 2, 3, 255, 255)),
        (0x60, (128, 129, 254, 255, 0, 1), (128, 129, 254, 255, 2, 3)),
        (0x80, (128, 129, 0, 1, 1, 0), (128, 129, 2, 3, 3, 2)),
        (0xa0, (128, 129, 0, 1, 2, 3), (128, 129, 2, 3, 0, 1)),
        (0xc0, (255,) * 6, (255,) * 6),
        (0xe0, (255,) * 6, (255,) * 6),
    ):
        for parity, layout in enumerate((even, odd)):
            # Select the second megabyte through the canonical or alias register.
            bank = 0x16 + parity
            base = 128 + 2 * (bank & 0x3e)
            for mapper, config in ((0xfff3, 0xfffc), (0xbff3, 0xbffc)):
                asm.write(0xfff3, 0)
                asm.write(0xfffc, 0)
                asm.write(0xdff3, 0x55)
                asm.write(0xdffc, 0xaa)
                asm.write(mapper, bank)
                asm.write(config, mode | 0x10)
                slots(asm, [base + p if p < 4 else p for p in layout],
                      f"mode {mode:02x}, bank {bank:02x}, registers {mapper:04x}/{config:04x}")
                asm.read(0xdff3, bank if mapper == 0xfff3 else 0x55, "mapper RAM shadow")
                asm.read(0xdffc, (mode | 0x10) if config == 0xfffc else 0xaa, "mode RAM shadow")
    asm.write(0xfff3, 0x16)
    asm.write(0xfffc, 0x30)
    for address in (0x7ff3, 0x7ffc, 0xbff2, 0xbffd, 0xdff3, 0xdffc):
        asm.write(address, 0)
    slots(asm, (172, 173, 174, 175, 255, 255), "nearby addresses leave the mapping intact")
    return asm


def run(core, directory, name, asm, crc, banks):
    asm.write(0xd4ff, 0xa7)
    asm.label("done")
    asm.jump("done")
    program = asm.finish()
    # The probe executes entirely in work RAM, below the result buffer at D000.
    assert len(program) < 0xd000 - 0xc100
    image = bytearray().join(bytes([bank]) * 0x2000 for bank in range(banks))
    loader = bytes.fromhex("f3 31 f0 df 21 00 02 11 00 c1 01")
    loader += struct.pack("<H", len(program)) + bytes.fromhex("ed b0 c3 00 c1")
    image[:len(loader)] = loader
    image[0x200:0x200 + len(program)] = program
    image[0x7ff0:0x7ff8] = b"TMR SEGA"
    image = forge_crc(image, crc)
    path = directory / (name + ".sms")
    path.write_bytes(image)
    em = Core(core, path, image)
    try:
        em.frame(6)
        ram = em.ram()
        assert ram[0x14ff] == 0xa7, f"{name}: probe did not finish"
        errors = [f"{label}: expected {value}, got {ram[0x1000 + i]}"
                  for i, (label, value) in enumerate(asm.results) if ram[0x1000 + i] != value]
        print(f"{name}: {len(asm.results) - len(errors)}/{len(asm.results)}", *errors[:2])
        return not errors
    finally:
        em.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        results = [run(args.core, directory, "128 Hap", xor_board(), 0xba5ec0e3, 128),
                   run(args.core, directory, "Super Game 150", super_game(), 0x1b8956d1, 256)]
    raise SystemExit(0 if all(results) else 1)
