#!/usr/bin/env python3
"""Original Mode 4 ROMs: status must not depend on frame skipping or LCD crop.
Run: python3 tests/sms_vdp_status_tests.py --core CORE --output RESULTS.json
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from sms_region_tests import Core


def fixture(case):
    raw = bytearray(32768)
    raw[:3] = bytes.fromhex('c30001')
    # IM1: acknowledge VDP, save status, increment RAM counter if the flag is set.
    bit = 0x6f if case['flag'] == 0x20 else 0x77
    handler = bytes.fromhex('f5dbbf3200c0cb') + bytes([bit]) + bytes.fromhex('28042101c034f1fbed4d')
    raw[0x38:0x38 + len(handler)] = handler
    code = bytearray(bytes.fromhex('f331f0dfed563e003200c03201c0'))

    def out(port, value):
        code.extend([0x3e, value, 0xd3, port])

    def register(index, value):
        out(0xbf, value)
        out(0xbf, 0x80 | index)

    def address(value):
        out(0xbf, value & 255)
        out(0xbf, 0x40 | (value >> 8))

    for index, value in [(0, 4), (1, 0), (2, 14), (5, 0x7f), (6, 0),
                         (7, 0), (8, 0), (9, 0), (10, 255)]:
        register(index, value)
    address(0)
    for _ in range(32):
        out(0xbe, 255)
    count = 9 if case['flag'] == 0x40 else 2
    address(0x3f00)
    for value in [case['y']] * count + [208]:
        out(0xbe, value)
    address(0x3f80)
    for sprite in range(count):
        out(0xbe, case['x'] + (sprite * 16 if count == 9 else 0))
        out(0xbe, 0)
    register(1, 0x60 if case.get('display', True) else 0x20)
    code.extend(bytes.fromhex('fb7618fd'))  # EI; HALT; JR HALT
    raw[0x100:0x100 + len(code)] = code
    raw[0x7ff0:0x7ff8] = b'TMR SEGA'
    raw[0x7fff] = 0x6c if case['native_gg'] else 0x4c
    return bytes(raw)


def run(core_path, case):
    raw = fixture(case)
    with tempfile.TemporaryDirectory(prefix='kega-mode4-') as temporary:
        rom = Path(temporary) / ('original.gg' if case['native_gg'] else 'original.sms')
        rom.write_bytes(raw)
        core = Core(core_path, raw, str(rom), {
            'genesis_plus_gx_system_hw': case['hardware'],
            'genesis_plus_gx_region_detect': case['region'],
            'genesis_plus_gx_overscan': case['overscan'],
        })
        original_env = core.keep[0]
        suppress = [case['skip']]
        env_type = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)

        @env_type
        def environment(command, pointer):
            if (command & 65535) == 47:
                C.cast(pointer, C.POINTER(C.c_int))[0] = 2 if suppress[0] else 3
                return True
            return original_env(command, pointer)

        audio = []
        batch_type = C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_size_t)

        @batch_type
        def batch(pointer, frames):
            audio.append(C.string_at(pointer, frames * 4))
            return frames

        core.keep.extend([environment, batch])
        core.lib.retro_set_environment(environment)
        core.lib.retro_set_audio_sample_batch(batch)
        ram = (C.c_ubyte * 8192).from_address(core.base + core.syms['work_ram'])
        samples = []
        for _ in range(8):
            core.frame()
            samples.append([ram[0], ram[1]])
        expected_increment = 1 if case['expected'] else 0
        steady = all(samples[i][1] - samples[i-1][1] == expected_increment
                     and bool(samples[i][0] & case['flag']) == case['expected']
                     for i in range(3, 8))
        state = core.save()

        def continuation():
            audio.clear()
            for i in range(6):
                suppress[0] = bool(i % 2)
                core.frame()
            return (core.save(), bytes(ram), b''.join(audio))

        first = continuation()
        accepted = core.restore(state)
        second = continuation() if accepted else None
        result = {'case': case, 'hardware': core.stats(), 'samples': samples,
                  'fixture_sha256': hashlib.sha256(raw).hexdigest(),
                  'steady_status_correct': steady, 'state_replay_exact': first == second,
                  'pass': steady and first == second}
        core.close()
        return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--core', required=True)
    parser.add_argument('--output')
    parser.add_argument('--case')
    args = parser.parse_args()
    core = str(Path(args.core).resolve())
    if args.case:
        print(json.dumps(run(core, json.loads(args.case))))
        return
    cases = []
    for hardware, region, native in [('master system II', 'ntsc-u', False),
                                     ('master system', 'pal', False),
                                     ('game gear', 'ntsc-u', False),
                                     ('game gear', 'ntsc-u', True)]:
        for name, y, x, flag, expected in [('visible', 64, 100, 32, True),
                                          ('above', 244, 100, 32, False),
                                          ('below', 200, 100, 32, False),
                                          ('overflow', 64, 20, 64, True),
                                          ('lcd_crop', 8, 20, 32, True)]:
            if name == 'lcd_crop' and not native:
                continue
            for overscan, skip in [('disabled', False), ('disabled', True), ('full', False)]:
                cases.append(dict(name=name, hardware=hardware, region=region, native_gg=native,
                                  y=y, x=x, flag=flag, expected=expected, overscan=overscan, skip=skip))
    for flag in (32, 64):
        for skip in (False, True):
            cases.append(dict(name='display_off', hardware='master system II', region='ntsc-u',
                              native_gg=False, y=184, x=20, flag=flag, expected=flag == 64,
                              overscan='disabled', skip=skip, display=False))
    results = []
    for case in cases:
        child = subprocess.run([sys.executable, __file__, '--core', core, '--case', json.dumps(case)],
                               text=True, capture_output=True)
        if child.returncode:
            raise RuntimeError(child.stderr)
        results.append(json.loads(child.stdout))
    passed = all(item['pass'] for item in results)
    result = {'core_sha256': hashlib.sha256(Path(core).read_bytes()).hexdigest(),
              'pass': passed, 'results': results}
    Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'pass': passed, 'cases': len(results),
                      'failed': [item['case'] for item in results if not item['pass']]}))
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
