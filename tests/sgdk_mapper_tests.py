#!/usr/bin/env python3
"""Generated SSF/SRAM cartridges; exercise bank writes with real guest instructions."""
import argparse
import ctypes as C
from pathlib import Path
import struct
import tempfile
from sms_region_tests import Core, U
from cd32x_ram_cart_tests import CdCore, make_disc

class MapperCore(Core):
    mailbox = CdCore.mailbox
    access = CdCore.access
    read = CdCore.read
    write = CdCore.write
    expect = CdCore.expect

def check(core_path, directory, label, sgdk, declared_sram):
    # Reuse the original RAM-safe mailbox program, never a commercial ROM.
    make_disc(directory)
    monitor = (directory / "bios_CD_U.bin").read_bytes()[0x200:0x300]
    rom = bytearray(0x500000)
    struct.pack_into(">II", rom, 0, 0xFFFF00, 0x200)
    rom[0x100:0x110] = b"SEGA SSF        "
    rom[0x110:0x120] = b"(C)SGDK 2026    " if sgdk else b"(C)ORIGINAL TEST"
    rom[0x120:0x150] = b"ORIGINAL MAPPER FIXTURE".ljust(48)
    struct.pack_into(">IIII", rom, 0x1a0, 0, len(rom)-1, 0xFF0000, 0xFFFFFF)
    if declared_sram:
        rom[0x1b0:0x1b4] = bytes.fromhex("5241f820")
        struct.pack_into(">II", rom, 0x1b4, 0x200001, 0x20ffff)
    rom[0x1f0] = ord("U")
    rom[0x200:0x202] = bytes.fromhex("60fe")
    for page in range(10):
        rom[page*0x80000+0x400] = page
    filename = directory/(label+".md")
    filename.write_bytes(rom)
    core = MapperCore(core_path, bytes(rom), str(filename),
                      {"genesis_plus_gx_add_on":"none",
                       "genesis_plus_gx_32x_add_on":"disconnected"})
    core.ram = core.base + core.syms["work_ram"]
    for offset, value in enumerate(monitor):
        C.c_ubyte.from_address(core.ram + ((0x2000+offset)^1)).value = value
    setreg = C.CFUNCTYPE(None,U,U)(core.base+core.syms["m68k_set_reg"])
    setreg(17,0x2700);setreg(15,0xFFFF00);setreg(16,0xFF2000)
    core.expect(0x400,0,"initial bank 0",byte=True)
    core.write(0xA130F1,1,byte=True)
    official = sgdk and declared_sram
    core.expect(0x400,0 if official else 1,"SRAM control versus extended bank 0",byte=True)
    if official:
        core.write(0x200001,0x5a,byte=True)
        core.expect(0x200001,0x5a,"enabled SRAM",byte=True)
    core.write(0xA130F1,0,byte=True)
    core.expect(0x400,0,"restored boot bank",byte=True)
    if official:
        core.expect(0x200001,0,"disabled SRAM exposes ROM",byte=True)
        core.write(0xA130F1,3,byte=True)
        core.expect(0x200001,0x5a,"SRAM contents retained",byte=True)
        core.write(0x200001,0xa5,byte=True)
        core.expect(0x200001,0x5a,"SRAM write protection",byte=True)
        core.write(0xA130F1,0,byte=True)
    core.write(0xA130FF,8,byte=True)
    core.expect(0x380400,8,"upper ROM page selectable",byte=True)
    state=core.save()
    core.write(0xA130FF,9,byte=True)
    core.expect(0x380400,9,"second upper page",byte=True)
    assert core.restore(state)
    core.expect(0x380400,8,"bank survives state restore",byte=True)
    core.close()
    print(label,"PASS")

if __name__=="__main__":
    ap=argparse.ArgumentParser();ap.add_argument("--core",required=True);args=ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="kega-ssf-sram-") as folder:
        directory=Path(folder)
        for label,sgdk,sram in (("sgdk_sram",True,True),
                               ("extended_ssf_sram",False,True),
                               ("sgdk_without_sram",True,False)):
            check(args.core,directory,label,sgdk,sram)
