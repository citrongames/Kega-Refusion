#!/usr/bin/env python3
"""Focused real-core Link Cable checks, using only original generated ROM bytes.

Linux ELF symbols are resolved from this exact local binary with nm. No debug
API, ROM fingerprint, external game image, GUI, or production state layout is
added. All output is bounded; old native/audio snapshots are independent bytes.
Run: python3 tests/link_hardware_tests.py --core CORE [--output RESULT.json]
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

P, U, SZ = C.c_void_p, C.c_uint, C.c_size_t
ENV = C.CFUNCTYPE(C.c_bool, U, P)
VIDEO = C.CFUNCTYPE(None, P, U, U, SZ)
AUDIO = C.CFUNCTYPE(SZ, P, SZ)
SAMPLE = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
POLL = C.CFUNCTYPE(None)
INPUT = C.CFUNCTYPE(C.c_int16, U, U, U, U)
YIELD = C.CFUNCTYPE(None, P, U)

class Game(C.Structure):
    _fields_ = [("path", C.c_char_p), ("data", P), ("size", SZ), ("meta", C.c_char_p)]

class Variable(C.Structure):
    _fields_ = [("key", C.c_char_p), ("value", C.c_char_p)]

# Read only the prefix whose fixed-width fields precede jmp_buf in m68k.h.
class CpuPrefix(C.Structure):
    _fields_ = [
        ("mapping", P * (5 * 256)), ("poll", U * 3),
        ("cycles", C.c_int), ("refresh", C.c_int), ("cycle_end", U),
        ("dar", U * 16), ("pc", U), ("previous_pc", U),
        ("previous_dar", U * 16), ("sp", U * 5), ("ir", U),
        ("t1", U), ("supervisor", U), ("x", U), ("n", U),
        ("not_z", U), ("v", U), ("c", U), ("interrupt_mask", U),
        ("interrupt_level", U), ("stopped", U),
    ]

def require(ok, detail):
    if not ok:
        raise AssertionError(detail)

def fixture(stop=False):
    """Small original 68000 program: PSG tone/noise or STOP + IRQ2 counter."""
    rom = bytearray([0xff]) * 65536
    def long(at, value):
        rom[at:at + 4] = struct.pack(">I", value)
    long(0, 0x00ffff00)
    long(4, 0x200)
    long(0x68, 0x400)
    rom[0x100:0x110] = b"SEGA GENESIS    "
    rom[0x120:0x130] = b"LINK NATIVE TEST"
    rom[0x150:0x160] = b"LINK NATIVE TEST"
    rom[0x180:0x182] = b"GM"
    long(0x1a0, 0)
    long(0x1a4, len(rom) - 1)
    long(0x1a8, 0xff0000)
    long(0x1ac, 0xffffff)
    rom[0x1f0] = ord("U")
    program = []
    def word(*values):
        program.extend(values)
    def write_byte(value, address):
        word(0x13fc, value, address >> 16, address & 65535)
    def write_word(value, address):
        word(0x33fc, value, address >> 16, address & 65535)
    write_word(0x8004, 0xc00004)
    write_word(0x8144, 0xc00004)
    if stop:
        write_word(0x8b08, 0xc00004)
        write_byte(0x80, 0xa1000b)
        word(0x46fc, 0x2000)  # MOVE #$2000,SR
        word(0x4e72, 0x2000, 0x60fa)  # STOP; BRA.S STOP
        handler = [0x5279, 0x00ff, 0x0000, 0x4e73]  # ADDQ.W #1,($FF0000).L; RTE
        rom[0x400:0x408] = b"".join(struct.pack(">H", x) for x in handler)
    else:
        for value in (0x84, 0x13, 0x90, 0xe7, 0xf2):
            write_byte(value, 0xc00011)
        word(0x60fe)  # BRA.S self, SR remains 0x2700
    rom[0x200:0x200 + len(program) * 2] = b"".join(struct.pack(">H", x) for x in program)
    return bytes(rom)

class Core:
    def __init__(self, path, rom=None, audio_filter="disabled", fixture_extension=".md"):
        self.path = str(Path(path).resolve())
        self.library = C.CDLL(self.path)
        self.keep = []
        self.temporary = tempfile.TemporaryDirectory(prefix="kega-link-native-")
        self.directory = os.fsencode(self.temporary.name)
        self.options = {"genesis_plus_gx_bios": "disabled",
                        "genesis_plus_gx_region_detect": "ntsc-u",
                        "genesis_plus_gx_audio_filter": audio_filter}
        self.last_audio = b""
        self.last_video = b""
        @ENV
        def environment(command, data):
            command &= 65535
            if command in (9, 30, 31):
                C.cast(data, C.POINTER(C.c_char_p))[0] = self.directory
                return True
            if command == 15:
                variable = C.cast(data, C.POINTER(Variable)).contents
                value = self.options.get(variable.key.decode())
                if value is None:
                    return False
                encoded = value.encode()
                self.keep.append(encoded)
                variable.value = encoded
                return True
            if command == 16:
                variables = C.cast(data, C.POINTER(Variable))
                index = 0
                while variables[index].key:
                    item = variables[index]
                    self.options.setdefault(item.key.decode(), item.value.decode().split(";", 1)[-1].strip().split("|")[0])
                    index += 1
                return True
            if command == 17:
                C.cast(data, C.POINTER(C.c_bool))[0] = False
                return True
            if command == 47:
                C.cast(data, C.POINTER(C.c_int))[0] = 3
                return True
            if command == 51:
                return True
            if command == 52:
                C.cast(data, C.POINTER(U))[0] = 0
                return True
            return command in (3, 10, 11, 18, 35, 37, 44, 45, 53, 54, 55)
        @VIDEO
        def video(data, width, height, pitch):
            self.last_video = hashlib.sha256(C.string_at(data, height * pitch)).digest() if data else b""
        @AUDIO
        def audio(data, count):
            self.last_audio = C.string_at(data, count * 4)
            return count
        callbacks = [environment, video, audio, SAMPLE(lambda *_: None),
                     POLL(lambda: None), INPUT(lambda *_: 0)]
        names = ["environment", "video_refresh", "audio_sample_batch", "audio_sample",
                 "input_poll", "input_state"]
        self.keep.extend(callbacks)
        for name, callback in zip(names, callbacks):
            getattr(self.library, "retro_set_" + name)(callback)
        self.library.retro_load_game.argtypes = [C.POINTER(Game)]
        self.library.retro_load_game.restype = C.c_bool
        self.library.retro_serialize_size.restype = SZ
        for name in ("retro_serialize", "retro_unserialize",
                     "retro_kegarefusion_link_audio_save", "retro_kegarefusion_link_audio_load"):
            function = getattr(self.library, name)
            function.argtypes, function.restype = [P, SZ], C.c_bool
        self.library.retro_kegarefusion_link_audio_size.restype = SZ
        self.library.retro_kegarefusion_link_io.restype = C.POINTER(C.c_ubyte)
        self.library.retro_kegarefusion_link_connect.argtypes = [P, P, P]
        self.library.retro_kegarefusion_link_restore_latch.argtypes = [U]
        self.library.retro_kegarefusion_link_latch.restype = U
        self.library.retro_get_memory_data.argtypes = [U]
        self.library.retro_get_memory_data.restype = P
        self.library.retro_init()
        self.rom = fixture() if rom is None else rom
        self.buffer = C.create_string_buffer(self.rom)
        fixture_path = Path(self.temporary.name) / ("original-link" + fixture_extension)
        fixture_path.write_bytes(self.rom)
        game = Game(os.fsencode(fixture_path), C.cast(self.buffer, P), len(self.rom), None)
        require(self.library.retro_load_game(C.byref(game)), "original fixture load")
        self.symbols = {}
        output = subprocess.check_output(["nm", "-an", self.path], text=True)
        for line in output.splitlines():
            words = line.split()
            if len(words) == 3:
                try:
                    self.symbols[words[2]] = int(words[0], 16)
                except ValueError:
                    pass
        self.base = C.cast(self.library.retro_init, P).value - self.symbols["retro_init"]
        self.cpu = CpuPrefix.from_address(self.address("m68k"))
        self.reg = (C.c_ubyte * 32).from_address(self.address("reg"))
        self.hint = C.c_ubyte.from_address(self.address("hint_pending"))
        self.vint = C.c_ubyte.from_address(self.address("vint_pending"))
        self.pending = U.from_address(self.address("io_link_irq_pending"))
        self.vdp_level = U.from_address(self.address("io_link_vdp_irq"))
        self.set_irq = self.function("m68k_set_irq", None, U)
        self.update_irq = self.function("m68k_update_irq", None, U)
        self.ack = self.function("vdp_68k_irq_ack", C.c_int, C.c_int)
        self.read = self.function("io_68k_read", U, U)
        self.write = self.function("io_68k_write", None, U, U)
        get_reg = self.function("m68k_get_reg", U, U)
        require(self.cpu.pc == get_reg(16), "ELF CPU prefix PC layout")
        self.set_irq(6)
        require(self.cpu.interrupt_level == 0x600, "ELF CPU prefix IRQ layout")
        self.set_irq(0)
    def address(self, name):
        require(name in self.symbols, "local ELF symbol absent: " + name)
        return self.base + self.symbols[name]
    def function(self, name, result, *arguments):
        return C.CFUNCTYPE(result, *arguments)(self.address(name))
    def connect(self, peer, callback=None):
        callback = callback or YIELD(lambda *_: None)
        self.keep.extend([peer, callback])
        self.library.retro_kegarefusion_link_connect(C.cast(peer, P), C.cast(callback, P), None)
    def disconnect(self):
        self.library.retro_kegarefusion_link_connect(None, None, None)
    def frame(self):
        self.last_audio, self.last_video = b"", b""
        self.library.retro_run()
    def native_save(self):
        size = self.library.retro_serialize_size()
        buffer = C.create_string_buffer(size)
        require(self.library.retro_serialize(buffer, size), "native serialize")
        return buffer.raw
    def native_load(self, data):
        buffer = C.create_string_buffer(bytes(data))
        return bool(self.library.retro_unserialize(buffer, len(data)))
    def audio_save(self):
        size = self.library.retro_kegarefusion_link_audio_size()
        require(size == 496, "496-byte canonical audio extension")
        buffer = C.create_string_buffer(size)
        require(self.library.retro_kegarefusion_link_audio_save(buffer, size), "audio save at completed frame")
        return buffer.raw
    def audio_load(self, data):
        buffer = C.create_string_buffer(bytes(data))
        return bool(self.library.retro_kegarefusion_link_audio_load(buffer, len(data)))
    def snapshot(self):
        return self.native_save(), self.audio_save()
    def restore(self, state):
        require(self.native_load(state[0]), "native restore")
        require(self.audio_load(state[1]), "audio restore")
    def trace(self, count):
        rows = []
        nonzero = False
        for _ in range(count):
            self.frame()
            nonzero |= any(self.last_audio)
            rows.append((hashlib.sha256(self.last_audio).hexdigest(),
                         self.last_video.hex(), len(self.last_audio)))
        require(nonzero, "nonzero original PSG tone/noise")
        return rows
    def close(self):
        self.disconnect()
        self.library.retro_unload_game()
        self.library.retro_deinit()
        self.temporary.cleanup()

def hardware_checks(path):
    core = Core(path)
    try:
        peer = (C.c_ubyte * 16)()
        peer[2], peer[5] = 0x20, 0x20
        core.connect(peer)
        core.reg[0], core.reg[1] = 0x10, 0x20
        core.hint.value, core.vint.value = 0x10, 0
        core.set_irq(4)
        core.update_irq(2)
        require(core.cpu.interrupt_level == 0x400 and core.pending.value == 1,
                "pending external 2 plus HINT 4 selects 4, not 6")
        core.ack(4)
        require(core.cpu.interrupt_level == 0x200 and core.pending.value == 1,
                "ack4 retains pending external IRQ2")
        core.ack(2)
        require(core.cpu.interrupt_level == 0 and core.pending.value == 0, "ack2 clears external only")
        core.hint.value, core.vint.value = 0x10, 0x20
        core.set_irq(6)
        core.update_irq(2)
        core.ack(2)
        require((core.hint.value, core.vint.value, core.cpu.interrupt_level) == (0x10, 0x20, 0x600),
                "ack2 does not clear pending HINT/VINT")
        core.update_irq(2)
        core.ack(6)
        require(core.cpu.interrupt_level == 0x400 and core.hint.value == 0x10 and core.pending.value == 1,
                "ack6 retains HINT and external")
        core.ack(4)
        require(core.cpu.interrupt_level == 0x200, "ack4 after ack6 retains external")
        core.ack(2)

        # Direction, pullups, all low data bits, crossed TH/TR and unused bit7.
        peer[2], peer[5] = 0x2a, 0x2f
        core.write(5, 0)
        core.write(2, 0x80)
        require(core.read(2) == 0xfa, "low data bits + pulled TL + crossed high TR/TH + latched bit7")
        peer[2] = 0x0a
        require(core.read(2) == 0xba, "peer TR low becomes local TH low")
        peer[2], peer[5] = 0, 0
        require(core.read(2) == 0xff, "peer input-configured pins float high")
        core.write(5, 0x2f)
        core.write(2, 0x85)
        require(core.read(2) == 0xd5, "local output direction overrides cable peer")
        core.write(5, 0)
        core.write(2, 0)
        peer[2], peer[5] = 0, 0x40
        require(core.read(2) == 0x5f, "peer TH low becomes local TR low")

        gates = []
        for control, external, expected in ((0x80, 8, 1), (0, 8, 0), (0xc0, 8, 0), (0x80, 0, 0)):
            core.disconnect()
            peer[2], peer[5] = 0x20, 0x20
            core.connect(peer)
            core.reg[11] = external
            core.write(5, control)
            core.set_irq(0)
            peer[2] = 0
            core.read(2)
            require(core.pending.value == expected, "TH IRQ direction/HL/VDP gate")
            gates.append({"control": control, "external_enable": external, "pending": expected})
            if expected:
                core.ack(2)

        core.disconnect()
        core.reg[11] = 8
        peer[2], peer[5] = 0, 0x20
        core.connect(peer)  # Baseline is already low: no invented edge.
        core.write(5, 0x80)
        core.read(2)
        require(core.pending.value == 0, "connect at low TH invents no falling edge")
        core.set_irq(4)
        core.update_irq(2)
        saved_latch = core.library.retro_kegarefusion_link_latch()
        require(saved_latch == 0x402, "latch encodes TH, pending external and VDP separately")
        core.ack(2)
        core.set_irq(0)
        core.library.retro_kegarefusion_link_restore_latch(saved_latch)
        require(core.cpu.interrupt_level == 0x400 and core.pending.value == 1, "latch restore preserves priorities")
        core.ack(2)
        core.set_irq(0)
        core.read(2)
        require(core.pending.value == 0, "low baseline after restore creates no repeated edge")

        core.disconnect()
        core.set_irq(4)
        core.update_irq(2)
        require(core.cpu.interrupt_level == 0x600, "disconnected core IRQ behavior unchanged")
        core.set_irq(0)
        return {"priority_and_ack": True, "port_direction_pullups": True, "irq_gates": gates,
                "latch_restore": True, "disconnected_behavior_unchanged": True}
    finally:
        core.close()

def stop_check(path):
    core = Core(path, fixture(stop=True))
    try:
        peer = (C.c_ubyte * 16)()
        peer[2], peer[5] = 0x20, 0x20
        triggered = [False]
        @YIELD
        def edge_when_stopped(_, cycles):
            if core.cpu.stopped and not triggered[0]:
                peer[2] = 0
                triggered[0] = True
        core.connect(peer, edge_when_stopped)
        core.frame()
        ram = core.library.retro_get_memory_data(2)
        count = C.c_uint16.from_address(ram).value
        require(triggered[0] and count == 1, "external falling edge wakes STOP and reaches original IRQ2 handler exactly once")
        return {"stopped_edge_observed": True, "irq_handler_count": count}
    finally:
        core.close()

def audio_checks(path, mode):
    core = Core(path, audio_filter=mode)
    try:
        core.trace(41)
        state_a = core.snapshot()
        require(struct.unpack_from("<I", state_a[1], 12)[0] == {"disabled": 0, "low-pass": 1, "EQ": 2}[mode],
                "requested audio filter active")
        first_a = core.trace(60)
        state_b = core.snapshot()
        first_b = core.trace(24)
        core.trace(7)
        core.restore(state_a)
        require(core.trace(60) == first_a, "older independent audio snapshot replay is bit exact")
        core.restore(state_b)
        require(core.trace(24) == first_b, "newer independent audio snapshot replay is bit exact")
        core.restore(state_a)
        require(core.trace(60) == first_a, "old snapshot reusable after restoring newer snapshot")

        before = core.audio_save()
        require(not core.audio_load(before[:-1]) and core.audio_save() == before,
                "bad audio length rejected without mutation")
        malformed = bytearray(before)
        malformed[72:80] = bytes([255]) * 8  # Blip offset >= one complete sample.
        require(not core.audio_load(malformed) and core.audio_save() == before,
                "unsafe Blip offset rejected without mutation")
        malformed = bytearray(before)
        malformed[232:240] = struct.pack("<Q", 0x7ff8000000000000)
        require(not core.audio_load(malformed) and core.audio_save() == before,
                "nonfinite EQ history rejected without mutation")
        native = core.native_save()
        require(not core.native_load(native[:-1]) and core.native_save() == native,
                "bad native length rejected without mutation")
        return {"filter": mode, "extension_bytes": len(before), "independent_states": 2,
                "bit_exact_audio_and_video_frames": 144, "bad_payloads_rejected_without_mutation": 4,
                "audio_trace_sha256": hashlib.sha256(json.dumps(first_a).encode()).hexdigest()}
    finally:
        core.close()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True)
    parser.add_argument("--output")
    args = parser.parse_args()
    core_path = str(Path(args.core).resolve())
    result = {"core": core_path, "core_sha256": hashlib.sha256(Path(core_path).read_bytes()).hexdigest(),
              "fixture_sha256": hashlib.sha256(fixture()).hexdigest(),
              "scope": "Original generated ROM, local ELF inspection, no external game ROMs",
              "hardware": hardware_checks(core_path), "stop": stop_check(core_path),
              "audio": [audio_checks(core_path, mode) for mode in ("disabled", "low-pass", "EQ")],
              "pass": True}
    if args.output:
        destination = Path(args.output)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))

if __name__ == "__main__":
    main()
