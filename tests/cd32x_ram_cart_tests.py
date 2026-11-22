#!/usr/bin/env python3
"""Exercise CD32X RAM cartridge maps and native states on a Linux libretro core.

The original synthetic Sega CD BIOS is a 68000 mailbox monitor. All accesses
under test are guest MOVE instructions. The generated ISO selects CD Mode 2.
"""
import argparse
import ctypes as C
from pathlib import Path
import struct
import tempfile

from sms_region_tests import Core, Game, P, SZ, U, Var


ADAPTER = 0xA15100
BANK = 0xA15104
DREQ = 0xA15106
PAGE = 0xA130F0
WINDOW = (0x400100, 0x600100)
FIXED = 0x880100
BANKED = 0x900100
COMMAND, ADDRESS, VALUE = 0xFF1000, 0xFF1002, 0xFF1006
READ_WORD, WRITE_WORD, READ_BYTE, WRITE_BYTE = range(1, 5)
OPTION = b"kegarefusion_cd32x_ram_cart"
CAPACITIES = (0, 4, 8, 16)
MIB = 1 << 20


def make_disc(directory):
    bios = bytearray(128 * 1024)
    struct.pack_into(">II", bios, 0, 0xFFFFF0, 0x200)
    bios[0x100:0x110] = b"SEGA GENESIS    "
    bios[0x120:0x130] = b"CD32X RAM TEST  "
    bios[0x1F0] = ord("U")
    words = [0x46FC, 0x2700]  # MOVE.W #$2700,SR
    labels = {}
    branches = []

    def absolute(opcode, address):
        words.extend((opcode, address >> 16, address & 0xFFFF))

    def branch(opcode, target):
        branches.append((len(words), target))
        words.append(opcode)

    labels["poll"] = len(words)
    absolute(0x3039, COMMAND)  # MOVE.W COMMAND,D0
    branch(0x6700, "poll")    # BEQ.S poll
    absolute(0x2079, ADDRESS)  # MOVEA.L ADDRESS,A0
    for command, opcode in ((READ_WORD, 0x33D0), (WRITE_WORD, 0x30B9),
                            (READ_BYTE, 0x13D0), (WRITE_BYTE, 0x10B9)):
        words.extend((0x0C40, command))  # CMPI.W #command,D0
        branch(0x6600, command)          # BNE.S next command
        absolute(opcode, VALUE + (command in (READ_BYTE, WRITE_BYTE)))
        if command != WRITE_BYTE:
            branch(0x6000, "done")
        labels[command] = len(words)
    labels["done"] = len(words)
    absolute(0x4279, COMMAND)  # CLR.W COMMAND
    branch(0x6000, "poll")
    for index, target in branches:
        offset = 2 * (labels[target] - index - 1)
        assert -128 <= offset <= 127 and offset != 0
        words[index] |= offset & 0xFF
    struct.pack_into(f">{len(words)}H", bios, 0x200, *words)
    (directory / "bios_CD_U.bin").write_bytes(bios)

    disc = bytearray(150 * 2048)
    disc[:14] = b"SEGADISCSYSTEM"
    path = directory / "original.iso"
    path.write_bytes(disc)
    return path


class CdCore(Core):
    def __init__(self, core_path, iso, capacity, connect_32x=True):
        lib = self.lib = C.CDLL(str(core_path))
        system_dir = str(iso.parent).encode()
        options = {
            b"genesis_plus_gx_region_detect": b"ntsc-u",
            b"genesis_plus_gx_add_on": b"none",
            b"genesis_plus_gx_32x_add_on": b"connected" if connect_32x else b"disconnected",
            OPTION: str(capacity).encode() if capacity else b"disabled",
        }
        advertised = {}

        @C.CFUNCTYPE(C.c_bool, U, P)
        def environment(command, data):
            command &= 0xFFFF
            if command in (9, 30, 31):
                C.cast(data, C.POINTER(C.c_char_p))[0] = system_dir
                return True
            if command == 15:
                variable = C.cast(data, C.POINTER(Var)).contents
                variable.value = options.get(variable.key)
                return variable.value is not None
            if command == 16:
                variables = C.cast(data, C.POINTER(Var))
                index = 0
                while variables[index].key:
                    variable = variables[index]
                    advertised[variable.key] = variable.value.split(b"; ", 1)[-1]
                    index += 1
                return True
            if command == 17:
                C.cast(data, C.POINTER(C.c_bool))[0] = False
                return True
            if command == 52:
                C.cast(data, C.POINTER(U))[0] = 0
                return True
            return command in (3, 10, 11, 18, 35, 37, 44, 45, 53, 54, 55)

        self.keep = {
            "environment": environment,
            "video_refresh": C.CFUNCTYPE(None, P, U, U, SZ)(lambda *_: None),
            "audio_sample": C.CFUNCTYPE(None, C.c_int16, C.c_int16)(lambda *_: None),
            "audio_sample_batch": C.CFUNCTYPE(SZ, P, SZ)(lambda _, count: count),
            "input_poll": C.CFUNCTYPE(None)(lambda: None),
            "input_state": C.CFUNCTYPE(C.c_int16, U, U, U, U)(lambda *_: 0),
        }
        for name, callback in self.keep.items():
            getattr(lib, "retro_set_" + name)(callback)
        lib.retro_load_game.argtypes, lib.retro_load_game.restype = [C.POINTER(Game)], C.c_bool
        lib.retro_get_memory_data.argtypes, lib.retro_get_memory_data.restype = [U], P
        lib.retro_get_memory_size.argtypes, lib.retro_get_memory_size.restype = [U], SZ
        lib.retro_kegarefusion_system_hw.restype = U
        lib.retro_serialize_size.restype = SZ
        for name in ("retro_serialize", "retro_unserialize"):
            function = getattr(lib, name)
            function.argtypes, function.restype = [P, SZ], C.c_bool

        lib.retro_init()
        assert advertised.get(OPTION) == b"disabled|4|8|16", advertised.get(OPTION)
        self.content_path = str(iso).encode()
        game = Game(self.content_path, None, 0, None)
        assert lib.retro_load_game(C.byref(game)), "original Sega CD ISO did not load"
        assert lib.retro_kegarefusion_system_hw() == 0x84, "Sega CD hardware path inactive"
        self.ram = lib.retro_get_memory_data(2)
        assert self.ram
        if connect_32x:
            self.grant()

    def mailbox(self, address):
        return C.c_uint16.from_address(self.ram + (address & 0xFFFF))

    def access(self, command, address, value=0):
        flag = self.mailbox(COMMAND)
        assert flag.value == 0
        self.mailbox(ADDRESS).value = address >> 16
        self.mailbox(ADDRESS + 2).value = address & 0xFFFF
        self.mailbox(VALUE).value = value
        flag.value = command
        for _ in range(4):
            self.frame()
            if flag.value == 0:
                return self.mailbox(VALUE).value
        raise AssertionError(f"Guest did not complete command {command} at 0x{address:06X}")

    def read(self, address, byte=False):
        return self.access(READ_BYTE if byte else READ_WORD, address)

    def write(self, address, value, byte=False):
        self.access(WRITE_BYTE if byte else WRITE_WORD, address, value)

    def expect(self, address, expected, description, byte=False):
        actual = self.read(address, byte)
        assert actual == expected, f"{description} at 0x{address:06X}: {actual:04X} != {expected:04X}"

    def grant(self):
        self.write(DREQ, 1)     # RV: grant the 68000 the direct cartridge bus.
        self.write(ADAPTER, 1)  # ADEN, keeping SH2s held in reset.
        assert self.read(ADAPTER) & 1, "32X adapter inactive"


def check_cart(core, capacity, states):
    low, high = WINDOW
    pages = capacity // 2
    initial_sram_size = core.lib.retro_get_memory_size(0)
    for page, address in enumerate(WINDOW):
        core.expect(address, 0xFFFF, "initial program RAM")
        core.write(address, 0x1000 + page)
    core.expect(low, 0x1000, "default window 0")
    core.expect(high, 0x1001, "default window 1")

    if capacity == 4:
        core.write(PAGE, 1, byte=True)
        core.write(PAGE + 2, 0)
        core.expect(low, 0x1000, "4 MiB ignores window 0 latch")
        core.expect(high, 0x1001, "4 MiB ignores window 1 latch")
        selected_low, selected_high = 0, 1
    else:
        for page in range(2, pages):
            core.write(PAGE, page, byte=True)
            core.expect(low, 0xFFFF, "untouched page")
            core.write(low, 0x1000 + page)
        selected_low, selected_high = 7 % pages, 2
        core.write(PAGE, 7, byte=True)
        core.expect(low, 0x1000 + selected_low, "8 MiB wraps page 7; 16 MiB selects it")
        core.write(PAGE + 2, 0x0205)
        core.expect(high, 0x1000 + selected_high, "word latch uses its high byte")
        core.write(PAGE + 1, 0, byte=True)
        core.write(PAGE + 3, 0, byte=True)
        core.write(PAGE + 4, 0, byte=True)
        core.write(PAGE + 6, 0)
        core.expect(low, 0x1000 + selected_low, "odd byte and CA2 writes ignored")
        core.expect(high, 0x1000 + selected_high, "odd byte and CA2 writes ignored")
        core.write(PAGE + 8, 1, byte=True)
        core.expect(low, 0x1001, "window 0 /TIME mirror")
        core.write(PAGE + 10, 0x0007)
        core.expect(high, 0x1000, "window 1 /TIME mirror")
        core.write(PAGE, selected_low, byte=True)
        core.write(PAGE + 2, selected_high, byte=True)

    core.expect(FIXED, 0xFFFF, "RV=1 disables fixed alias")
    core.expect(BANKED, 0xFFFF, "RV=1 disables banked alias")
    core.write(DREQ, 0)
    core.expect(low, 0xFFFF, "RV=0 disables direct window")
    core.write(low, 0xDEAD)
    core.write(PAGE, 1, byte=True)  # /TIME is blocked while RV=0.
    core.expect(FIXED, 0x1000 + selected_low, "fixed alias sees selected page")
    core.write(FIXED, 0xDEAD)
    core.write(FIXED + 1, 0xEF, byte=True)
    core.expect(FIXED, 0x1000 + selected_low, "fixed alias is read-only")
    core.write(BANK, 2)
    core.expect(BANKED, 0x1000 + selected_high, "banked alias sees window 1")
    core.write(BANKED, 0xCAFE)
    core.write(BANKED + 1, 0xEF, byte=True)
    core.expect(BANKED, 0xCAEF, "banked alias allows word and byte writes")
    core.expect(BANKED, 0xCA, "banked alias even byte lane", byte=True)
    core.expect(BANKED + 1, 0xEF, "banked alias odd byte lane", byte=True)
    core.write(BANK, 0)
    core.expect(BANKED, 0x1000 + selected_low, "ROM bank selects window 0 again")
    core.write(DREQ, 1)
    core.expect(high, 0xCAEF, "direct window sees banked alias writes")
    assert core.lib.retro_get_memory_size(0) == initial_sram_size, "program RAM exposed as SRAM"

    state = core.save()
    assert len(state) == len(states[0]) + 16 + capacity * MIB
    core.write(low, 0xDEAD)
    core.write(high, 0xBEEF)
    if capacity > 4:
        core.write(PAGE, 0, byte=True)
        core.write(PAGE + 2, 1, byte=True)
    for other_capacity, other_state in states.items():
        assert not core.restore(other_state), f"accepted {other_capacity} MiB state under {capacity} MiB"
    # A wrong capacity in a state of the correct size must also be rejected.
    bad_capacity = bytearray(state)
    header = len(state) - capacity * MIB - 16
    assert bad_capacity[header:header + 4] == b"RAMC"
    struct.pack_into("<I", bad_capacity, header + 4, capacity * MIB // 2)
    assert not core.restore(bytes(bad_capacity)), "accepted incorrect RAM capacity in state header"
    core.expect(low, 0xDEAD if capacity == 4 else 0x1000, "rejected states preserve live window 0")
    core.expect(high, 0xBEEF if capacity == 4 else 0x1001, "rejected states preserve live window 1")
    if capacity > 4:
        core.write(PAGE, selected_low, byte=True)
        core.write(PAGE + 2, selected_high, byte=True)
        core.expect(low, 0xDEAD, "rejected states preserve page data")
        core.expect(high, 0xBEEF, "rejected states preserve page data")
        core.write(PAGE, 0, byte=True)
        core.write(PAGE + 2, 1, byte=True)
    assert core.restore(state), f"{capacity} MiB state could not be restored"
    core.expect(low, 0x1000 + selected_low, "state restores window 0 latch and RAM")
    core.expect(high, 0xCAEF, "state restores window 1 latch and RAM")

    core.lib.retro_reset()
    core.grant()
    core.expect(low, 0x1000, "reset returns window 0 to page 0")
    core.expect(high, 0xCAEF if capacity == 4 else 0x1001, "reset returns window 1 to page 1")
    if capacity > 4:
        core.write(PAGE + 2, selected_high, byte=True)
        core.expect(high, 0xCAEF, "reset preserves program RAM")
    return state


def check_sh2_fetch(core):
    # Stage a tiny CD IPL in the framebuffer for the built-in SH-2 boot ROMs.
    # The master runs from SDRAM and either reads or calls 0x02000100.
    framebuffer_ipl = 0x840018
    comm = 0xA15120
    command, result = comm + 10, comm + 12
    program = (
        0xD207, 0xD308,          # MOV.L comm_command,R2; MOV.L cart_entry,R3
        0x6021, 0x2008, 0x89FC,  # poll: MOV.W @R2,R0; TST R0,R0; BT poll
        0x8801, 0x8B02,          # CMP/EQ #1,R0; BF call
        0x6031, 0xA002, 0x0009,  # MOV.W @R3,R0; BRA report; NOP
        0x430B, 0x0009,          # call: JSR @R3; NOP
        0x8121, 0xE000,          # report: MOV.W R0,@(2,R2); MOV #0,R0
        0xAFF2, 0x2201,          # BRA poll; MOV.W R0,@R2
        0x2000, 0x402A,          # comm_command
        0x0200, 0x0100,          # cart_entry
        0xAFFE, 0x0009,          # slave: BRA slave; NOP
    )
    entry, slave_entry, vbr = 0x06000000, 0x06000028, 0x06000100
    ipl = struct.pack(">8I", entry, 2 * len(program), entry, slave_entry, vbr, vbr, 0, 0)
    ipl += struct.pack(f">{len(program)}H", *program)
    for offset, (word,) in enumerate(struct.iter_unpack(">H", ipl)):
        core.write(framebuffer_ipl + 2 * offset, word)

    # Same SH-2 entry address, different return values in two cart pages.
    for page, opcode in ((1, 0xE033), (0, 0xE011)):
        core.write(PAGE, page, byte=True)
        for index, word in enumerate((opcode, 0x000B, 0x0009)):  # MOV #n,R0; RTS; NOP
            core.write(WINDOW[0] + 2 * index, word)
    core.write(comm, 0x5F43)      # "_CD_" handshake
    core.write(comm + 2, 0x445F)
    core.write(ADAPTER, 3)       # Release SH-2 reset.
    core.expect(comm, 0x4D5F, "master SH-2 completed CD startup")
    core.expect(comm + 4, 0x535F, "slave SH-2 completed CD startup")

    def request(operation, expected, description):
        core.write(result, 0)
        core.write(command, operation)
        core.expect(command, 0, "SH-2 acknowledged mailbox command")
        core.expect(result, expected, description)

    statistics = core.lib.retro_kegarefusion_core_stats
    statistics.argtypes, statistics.restype = [C.POINTER(C.c_uint64), SZ], C.c_int
    counts = (C.c_uint64 * 14)()
    request(1, 0xE011, "SH-2 reads RAM as data")
    request(1, 0xE011, "warm SDRAM translations")
    jit_active = statistics(counts, len(counts)) and counts[0] > 0
    misses = counts[12]
    for offset in range(8, 24, 2):
        core.write(WINDOW[0] + offset, offset)
    core.write(WINDOW[0], 0xE022)
    request(1, 0xE022, "SH-2 reads updated RAM data")
    if jit_active:
        assert statistics(counts, len(counts)) and counts[12] == misses, (
            "data-only cart writes invalidated SDRAM translations"
        )
        print("PASS RAM cartridge data writes preserve SDRAM JIT cache")

    request(2, 0x22, "SH-2 executes RAM cartridge code")
    core.write(WINDOW[0], 0xE044)
    request(2, 0x44, "SH-2 executes rewritten instruction")
    core.write(PAGE, 1, byte=True)
    request(2, 0x33, "SH-2 executes code from the new page")
    core.write(PAGE, 0, byte=True)
    request(2, 0x44, "SH-2 executes rewritten code when the first page returns")


def check_sub_cpu_addressing(core, allow_odd):
    # Main CPU stages an original Sub CPU program and its reset/exception vectors.
    prg_ram = 0x020000
    reset_busreq = 0xA12001
    comm = 0xA12020
    program = (
        0x33F9, 0x0000, 0x1001, 0x00FF, 0x8020,          # MOVE.W ($1001).L,($FF8020).L
        0x23F9, 0x0000, 0x1001, 0x00FF, 0x8022,          # MOVE.L ($1001).L,($FF8022).L
        0x33FC, 0xABCD, 0x0000, 0x1005,                  # MOVE.W #$ABCD,($1005).L
        0x23FC, 0xEFFE, 0x8001, 0x0000, 0x1009,          # MOVE.L #$EFFE8001,($1009).L
        0x13F9, 0x0000, 0x1001, 0x0000, 0x1007,          # MOVE.B ($1001).L,($1007).L
        0x33FC, 0x51A7, 0x00FF, 0x8028, 0x60FE,          # Report success; BRA.S to self.
    )
    vector_3 = (0x33FC, 3, 0x00FF, 0x8028, 0x60FE)  # Report Address Error; BRA.S to self.
    data = (0x1234, 0x5678, 0x4546, 0x789A, 0xFFE0, 0xFFE1, 0x6001)
    core.write(reset_busreq, 0, byte=True)
    for offset, words in (
        (0x0000, (0, 0x2000, 0, 0x0200)),  # Initial SP and PC.
        (0x000C, (0, 0x0300)),             # Address Error vector.
        (0x0200, program),
        (0x0300, vector_3),
        (0x1000, data),
    ):
        for index, word in enumerate(words):
            core.write(prg_ram + offset + 2 * index, word)
    core.write(reset_busreq, 1, byte=True)

    if allow_odd:
        core.expect(comm + 8, 0x51A7, "Sub CPU completed odd accesses")
        for offset, expected in ((0, 0x1234), (2, 0x1234), (4, 0x5678)):
            core.expect(comm + offset, expected, "Sub CPU rounds odd word and long reads down")
        expected_data = (0x1234, 0x5678, 0xABCD, 0x7834, 0xEFFE, 0x8001, 0x6001)
    else:
        core.expect(comm + 8, 3, "Sub CPU takes Address Error vector on an odd access")
        core.expect(comm, 0, "faulting Sub CPU read leaves destination unchanged")
        expected_data = data

    core.write(reset_busreq, 3, byte=True)  # Halt the Sub CPU so Main can read PRG RAM.
    for index, expected in enumerate(expected_data):
        core.expect(prg_ram + 0x1000 + 2 * index, expected, "Sub CPU writes and adjacent byte lanes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True, type=Path)
    args = parser.parse_args()
    core_path = args.core.resolve()
    states = {}
    with tempfile.TemporaryDirectory(prefix="cd32x-ram-cart-") as temporary:
        iso = make_disc(Path(temporary))
        for capacity in CAPACITIES:
            core = CdCore(core_path, iso, capacity)
            try:
                states[capacity] = check_cart(core, capacity, states) if capacity else core.save()
            finally:
                core.close()
            name = f"{capacity} MiB" if capacity else "disabled"
            print(f"PASS CD32X RAM cartridge {name}")
        core = CdCore(core_path, iso, 0)
        try:
            for capacity in CAPACITIES[1:]:
                assert not core.restore(states[capacity]), f"accepted {capacity} MiB state with RAM disabled"
            assert core.restore(states[0]), "disabled cartridge state could not be restored"
            print("PASS disabled cartridge rejects RAM states")
            check_sub_cpu_addressing(core, allow_odd=False)
            print("PASS Sub CPU Address Error with CD32X RAM disabled")
        finally:
            core.close()
        core = CdCore(core_path, iso, 8)
        try:
            check_sh2_fetch(core)
            print("PASS SH-2 RAM cartridge data and instruction coherence")
            check_sub_cpu_addressing(core, allow_odd=True)
            print("PASS CD32X RAM Sub CPU odd word, long and byte accesses")
        finally:
            core.close()

        core = CdCore(core_path, iso, 4, connect_32x=False)
        try:
            check_sub_cpu_addressing(core, allow_odd=False)
            print("PASS Sub CPU Address Error with 32X disconnected")
        finally:
            core.close()


if __name__ == "__main__":
    main()
