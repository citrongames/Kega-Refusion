#!/usr/bin/env python3
"""Restore ordinary SG-1000 video, PCM and native state from an original cartridge."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from sms_region_tests import Core, P, SZ, U

VDP_DATA = 0xBE
VDP_CONTROL = 0xBF
PSG = 0x7F
COUNTER = 0xC000
SAVE_COUNTER = 3
CONTINUATION = 6
MUTATED = 13
FRONTEND_AUDIO_VIDEO = 47
FRAME_COMPONENTS = ("video", "pcm", "native_state")


def out(port, value):
    return bytes((0x3E, value, 0xD3, port))  # LD A,value; OUT (port),A


def register(index, value):
    return out(VDP_CONTROL, value) + out(VDP_CONTROL, 0x80 | index)


def vram_address(address):
    return out(VDP_CONTROL, address & 0xFF) + out(VDP_CONTROL, 0x40 | (address >> 8))


def read_counter():
    return b"\x3a" + struct.pack("<H", COUNTER)  # LD A,(COUNTER)


def on_counter(frame, commands):
    assert 0 < len(commands) <= 127
    # Compare and skip the following block until the specified VBlank.
    return read_counter() + bytes((0xFE, frame, 0x20, len(commands))) + commands


def cartridge(graphics2):
    rom = bytearray(0x8000)
    code = bytearray(b"\xf3\x31\xf0\xc3\xaf\x32" + struct.pack("<H", COUNTER))
    data_offset = 0x1000

    def upload(address, data):
        nonlocal data_offset
        for start in range(0, len(data), 256):
            chunk = data[start:start + 256]
            # OTIR streams HL to port C, with B=0 transferring 256 bytes.
            code.extend(vram_address(address + start))
            code.extend(b"\x21" + struct.pack("<H", data_offset))
            code.extend(bytes((0x01, VDP_DATA, len(chunk) & 0xFF, 0xED, 0xB3)))
            rom[data_offset:data_offset + len(chunk)] = chunk
            data_offset += len(chunk)

    # Enable 16 KiB addressing with display blank before initializing VRAM.
    code.extend(register(1, 0x80))
    for index, value in ((0, 2 if graphics2 else 0), (2, 6), (3, 0xFF if graphics2 else 0x80),
                         (4, 3 if graphics2 else 0), (5, 0x36), (6, 7), (7, 0x0D)):
        code.extend(register(index, value))
    tiles = bytes.fromhex("00000000000000008142241818244281f0f00f0ff0f00f0f")
    for page in range(3):
        upload(0x0800 * page, tiles)
        colors = bytes((0xC0 + 0x10 * page,)) * 24
        upload(0x2000 + 0x0800 * page, colors)
    upload(0x1800, bytes((col + row) % 3 for row in range(24) for col in range(32)))
    upload(0x3800, bytes.fromhex("ff8181bdadbdbdff183c7effff7e3c18"))
    # Sprite 0 touches the first displayed scanline. A second sprite tests color/priority.
    upload(0x1B00, bytes((0xFF, 25, 0, 8, 4, 31, 1, 15, 0xD0)))
    code.extend(register(1, 0xC0))
    code.extend(out(PSG, 0x84) + out(PSG, 0x04) + out(PSG, 0x92))

    vblank = len(code)
    # IN A,(VDP_CONTROL); BIT 7,A; JR Z,vblank.
    code.extend(bytes((0xDB, VDP_CONTROL, 0xCB, 0x7F, 0x28, 0xFA)))
    code.extend(read_counter() + b"\x3c\x32" + struct.pack("<H", COUNTER))
    # Write R7=0 once, after the save boundary. Fifty/sixty-hertz paths
    # both arrive here via the guest's real VBlank status polls.
    code.extend(on_counter(4, register(7, 0x00)))
    # Let the stale backdrop affect several rendered frames before another change.
    code.extend(on_counter(8, register(7, 0x03)))
    # A later dissimilar scene makes advancing in place distinct from a restore.
    code.extend(on_counter(11, register(0, 0 if graphics2 else 2)
                           + register(3, 0x80 if graphics2 else 0xFF)
                           + register(4, 0 if graphics2 else 3)
                           + register(7, 0x09)))
    code.extend(vram_address(0x1B01) + read_counter()
                + bytes((0x87, 0xC6, 25, 0xD3, VDP_DATA)))  # ADD A,A; ADD A,25; OUT
    code.extend(read_counter() + bytes((0xE6, 0x0F, 0xF6, 0x80, 0xD3, PSG)))
    code.extend(b"\xc3" + struct.pack("<H", vblank))
    assert len(code) < 0x1000
    rom[:len(code)] = code
    return bytes(rom)


class Output:
    def __init__(self, core):
        self.core = core
        self.frame = None
        self.audio = bytearray()
        self.fast = False
        self.audio_enabled = True
        env = core.keep[0]
        @type(env)
        def environment(command, data):
            if (command & 0xFFFF) == FRONTEND_AUDIO_VIDEO:
                flags = 1 | (2 if self.audio_enabled else 0) | (4 if self.fast else 0)
                C.cast(data, C.POINTER(C.c_int))[0] = flags
                return True
            return env(command, data)

        @C.CFUNCTYPE(None, P, U, U, SZ)
        def video(data, width, height, pitch):
            if data:
                rows = (C.string_at(data + y * pitch, 2 * width) for y in range(height))
                self.frame = (width, height, b"".join(rows))

        @C.CFUNCTYPE(SZ, P, SZ)
        def audio(data, samples):
            if self.audio_enabled:
                self.audio.extend(C.string_at(data, samples * 4))
            return samples

        for name, callback in (("environment", environment), ("video_refresh", video),
                               ("audio_sample_batch", audio)):
            setter = getattr(core.lib, "retro_set_" + name)
            setter.argtypes = [type(callback)]
            setter(callback)
            core.keep.append(callback)

        self.rewind = core.lib.retro_kegarefusion_rewind_video
        self.rewind.argtypes, self.rewind.restype = [U], U
        core.lib.retro_get_memory_data.argtypes = [U]
        core.lib.retro_get_memory_data.restype = P
        ram = core.lib.retro_get_memory_data(2)
        assert ram, "Core did not expose SG work RAM"
        self.counter = C.c_uint8.from_address(ram)

    def advance(self):
        self.frame = None
        self.audio.clear()
        self.core.frame()

    def to_counter(self, target):
        for _ in range(24):
            if self.counter.value == target:
                return
            self.advance()
        raise AssertionError(f"Guest missed VBlank checkpoint {target}")

    def save(self, fast=False):
        self.fast = fast
        try:
            return self.core.save()
        finally:
            self.fast = False

    def restore(self, state, fast=False):
        self.fast = fast
        try:
            if not self.core.restore(state):
                raise AssertionError("Native state was rejected")
        finally:
            self.fast = False

    def step(self):
        self.advance()
        assert self.frame and self.audio, "Missing raw video or PCM"
        # A slow capture leaves the fast save's separate sound buffer intact.
        return self.frame, bytes(self.audio), self.save()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def check_continuation(output, expected):
    frames = []
    for reference in expected:
        actual = output.step()
        frames.append([part for part, before, after in zip(FRAME_COMPONENTS, reference, actual)
                       if before != after])
    return frames


def run_case(path, case):
    hw, region, graphics2, fast = case
    options = {
        "genesis_plus_gx_system_hw": hw,
        "genesis_plus_gx_region_detect": region,
        "genesis_plus_gx_bios": "disabled",
        "genesis_plus_gx_overscan": "full",
    }
    raw = cartridge(graphics2)
    with tempfile.TemporaryDirectory(prefix="sg-state-video-") as temp:
        rom = Path(temp) / "original.sg"
        rom.write_bytes(raw)
        core = Core(str(path), raw, str(rom), options)
        try:
            out = Output(core)
            stats = core.stats()
            assert stats["hardware"] == (1 if hw == "sg-1000" else 2)
            assert bool(stats["vdp_pal"]) == (region == "pal")
            out.to_counter(SAVE_COUNTER)
            assert out.frame and out.frame[2][:2] != b"\x00\x00", "Missing colored backdrop"
            boundary_picture = out.frame
            initial = out.save(fast)
            expected = [out.step() for _ in range(CONTINUATION)]
            assert len({sha(frame[2]) for frame, _, _ in expected}) >= 3, "Picture failed to animate"
            assert len({frame[2][:2] for frame, _, _ in expected}) >= 2, "Backdrop did not change"
            assert all(any(pcm) for _, pcm, _ in expected), "PSG produced silence"
            out.to_counter(MUTATED)
            assert out.frame != boundary_picture, "Fixture failed to change the scene"

            out.restore(initial, fast)
            direct_boundary = out.save() == initial
            direct = check_continuation(out, expected)

            out.to_counter(MUTATED)
            out.restore(initial, fast)
            # Mirror the frontend's restore, silent render and restore sequence.
            out.rewind(1)
            out.audio_enabled = False
            out.advance()
            out.rewind(3)
            assert out.frame and not out.audio
            preview = out.frame == expected[0][0]
            out.restore(initial, fast)
            out.rewind(2)
            out.audio_enabled = True
            preview_boundary = out.save() == initial
            resumed = check_continuation(out, expected)
        finally:
            core.close()
    return {
        "case": case,
        "hardware": stats,
        "rom_sha256": sha(raw),
        "start_counter": SAVE_COUNTER,
        "mutated_counter": MUTATED,
        "direct_boundary": direct_boundary,
        "direct": direct,
        "preview": preview,
        "preview_boundary": preview_boundary,
        "resumed": resumed,
        "pass": direct_boundary and preview and preview_boundary and
                not any(direct + resumed),
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--core", type=Path, required=True)
    p.add_argument("--output", type=Path)
    p.add_argument("--case", help=argparse.SUPPRESS)
    args = p.parse_args()
    if args.case:
        print(json.dumps(run_case(args.core.resolve(), json.loads(args.case))))
        return 0

    cases = [
        (hardware, region, graphics2, fast)
        for hardware, graphics2 in (("sg-1000", False), ("sg-1000 II", True))
        for region in ("ntsc-u", "pal")
        for fast in (False, True)
    ]
    rows = []
    for case in cases:
        run = subprocess.run(
            [sys.executable, "-B", __file__, "--core", str(args.core), "--case", json.dumps(case)],
            text=True, capture_output=True, check=False,
        )
        if run.returncode:
            raise RuntimeError(f"{case}\n{run.stderr}")
        rows.append(json.loads(run.stdout))
    result = {"core_sha256": sha(args.core.read_bytes()), "cases": rows, "pass": all(r["pass"] for r in rows)}
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    for row in rows:
        status = "PASS" if row["pass"] else "FAIL"
        print(f"{status} {row['case']} direct={row['direct']} resumed={row['resumed']} preview={row['preview']}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
