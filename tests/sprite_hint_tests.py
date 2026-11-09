#!/usr/bin/env python3
"""Real-core native MD ownership checks, original generated 68000 ROM only.
Run: python3 tests/sprite_hint_tests.py --core CORE --output RESULT.json
"""
import argparse
import ctypes as C
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from sms_region_tests import Core, Game, P, U, SZ


def fixture(shadow=False, wide=False, im2=False, leftblank=False, layout=None):
    raw = bytearray(65536)
    def long_at(at, value): raw[at:at+4] = struct.pack('>I', value)
    long_at(0, 0xfffff0); long_at(4, 0x200)
    raw[0x100:0x110] = b'SEGA GENESIS    '
    raw[0x120:0x130] = b'SPRITE HINT TEST'
    long_at(0x1a4, len(raw)-1); long_at(0x1a8, 0xff0000); long_at(0x1ac, 0xffffff)
    raw[0x1f0] = ord('U')
    pc = 0x200
    def w(value):
        nonlocal pc
        raw[pc:pc+2] = struct.pack('>H', value & 65535); pc += 2
    def addr(value): w(value >> 16); w(value)
    def mw(value, address): w(0x33fc); w(value); addr(address)
    def ml(value, address): w(0x23fc); addr(value); addr(address)
    def reg(index, value): mw(0x8000 | (index << 8) | value, 0xc00004)
    def vram(address): ml(0x40000000 | ((address & 0x3fff) << 16) | (address >> 14), 0xc00004)
    def fill(address, count, value):
        nonlocal pc
        vram(address); w(0x303c); w(count-1); loop = pc; mw(value, 0xc00000); w(0x51c8); w(loop-pc)
    w(0x46fc); w(0x2700); reg(0, 0x24 if leftblank else 4); reg(1, 4); reg(15, 2)
    reg(12, (1 if wide else 0) | (8 if shadow else 0) | (6 if im2 else 0))
    reg(2, 0x30); reg(3, 0x2c); reg(4, 7); reg(5, 0x68); reg(7, 0); reg(13, 0x3c)
    reg(16, 0); reg(17, 0); reg(18, 0)
    fill(0, 32768, 0)
    # Interlace mode 2 patterns contain sixteen scanlines.
    tile_bytes = 64 if im2 else 32
    for tile in (1, 2, 3, 4, 5, 6, 7, 14, 15):
        color = 4 if tile in (5, 6, 7) else tile
        fill(tile*tile_bytes, tile_bytes//2, color*0x1111)
    ml(0xc0000000, 0xc00004)
    for palette in range(4):
        for color in range(16): mw((0, 0x000e, 0x00e0, 0x0e00, 0x0eee)[min(color, 4)], 0xc00000)
    fill(0xc000, 1024, 1); fill(0xe000, 1024, 2)
    for x in (48, 64, 80):
        # Cover both tile rows occupied by 8x16 IM2 sprites.
        for row in range(4, 6): vram(0xc000 + 2*(row*32 + x//8)); mw(0x8001, 0xc00000)
    entries = [(0,4), (32,4), (48,4), (64,0x8004), (80,4), (80,0x8003),
               (96,0), (112,1), (128,0x600e), (144,0x600f), (160,0x6004),
               (208,0), (208,3)]
    entries = [(x, attr, 0) for x, attr in entries]
    if layout == 'pixel_budget':
        entries = [(-120, 4, 0x0c)]*7 + [(32, 4, 0), (64, 0x8004, 0x0c)]
    elif layout == 'sprite_mask':
        entries = [(32, 4, 0), (-128, 4, 0), (64, 4, 0)]
    elif layout == 'left_clip':
        entries = [(-4, 4, 0)]
    elif layout == 'sat_limit':
        entries = [(-120, 4, 0)]*16 + [(32, 4, 0)]
    vram(0xd000)
    for index, (x, attr, size) in enumerate(entries):
        link = index+1 if index+1 < len(entries) else 0
        for value in (0x140 if im2 else 0xa0, (size << 8) | link, attr, 0x80+x): mw(value, 0xc00000)
    reg(1, 0x44); w(0x60fe)
    return bytes(raw)


def run_case(core_path, case):
    raw = fixture(shadow=case.get('shadow', False), wide=case.get('wide', False),
                  im2=case.get('im2', False), leftblank=case.get('leftblank', False), layout=case.get('layout'))
    if case.get('hardware') == 'sms':
        data = bytearray(32768); data[:6] = bytes.fromhex('f331f0df18fe')
        data[0x7ff0:0x7ff8] = b'TMR SEGA'; data[0x7fff] = 0x4c
        raw = bytes(data)
    with tempfile.TemporaryDirectory(prefix='sprite-hint-') as directory:
        rom = Path(directory)/('original.sms' if case.get('hardware') == 'sms' else 'original.md'); rom.write_bytes(raw)
        core = Core(core_path, raw, str(rom), options=case.get('options'))
        lib = core.lib
        if case.get('hardware') == 'cd':
            # Execute our same original 68000 sprite setup as a synthetic CD
            # bootstrap. No retail BIOS, security program or game bytes used.
            lib.retro_unload_game()
            (Path(directory)/'bios_CD_U.bin').write_bytes(raw.ljust(128*1024, b'\0'))
            disc = bytearray(150*2048); signature = b'SEGADISCSYSTEM'
            disc[:len(signature)] = signature
            iso = Path(directory)/'original.iso'; iso.write_bytes(disc)
            old_environment = core.keep[0]
            encoded_directory = str(Path(directory)).encode()
            @C.CFUNCTYPE(C.c_bool, U, P)
            def cd_environment(command, data):
                if (command & 65535) in (9, 30, 31):
                    C.cast(data, C.POINTER(C.c_char_p))[0] = encoded_directory
                    return True
                return old_environment(command, data)
            core.keep.append(cd_environment); lib.retro_set_environment(cd_environment)
            path = str(iso).encode(); core.keep.append(path)
            game = Game(path, None, 0, None)
            assert lib.retro_load_game(C.byref(game)), 'original CD fixture load'
            assert lib.retro_kegarefusion_system_hw() == 0x84, 'not actual Sega CD hardware path'
        lib.retro_kegarefusion_sprite_hint_enable.argtypes = [C.c_bool]
        lib.retro_kegarefusion_sprite_hint.argtypes = [C.POINTER(U), C.POINTER(U), C.POINTER(SZ)]
        lib.retro_kegarefusion_sprite_hint.restype = P
        lib.retro_kegarefusion_debug_masks.argtypes = [C.c_uint32, C.c_uint64]
        allocation = C.c_void_p.from_address(core.base + core.syms['kega_sprite_hint_bitmap'])
        latest = {}
        def hint():
            width, height, pitch = U(), U(), SZ()
            ptr = lib.retro_kegarefusion_sprite_hint(C.byref(width), C.byref(height), C.byref(pitch))
            return (C.string_at(ptr, pitch.value * height.value) if ptr else None,
                    width.value, height.value, pitch.value)
        @C.CFUNCTYPE(None, P, U, U, SZ)
        def video(data, width, height, pitch):
            latest['video'] = (C.string_at(data, pitch*height) if data else None, width, height, pitch)
            latest['hint_in_callback'] = hint()
        lib.retro_set_video_refresh(video); core.keep.append(video)
        for _ in range(30): core.frame()
        assert hint()[0] is None, 'disabled core published ownership'
        assert not allocation.value, 'disabled core allocated ownership raster'
        initial = core.save()
        assert core.restore(initial)
        for _ in range(2): core.frame()
        off_state = core.save(); off_video = latest['video']
        assert core.restore(initial)
        lib.retro_kegarefusion_sprite_hint_enable(True)
        assert hint()[0] is None, 'enable published stale ownership'
        assert not allocation.value, 'enable allocated before a supported frame'
        for _ in range(2): core.frame()
        on_state = core.save(); on_video = latest['video']; raster = hint()
        assert off_state == on_state, 'hint altered emulated serialized state'
        assert off_video == on_video, 'hint altered emulated colors'
        checks = ['disabled_null', 'enable_invalidates', 'native_state_invariant', 'color_invariant']
        if case.get('unsupported'):
            assert raster[0] is None, 'unsupported output published native ownership'
            assert not allocation.value, 'unsupported-only session allocated ownership raster'
            checks.append('unsupported_output_null')
        else:
            assert raster == latest['hint_in_callback'], 'ownership was not current in video callback'
            mask, width, height, pitch = raster
            assert mask is not None and (width, height) == on_video[1:3] and pitch == 720, 'sidecar geometry mismatch'
            assert set(mask) <= {0, 1}, 'non-binary ownership'
            if case.get('im2'): assert height == 448, 'IM2 double field geometry'
            y = 66 if case.get('im2') else 33
            expected = {1: 0 if case.get('leftblank') else 1, 33:1, 49:0, 65:1, 81:0, 97:0,
                        113:1, 129:0 if case.get('shadow') else 1,
                        145:0 if case.get('shadow') else 1, 161:1, 209:1}
            if case.get('layout'):
                expected = case['expected']
                expected = {int(x):value for x, value in expected.items()}
            for x, value in expected.items():
                actual = mask[y*pitch+x]
                assert actual == value, f'{case["name"]} mask at ({x},{y}) is {actual}, expected {value}'
            checks += ['callback_current', 'geometry_exact', 'binary_raster']
            if case.get('layout'):
                checks.append(case['layout'])
            else:
                checks += ['visible_sprite', 'high_plane_occlusion', 'high_sprite_priority',
                           'first_opaque_wins', 'transparent_sprite', 'same_color_owned',
                           'transparent_then_opaque']
                if case.get('shadow'): checks.append('shadow_highlight_operators')
            if case.get('hardware') == 'cd': checks.append('actual_cd_frame_path')
            # Debug sprite hiding must remove ownership without changing collision state.
            assert core.restore(initial); lib.retro_kegarefusion_debug_masks(8, 0); core.frame()
            assert hint()[0] is not None and not any(hint()[0]), 'hidden sprite ownership remained'
            checks.append('debug_hidden_sprites_zero')
            lib.retro_kegarefusion_debug_masks(0, 0)
        saved = core.save(); assert core.restore(saved); assert hint()[0] is None
        checks.append('unserialize_invalidates')
        core.frame(); lib.retro_reset(); assert hint()[0] is None; checks.append('reset_invalidates')
        # Unloading releases the raster but keeps the explicit preference armed.
        lib.retro_unload_game(); assert not allocation.value and hint()[0] is None
        checks.append('unload_frees')
        if case.get('hardware') != 'cd':
            path = str(rom).encode(); core.keep.append(path)
            game = Game(path, C.cast(core.raw, P), len(raw), None)
        assert lib.retro_load_game(C.byref(game)), 'reload original fixture'
        assert not allocation.value, 'load allocated before supported frame'
        for _ in range(30): core.frame()
        assert bool(hint()[0]) != bool(case.get('unsupported')), 'unload lost requested preference'
        checks.append('requested_preference_survives_reload')
        lib.retro_kegarefusion_sprite_hint_enable(False)
        assert hint()[0] is None and not allocation.value; checks.append('disable_frees')
        core.close(); assert hint()[0] is None; checks.append('deinit_null')
        return {'case': case['name'], 'pass': True, 'checks': checks,
                'fixture_sha256': hashlib.sha256(raw).hexdigest(), 'geometry': list(on_video[1:3])}


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--core', required=True); parser.add_argument('--output'); parser.add_argument('--case')
    args = parser.parse_args()
    if args.case:
        print(json.dumps(run_case(args.core, json.loads(args.case)))); return
    cases = [{'name':'normal_h32'}, {'name':'cd_native_vdp', 'hardware':'cd'}, {'name':'shadow_highlight', 'shadow':True},
             {'name':'normal_h40', 'wide':True}, {'name':'left_column_blank', 'leftblank':True},
             {'name':'im2_weave', 'im2':True, 'options':{'genesis_plus_gx_render':'double field'}},
             {'name':'pixel_budget', 'layout':'pixel_budget', 'expected':{33:1,65:1,81:1,89:0}},
             {'name':'sprite_mask', 'layout':'sprite_mask', 'expected':{33:1,65:0}},
             {'name':'left_clip', 'layout':'left_clip', 'expected':{0:1,3:1,4:0}},
             {'name':'sat_limit', 'layout':'sat_limit', 'expected':{33:0}},
             {'name':'sms_suppressed', 'hardware':'sms', 'unsupported':True},
             {'name':'blargg_suppressed', 'unsupported':True, 'options':{'genesis_plus_gx_blargg_ntsc_filter':'composite'}}]
    results = []
    for case in cases:
        child = subprocess.run([sys.executable, __file__, '--core', args.core, '--case', json.dumps(case)], text=True, capture_output=True)
        if child.returncode: raise RuntimeError(child.stderr + child.stdout)
        results.append(json.loads(child.stdout))
    result = {'pass': True, 'core_sha256':hashlib.sha256(Path(args.core).read_bytes()).hexdigest(), 'results':results}
    if args.output: Path(args.output).write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({'pass':True, 'cases':len(results)}))


if __name__ == '__main__': main()
