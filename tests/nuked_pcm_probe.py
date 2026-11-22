#!/usr/bin/env python3
"""Hash raw libretro PCM with fixed inputs, independent of frontend playback."""
import argparse
import ctypes as C
from contextlib import nullcontext
import hashlib
import json
import struct
import sys
import tempfile
from pathlib import Path

from sms_region_tests import AV, Game, P, SZ, U, Var

OVERRIDES = {
    "genesis_plus_gx_system_hw": "auto",
    "genesis_plus_gx_region_detect": "ntsc-u",
    "genesis_plus_gx_bios": "disabled",
    "genesis_plus_gx_add_on": "none",
    "genesis_plus_gx_32x_add_on": "disconnected",
    "genesis_plus_gx_ym2612": "nuked (ym2612)",
    "genesis_plus_gx_sound_output": "stereo",
    "genesis_plus_gx_audio_filter": "disabled",
    "genesis_plus_gx_fm_preamp": "100",
    "genesis_plus_gx_psg_preamp": "150",
}


def run(core_path, rom_path, frame_count, directory, pcm_file):
    defaults = {}
    used = {}
    settings = {k: v.encode() for k, v in OVERRIDES.items()}
    directory = str(directory).encode()
    lib = C.CDLL(str(core_path))
    chunk = bytearray()

    @C.CFUNCTYPE(C.c_bool, U, P)
    def env(command, data):
        command &= 0xFFFF
        if command in (9, 30, 31):
            C.cast(data, C.POINTER(C.c_char_p))[0] = directory
            return True
        if command == 15:
            var = C.cast(data, C.POINTER(Var)).contents
            key = var.key.decode()
            var.value = settings.get(key)
            used[key] = var.value.decode() if var.value is not None else None
            return var.value is not None
        if command == 16:
            variables = C.cast(data, C.POINTER(Var))
            index = 0
            while variables[index].key:
                var = variables[index]
                key = var.key.decode()
                val = var.value.split(b"; ", 1)[-1].split(b"|")
                defaults[key] = val[0].decode()
                settings.setdefault(key, val[0])
                index += 1
            return True
        if command == 17:
            C.cast(data, C.POINTER(C.c_bool))[0] = False
            return True
        if command == 52:
            C.cast(data, C.POINTER(U))[0] = 0
            return True
        return command in (3, 10, 11, 18, 35, 37, 44, 45, 53, 54, 55)

    @C.CFUNCTYPE(SZ, P, SZ)
    def samples(data, count):
        chunk.extend(C.string_at(data, count * 4))
        return count

    @C.CFUNCTYPE(None, C.c_int16, C.c_int16)
    def sample(left, right):
        chunk.extend(struct.pack("<hh", left, right))

    callbacks = {
        "environment": env,
        "video_refresh": C.CFUNCTYPE(None, P, U, U, SZ)(lambda *_: None),
        "audio_sample": sample,
        "audio_sample_batch": samples,
        "input_poll": C.CFUNCTYPE(None)(lambda: None),
        "input_state": C.CFUNCTYPE(C.c_int16, U, U, U, U)(lambda *_: 0),
    }
    for name, callback in callbacks.items():
        getattr(lib, "retro_set_" + name)(callback)
    lib.retro_load_game.argtypes = [C.POINTER(Game)]
    lib.retro_load_game.restype = C.c_bool
    lib.retro_get_system_av_info.argtypes = [C.POINTER(AV)]
    lib.retro_get_region.restype = U
    lib.retro_kegarefusion_ym2612_backend.restype = C.c_int
    lib.retro_kegarefusion_system_hw.restype = U
    lib.retro_init()
    rom = rom_path.read_bytes()
    raw = C.create_string_buffer(rom)
    path = str(rom_path).encode()
    game = Game(path, C.cast(raw, P), len(rom), None)
    assert lib.retro_load_game(C.byref(game)), f"Could not load {rom_path.name}"
    try:
        assert lib.retro_kegarefusion_system_hw() == 0x80
        assert lib.retro_kegarefusion_ym2612_backend() == 1, "Nuked YM2612 not active"
        av = AV()
        lib.retro_get_system_av_info(C.byref(av))
        whole_hash = hashlib.sha256()
        frame_hashes = []
        sample_frames = 0
        audible_frames = 0
        for frame in range(frame_count):
            chunk.clear()
            lib.retro_run()
            assert len(chunk) % 4 == 0
            frame_hashes.append(hashlib.sha256(chunk).hexdigest())
            whole_hash.update(chunk)
            if pcm_file is not None:
                pcm_file.write(chunk)
            sample_frames += len(chunk) // 4
            audible_frames += bool(chunk.strip(b"\0"))
            if (frame + 1) % 3600 == 0:
                print(f"{core_path.parent.name} {frame + 1}/{frame_count}", file=sys.stderr, flush=True)
        assert audible_frames
        return {
            "core": str(core_path), "core_sha256": hashlib.sha256(core_path.read_bytes()).hexdigest(),
            "rom": rom_path.name, "rom_sha256": hashlib.sha256(rom).hexdigest(),
            "frames": frame_count, "input": "none", "region": lib.retro_get_region(),
            "fps": av.timing.fps, "sample_rate": av.timing.rate,
            "sample_frames_stereo_pcm16": sample_frames, "nonzero_pcm_frames": audible_frames,
            "ym2612_backend": lib.retro_kegarefusion_ym2612_backend(),
            "overrides": OVERRIDES, "advertised_defaults": defaults, "queried_options": used,
            "pcm_sha256": whole_hash.hexdigest(), "per_frame_pcm_sha256": frame_hashes,
        }
    finally:
        lib.retro_unload_game()
        lib.retro_deinit()


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--core", type=Path, required=True)
parser.add_argument("--rom", type=Path, required=True)
parser.add_argument("--frames", type=int, default=32400)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument('--pcm-dump', type=Path, help='Optional streamed s16le stereo PCM; bounded by --frames (maximum 36000)')
args = parser.parse_args()
if not 1 <= args.frames <= 36000:
    parser.error("--frames must be between 1 and 36000")
args.output.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="nuked-pcm-", dir=args.output.resolve().parent) as directory:
    with args.pcm_dump.open('wb') if args.pcm_dump else nullcontext(None) as pcm_file:
        result = run(args.core.resolve(), args.rom.resolve(), args.frames, directory, pcm_file)
args.output.write_text(json.dumps(result, indent=2) + "\n")
print({k: result[k] for k in ("frames", "fps", "sample_rate", "sample_frames_stereo_pcm16", "pcm_sha256")})
