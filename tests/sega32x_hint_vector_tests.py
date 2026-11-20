#!/usr/bin/env python3
"""Exercise the 32X HINT latch and framebuffer ownership through an original cartridge."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from sms_region_tests import Core, Game, P, SZ, U, Var

ADAPTER = 0xA15100
HINT = 0x70
ADJACENT = (0x6C, 0x74, 0x78)
CARTRIDGE_HINT = 0x12345678
SYNTHETIC_BIOS_HINT = 0x00C0FFEE
LATCH = 0x12AB34CD
MUTATION = 0x89ABCDEF
LEGACY_HINT = 0x008802A2
LOG = 0xFF1000
COMMAND = 0xFF1100
SETUP = 0xFF10FC
DONE = 0x51A7
FRAMEBUFFER = 0x840000
OVERWRITE = 0x860000
FB_CASES = (
    ("normal_byte", "B", FRAMEBUFFER + 1, 0xAA, 0xEE, 0x11AA3344),
    ("normal_word", "W", FRAMEBUFFER + 4, 0x1234, 0xFEDC, 0x12343344),
    ("normal_long", "L", FRAMEBUFFER + 8, 0x87654321, 0xCAFEBABE, 0x87654321),
    ("overwrite_byte", "B", OVERWRITE + 13, 0xAA, 0xEE, 0x11AA3344),
    ("overwrite_word", "W", OVERWRITE + 16, 0x1200, 0x00EE, 0x12223344),
    ("overwrite_long", "L", OVERWRITE + 20, 0x56000078, 0x00BBCC00, 0x56223378),
)
Z80_RAM = 0xA00000
Z80_BUSREQ = 0xA11100
Z80_NRES = 0xA11200


def z80_framebuffer_writer(value):
    code = bytearray((0xF3,))  # DI
    for address in (FRAMEBUFFER + 24, OVERWRITE + 29):
        bank = address >> 15
        for bit in range(9):
            code.extend((0x3E, (bank >> bit) & 1, 0x32, 0x00, 0x60))
        target = 0x8000 | (address & 0x7FFF)
        code.extend((0x3E, value, 0x32, target & 0xFF, target >> 8))
    code.extend((0x18, 0xFE))  # JR to self.
    return code


def cartridge():
    rom = bytearray(0x10000)
    struct.pack_into(">II", rom, 0, 0xFFFFF0, 0x200)
    struct.pack_into(">I", rom, HINT, CARTRIDGE_HINT)
    rom[0x100:0x110] = b"SEGA 32X        "
    rom[0x120:0x130] = b"KEGA VECTOR TEST"
    struct.pack_into(">IIII", rom, 0x1A0, 0, len(rom) - 1, 0xFF0000, 0xFFFFFF)
    rom[0x1F0] = ord("U")
    words = []
    log = []

    def word(*values):
        words.extend(values)

    def long(value):
        word(value >> 16, value & 0xFFFF)

    def write(size, value, address):
        word({"B": 0x13FC, "W": 0x33FC, "L": 0x23FC}[size])
        if size == "L":
            long(value)
        else:
            word(value)
        long(address)

    def sample(label, address, size="L"):
        word({"B": 0x13F9, "W": 0x33F9, "L": 0x23F9}[size])
        long(address)
        long(LOG + 4 * len(log) + {"B": 3, "W": 2, "L": 0}[size])
        log.append(label)

    write("W", 0, COMMAND)
    write("W", 0, SETUP)
    sample("before_enable", HINT)
    write("W", 1, ADAPTER)       # ADEN=1, SH2 held in reset.
    sample("first_enabled", HINT)
    for address in ADJACENT:
        sample(f"neighbor_{address:02x}_before", address)
        write("L", 0xFFFFFFFF, address)
        sample(f"neighbor_{address:02x}_after", address)

    for name, size, address, value in (
        ("long", "L", 0x70, 0x11223344),
        ("word_hi", "W", 0x70, 0xCAFE),
        ("word_lo", "W", 0x72, 0x5678),
        ("byte_0", "B", 0x70, 0x12),
        ("byte_1", "B", 0x71, 0xAB),
        ("byte_2", "B", 0x72, 0x34),
        ("byte_3", "B", 0x73, 0xCD),
    ):
        write(size, value, address)
        sample(name, HINT)
    sample("read_word", HINT + 2, "W")
    sample("read_byte", HINT + 1, "B")
    write("W", 0, ADAPTER)
    sample("disabled", HINT)
    write("W", 1, ADAPTER)
    sample("re_enabled", HINT)
    for label, size, address, allowed, _, _ in FB_CASES:
        normal = FRAMEBUFFER | (address & 0x1FFFC)
        write("L", 0x11223344, normal)
        write(size, allowed, address)
        sample(f"fb_owned_{label}", normal)

    def z80_write(value):
        write("W", 0x0100, Z80_BUSREQ)
        write("W", 0x0000, Z80_NRES)
        write("W", 0x0100, Z80_NRES)
        for offset, byte in enumerate(z80_framebuffer_writer(value)):
            write("B", byte, Z80_RAM + offset)
        write("W", 0x0000, Z80_BUSREQ)
        word(0x303C, 511, 0x51C8, 0xFFFE)  # Run the guest Z80 without the 68K bus grant.

    write("L", 0x11223344, FRAMEBUFFER + 24)
    write("L", 0x55667788, FRAMEBUFFER + 28)
    z80_write(0xAA)
    sample("fb_owned_z80_normal", FRAMEBUFFER + 24)
    sample("fb_owned_z80_overwrite", FRAMEBUFFER + 28)
    write("W", 0x8001, ADAPTER)  # SH2 owns the 32X video window.
    sample("fb_owner_bit", ADAPTER, "W")
    for _, size, address, _, denied, _ in FB_CASES:
        write(size, denied, address)
    z80_write(0xEE)
    write("W", 1, ADAPTER)
    # Reads while FM=1 are unspecified. Read memory after MD ownership returns.
    for label, _, address, _, _, _ in FB_CASES:
        sample(f"fb_unowned_{label}", FRAMEBUFFER | (address & 0x1FFFC))
    sample("fb_unowned_z80_normal", FRAMEBUFFER + 24)
    sample("fb_unowned_z80_overwrite", FRAMEBUFFER + 28)
    write("W", DONE, SETUP)

    # Run from work RAM so toggling ADEN does not remove the currently executing code.
    wait = len(words)
    word(0x4A79)                 # TST.W (COMMAND).L
    long(COMMAND)
    word(0x6700 | ((wait - len(words) - 1) * 2 & 0xFF))
    word(0x0C79, 1)              # CMPI.W #1,(COMMAND).L
    long(COMMAND)
    word(0x660A)                 # BNE.S over the optional mutation.
    write("L", MUTATION, HINT)
    # Report the guest-visible latch for both mailbox commands.
    sample("live_latch", HINT)
    write("W", 0, COMMAND)
    word(0x6000 | ((wait - len(words) - 1) * 2 & 0xFF))

    # Read the boot code at the same PC-relative displacement from either ROM alias.
    boot = (0x46FC, 0x2700,      # MOVE.W #$2700,SR
            0x41FA, 0x003A,      # LEA code(PC),A0, at $204 with base $206.
            0x43F9, 0x00FF, 0x4000,
            0x303C, len(words) - 1,
            0x32D8, 0x51C8, 0xFFFC,
            0x4EF9, 0x00FF, 0x4000)
    struct.pack_into(f">{len(boot)}H", rom, 0x200, *boot)
    struct.pack_into(f">{len(words)}H", rom, 0x240, *words)
    checksum = sum(struct.unpack_from(f">{(len(rom) - 0x200) // 2}H", rom, 0x200)) & 0xFFFF
    struct.pack_into(">H", rom, 0x18E, checksum)
    return bytes(rom), log


class HintCore(Core):
    def __init__(self, core, rom, directory):
        self.lib = C.CDLL(str(core))
        path = str(directory).encode()
        options = {"genesis_plus_gx_bios": b"disabled", "genesis_plus_gx_region_detect": b"ntsc-u",
                   "genesis_plus_gx_32x_add_on": b"connected", "genesis_plus_gx_add_on": b"none"}

        @C.CFUNCTYPE(C.c_bool, U, P)
        def env(command, data):
            command &= 0xFFFF
            if command in (9, 30, 31):
                C.cast(data, C.POINTER(C.c_char_p))[0] = path
                return True
            if command == 15:
                variable = C.cast(data, C.POINTER(Var)).contents
                variable.value = options.get(variable.key.decode())
                return variable.value is not None
            if command == 17:
                C.cast(data, C.POINTER(C.c_bool))[0] = False
                return True
            return command in (3, 10, 11, 16, 18, 35, 37, 44, 45, 51, 53, 54, 55)

        self.keep = {
            "environment": env,
            "video_refresh": C.CFUNCTYPE(None, P, U, U, SZ)(lambda *_: None),
            "audio_sample": C.CFUNCTYPE(None, C.c_int16, C.c_int16)(lambda *_: None),
            "audio_sample_batch": C.CFUNCTYPE(SZ, P, SZ)(lambda _, count: count),
            "input_poll": C.CFUNCTYPE(None)(lambda: None),
            "input_state": C.CFUNCTYPE(C.c_int16, U, U, U, U)(lambda *_: 0),
        }
        for name, callback in self.keep.items():
            getattr(self.lib, "retro_set_" + name)(callback)
        self.lib.retro_load_game.argtypes = [C.POINTER(Game)]
        self.lib.retro_load_game.restype = C.c_bool
        self.lib.retro_serialize_size.restype = SZ
        for name in ("retro_serialize", "retro_unserialize"):
            function = getattr(self.lib, name)
            function.argtypes, function.restype = [P, SZ], C.c_bool
        self.lib.retro_get_memory_data.argtypes = [U]
        self.lib.retro_get_memory_data.restype = P

        symbols = {}
        for line in subprocess.check_output(["nm", "-an", str(core)], text=True).splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[-1] in ("retro_init", "kegarefusion_32x_reinit", "kegarefusion_32x_attached"):
                symbols[fields[-1]] = int(fields[0], 16)
        base = C.cast(self.lib.retro_init, P).value - symbols["retro_init"]
        self.reinit = C.CFUNCTYPE(None)(base + symbols["kegarefusion_32x_reinit"])
        self.lib.retro_init()
        self.raw = C.create_string_buffer(rom)
        rom_path = directory / "original.32x"
        rom_path.write_bytes(rom)
        game = Game(str(rom_path).encode(), C.cast(self.raw, P), len(rom), None)
        assert self.lib.retro_load_game(C.byref(game))
        assert C.c_int.from_address(base + symbols["kegarefusion_32x_attached"]).value
        self.ram = self.lib.retro_get_memory_data(2)
        assert self.ram

    def read_word(self, address):
        return C.c_uint16.from_address(self.ram + (address & 0xFFFF)).value

    def read_long(self, address):
        return self.read_word(address) << 16 | self.read_word(address + 2)

    def finish_setup(self):
        marker = C.c_uint16.from_address(self.ram + (SETUP & 0xFFFF))
        marker.value = 0
        for _ in range(4):
            self.frame()
            if marker.value == DONE:
                return
        raise AssertionError("Guest setup did not finish within four frames")

    def command(self, val, address):
        flag = C.c_uint16.from_address(self.ram + (COMMAND & 0xFFFF))
        flag.value = val
        self.frame()
        assert flag.value == 0
        return self.read_long(address)


def state_header(data):
    offset = data.rfind(b"KR32X001")
    assert offset >= 0 and struct.unpack_from("<I", data, offset + 12)[0] == len(data) - offset
    return offset


def run(core, baseline, rom, names, directory, use_external):
    if use_external:
        vectors = bytearray(256)
        struct.pack_into(">II", vectors, 0, 0xFFFFF0, 0x880200)
        for index, value in zip(ADJACENT, (0x88029C, 0x8802A8, 0x8802AE)):
            struct.pack_into(">I", vectors, index, value)
        struct.pack_into(">I", vectors, HINT, SYNTHETIC_BIOS_HINT)
        (directory / "32X_G_BIOS.BIN").write_bytes(vectors)
    expected_neighbors = (0x88029C, 0x8802A8, 0x8802AE)
    old_cold = None
    if baseline is not None:
        old = HintCore(baseline, rom, directory)
        try:
            old_cold = old.save()
        finally:
            old.close()
    instance = HintCore(core, rom, directory)
    try:
        cold = instance.save()
        offset = state_header(cold)
        instance.finish_setup()
        data = {name: instance.read_long(LOG + 4 * index) for index, name in enumerate(names[:-1])}
        live = LOG + 4 * (len(names) - 1)
        saved = instance.save()
        assert instance.command(1, live) == MUTATION
        assert instance.restore(saved)
        restored = instance.command(2, live)
        # Manufacture an old native-state latch using the existing header field.
        # The decoder must preserve such a saved value even though cold boot now clears it.
        legacy = bytearray(saved)
        struct.pack_into("<I", legacy, offset + 48, LEGACY_HINT)
        assert instance.restore(bytes(legacy))
        legacy_restored = instance.command(2, live)
        assert instance.restore(saved)
        instance.lib.retro_reset()
        instance.finish_setup()
        soft = instance.read_long(LOG + 4)
        instance.reinit()
        instance.finish_setup()
        reinit = instance.read_long(LOG + 4)
        actual = {"power_snapshot": struct.unpack_from("<I", cold, offset + 48)[0],
                  **data, "serialized": struct.unpack_from("<I", saved, offset + 48)[0],
                  "restored": restored, "legacy_restored": legacy_restored,
                  "soft_reset": soft, "reinit": reinit}
        expected = {
            "power_snapshot": 0, "before_enable": CARTRIDGE_HINT, "first_enabled": 0,
            "long": 0x11223344, "word_hi": 0xCAFE3344, "word_lo": 0xCAFE5678,
            "byte_0": 0x12FE5678, "byte_1": 0x12AB5678, "byte_2": 0x12AB3478, "byte_3": LATCH,
            "read_word": 0x34CD, "read_byte": 0xAB, "disabled": CARTRIDGE_HINT, "re_enabled": LATCH,
            "serialized": LATCH, "restored": LATCH, "legacy_restored": LEGACY_HINT,
            "soft_reset": LATCH, "reinit": 0, "fb_owner_bit": 0x8081,
            "fb_owned_z80_normal": 0xAA223344, "fb_owned_z80_overwrite": 0x55AA7788,
            "fb_unowned_z80_normal": 0xAA223344, "fb_unowned_z80_overwrite": 0x55AA7788,
        }
        for label, _, _, _, _, value in FB_CASES:
            expected[f"fb_owned_{label}"] = value
            expected[f"fb_unowned_{label}"] = value
        if old_cold is not None:
            assert instance.restore(old_cold)
            instance.finish_setup()
            actual["old_cold_state"] = instance.read_long(LOG + 4)
            expected["old_cold_state"] = struct.unpack_from("<I", old_cold, state_header(old_cold) + 48)[0]
        for address, value in zip(ADJACENT, expected_neighbors):
            expected[f"neighbor_{address:02x}_before"] = value
            expected[f"neighbor_{address:02x}_after"] = value
        mismatches = {name: {"actual": f"{actual[name]:08X}", "expected": f"{value:08X}"}
                      for name, value in expected.items() if actual[name] != value}
        return {"external_synthetic_g_bios": use_external,
                "actual": {name: f"{value:08X}" for name, value in actual.items()},
                "mismatches": mismatches, "pass": not mismatches}
    finally:
        instance.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, help="Also load an actual native state from this earlier core.")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    core = args.core.resolve(strict=True)
    baseline = args.baseline.resolve(strict=True) if args.baseline else None
    rom, names = cartridge()
    cases = []
    for external in (False, True):
        with tempfile.TemporaryDirectory(prefix="kega-32x-vector-", dir=args.output.resolve().parent) as temporary:
            cases.append(run(core, baseline, rom, names, Path(temporary), external))
    with core.open("rb") as source:
        core_hash = hashlib.file_digest(source, "sha256").hexdigest()
    passed = all(case["pass"] for case in cases)
    result = {"core_sha256": core_hash, "fixture_sha256": hashlib.sha256(rom).hexdigest(),
              "cases": cases, "pass": passed}
    if baseline is not None:
        with baseline.open("rb") as source:
            result["baseline_sha256"] = hashlib.file_digest(source, "sha256").hexdigest()
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"pass": passed, "mismatches": [case["mismatches"] for case in cases]}))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
