#!/usr/bin/env python3
"""Original generated Z80 IrataHack/SST39SF040 regression, no game ROM bytes.

The generated diagnostic executes from SMS work RAM while switching every bank.
Its CRC selects the existing IrataHack identity; the bytes are entirely original.
Visible ROM: python3 tests/sms_irata_mapper_tests.py --export PATH.sms
Regression:  python3 tests/sms_irata_mapper_tests.py --core CORE --output RESULT.json
Reference: https://github.com/drhelius/Gearsystem/blob/master/src/IratahackMemoryRule.cpp
Flash semantics follow SST39SF040: 4 KiB sectors, one-way byte programming.
"""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import zlib

from sms_codemasters_mapper_tests import Core as BaseCore, forge_crc

FLASH_SIZE = 0x80000
CRC = 0x49AF30F8
CODE_BASE = 0xC100
RESULT_BASE = 0xD000
FAILURES = 0xD3FE
READY = 0xD4FF
TARGET = 31 * 0x4000 + 0x2123
NEIGHBOR = TARGET + 0x1000

class Asm:
    def __init__(self):
        self.b = bytearray()
        self.results = []
        self.labels = {}
        self.links = []

    def emit(self, *values):
        self.b.extend(values)

    def word(self, value):
        self.b.extend(struct.pack('<H', value))

    def write(self, address, value):
        self.emit(0x3E, value, 0x32)
        self.word(address)

    def read(self, address, value, name):
        index = len(self.results)
        self.emit(0x3A)
        self.word(address)
        self.emit(0x32)
        self.word(RESULT_BASE + index)
        self.emit(0xFE, value)
        self.jump('check_' + str(index), 0xCA)
        self.emit(0x3A)
        self.word(FAILURES)
        self.emit(0x3C, 0x32)
        self.word(FAILURES)
        self.label('check_' + str(index))
        self.results.append((name, value))

    def label(self, name):
        self.labels[name] = CODE_BASE + len(self.b)

    def address(self, name):
        self.links.append((len(self.b), name))
        self.word(0)

    def jump(self, name, opcode=0xC3):
        self.emit(opcode)
        self.address(name)

    def load_hl(self, name):
        self.emit(0x21)
        self.address(name)

    def call(self, name):
        self.jump(name, 0xCD)

    def flash(self, command):
        self.write(0x5555, 0xAA)
        self.write(0x2AAA, 0x55)
        self.write(0x5555, command)

    def erase(self, address):
        self.flash(0x80)
        self.write(0x5555, 0xAA)
        self.write(0x2AAA, 0x55)
        self.write(address, 0x30)

    def program(self, address, value):
        self.flash(0xA0)
        self.write(address, value)

    def output(self, port, value):
        self.emit(0x3E, value, 0xD3, port)

    def vdp_address(self, address):
        self.output(0xBF, address & 255)
        self.output(0xBF, (address >> 8) & 255)

    def finish(self):
        for offset, name in self.links:
            self.b[offset:offset + 2] = struct.pack('<H', self.labels[name])
        assert len(self.b) < RESULT_BASE - CODE_BASE, 'fixture overlaps result RAM'
        return bytes(self.b)

# Original small bitmap letter shapes. Tile zero is a blank.
GLYPHS = {
    'A': [14,17,17,31,17,17,17], 'C': [14,17,16,16,16,17,14],
    'E': [31,16,16,30,16,16,31], 'F': [31,16,16,30,16,16,16],
    'H': [17,17,17,31,17,17,17], 'I': [14,4,4,4,4,4,14],
    'K': [17,18,20,24,20,18,17], 'L': [16,16,16,16,16,16,31],
    'M': [17,27,21,21,17,17,17], 'P': [30,17,17,30,16,16,16],
    'R': [30,17,17,30,20,18,17], 'S': [15,16,16,14,1,1,30],
    'T': [31,4,4,4,4,4,4], 'V': [17,17,17,17,17,10,4],
}

def add_display(a):
    # Clear VRAM, then write a compact text screen using original glyph tiles.
    for register, value in enumerate([0x04,0x00,0x0E,0x00,0x00,0x7E,0,0,0,0,0xFF]):
        a.output(0xBF, value)
        a.output(0xBF, 0x80 | register)
    a.vdp_address(0x4000)
    a.emit(0x01)
    a.word(0x4000)
    a.label('clear_vram')
    a.emit(0xAF,0xD3,0xBE,0x0B,0x78,0xB1)
    a.jump('clear_vram', 0xC2)
    a.vdp_address(0xC000)
    for value in [0x00, 0x3F] + [0] * 30:
        a.output(0xBE, value)
    tile_numbers = {' ': 0}
    patterns = bytes(32)
    for index, (letter, rows) in enumerate(GLYPHS.items(), 1):
        tile_numbers[letter] = index
        patterns += bytes(v for row in rows + [0] for v in (row << 2,0,0,0))
    a.vdp_address(0x4000)
    a.load_hl('font')
    a.emit(0x01)
    a.word(len(patterns))
    a.call('send_bytes')
    for text, row, column in [
        ('IRATAHACK MAPPER TEST', 6, 6),
        ('FLASH TEST', 9, 11),
    ]:
        a.vdp_address(0x4000 + 0x3800 + row * 64 + column * 2)
        for letter in text:
            a.output(0xBE, tile_numbers[letter])
            a.output(0xBE, 0)
    a.emit(0x3A)
    a.word(FAILURES)
    a.emit(0xB7)
    a.jump('display_fail', 0xC2)
    a.load_hl('pass_text')
    a.emit(0x16, 0x0C) # green
    a.jump('display_result')
    a.label('display_fail')
    a.load_hl('fail_text')
    a.emit(0x16, 0x03) # red
    a.label('display_result')
    a.vdp_address(0x4000 + 0x3800 + 13 * 64 + 14 * 2)
    a.emit(0x01)
    a.word(8)
    a.call('send_bytes')
    a.emit(0x3A)
    a.word(0xD4F0)
    a.emit(0xFE, 0xA5)
    a.jump('display_fresh', 0xC2)
    a.load_hl('save_pass_text')
    a.jump('display_save')
    a.label('display_fresh')
    a.load_hl('save_fresh_text')
    a.label('display_save')
    a.vdp_address(0x4000 + 0x3800 + 17 * 64 + 11 * 2)
    a.emit(0x01)
    a.word(20)
    a.call('send_bytes')
    # Backdrop is color zero; white glyphs use color one.
    a.vdp_address(0xC000)
    a.emit(0x7A, 0xD3, 0xBE)
    a.output(0xBF, 0x40)
    a.output(0xBF, 0x81)
    a.jump('after_display')
    a.label('send_bytes')
    a.emit(0x7E,0xD3,0xBE,0x23,0x0B,0x78,0xB1)
    a.jump('send_bytes', 0xC2)
    a.emit(0xC9)
    a.label('font')
    a.b.extend(patterns)
    for name, text in [('pass_text', 'PASS'), ('fail_text', 'FAIL'), ('save_pass_text', 'SAVE PASS '), ('save_fresh_text', 'SAVE FRESH')]:
        a.label(name)
        a.b.extend(bytes(v for letter in text for v in (tile_numbers[letter],0)))
    a.label('after_display')

def fixture(chip_erase=False):
    a = Asm()
    a.write(FAILURES, 0)
    for address, value in [(0x100,0x40),(0x4100,0x41),(0x8100,0x40)]:
        a.read(address, value, 'reset_bank_' + hex(address))
    # Observe an earlier session's programmed marker before the self-test erases it.
    a.write(0xFFFE, 3)
    a.write(0xFFFF, 7)
    a.emit(0x3A)
    a.word(0xA123)
    a.emit(0x32)
    a.word(0xD4F0)
    a.write(0xFFFE, 0)
    a.write(0xFFFF, 0)
    a.write(0xFFFF, 5)
    for game in range(4):
        a.write(0xFFFE, game)
        for address, bank in [(0x100,game*8),(0x4100,game*8+1),(0x8100,game*8+5)]:
            a.read(address, 0x40+bank, 'game_%d_slot_%04x' % (game,address))
        for bank in range(8):
            a.write(0xFFFF, bank)
            a.read(0x8100, 0x40 + game*8 + bank, 'game_%d_all_eight_banks_%d' % (game,bank))
        a.write(0xFFFF, 5)
    a.read(0, 0x58, 'game_slot_changes_vectors')
    a.write(0xFFFF, 0xFB)
    a.read(0x8100, 0x5B, 'slot2_register_uses_low_three_bits')
    a.write(0xFFFE, 0xFC)
    a.read(0x100, 0x40, 'game_register_uses_low_two_bits')
    a.read(0x8100, 0x43, 'game_change_preserves_slot2_bank')
    a.write(0xFFFC, 8)
    a.write(0xFFFD, 7)
    for address, value in [(0x100,0x40),(0x4100,0x41),(0x8100,0x43)]:
        a.read(address, value, 'other_SEGA_register_ignored_' + hex(address))
    a.write(0xDFFE, 2)
    a.read(0x100, 0x40, 'RAM_mirror_write_does_not_switch_game')
    a.read(0xFFFE, 2, 'mapper_register_retains_mirrored_work_RAM')
    a.write(0xFFFE, 3)
    a.write(0xFFFF, 7)
    a.flash(0x90)
    for address, value in [(0,0xBF),(1,0xB7),(2,0xFF),(0x4100,0x59),(0x8100,0x5F)]:
        a.read(address, value, 'flash_ID_' + hex(address))
    a.write(0x0000, 0xF0)
    a.read(0x100, 0x58, 'flash_ID_exit_restores_ROM')
    a.erase(0xA123)
    a.erase(0xB123)
    a.write(0x5555, 0xA0)
    a.write(0xA124, 0x42)
    a.read(0xA124, 0xFF, 'program_rejected_without_unlock')
    a.program(0xA123, 0x0F)
    a.read(0xA123, 0x0F, 'flash_program_byte')
    a.program(0xA123, 0xA0)
    a.read(0xA123, 0, 'flash_program_only_clears_bits')
    a.program(0xB123, 0x0A)
    a.erase(0xA123)
    a.read(0xA123, 0xFF, 'flash_erase_4K_sector')
    a.read(0xB123, 0x0A, 'flash_erase_preserves_neighbor_4K_sector')
    a.read(0x8100, 0x5F, 'flash_erase_preserves_other_sector_same_bank')
    a.program(0xA123, 0xA5)
    a.read(0xA123, 0xA5, 'flash_reprogram_after_erase')
    a.write(0xFFFF, 6)
    a.read(0xA123, 0x5E, 'flash_data_is_banked')
    a.write(0xFFFF, 7)
    a.read(0xA123, 0xA5, 'flash_data_survives_bank_switch')
    if chip_erase:
        a.flash(0x80)
        a.write(0x5555, 0xAA)
        a.write(0x2AAA, 0x55)
        a.write(0x5555, 0x10)
        for address in [0x100,0x4100,0x8100,0xA123,0xB123]:
            a.read(address, 0xFF, 'chip_erase_all_flash_' + hex(address))
    add_display(a)
    a.write(READY, 0xA7)
    a.label('wait')
    for command, name in [(1,'mutate'),(3,'inspect'),(4,'unlock_start'),(5,'unlock_finish'),(6,'unlock_cancel')]:
        a.emit(0x3A)
        a.word(0xD400)
        a.emit(0xFE, command)
        a.jump(name, 0xCA)
    a.jump('wait')
    a.label('mutate')
    a.program(0xA123, 0x24)
    a.write(0xFFFE, 1)
    a.write(0xFFFF, 4)
    a.write(0xD401, 1)
    a.write(0xD400, 0)
    a.jump('wait')
    a.label('inspect')
    for index, address in enumerate([0x100,0x4100,0x8100,0xA123,0xB123]):
        a.emit(0x3A)
        a.word(address)
        a.emit(0x32)
        a.word(0xD410 + index)
    a.write(0xD401, 3)
    a.write(0xD400, 0)
    a.jump('wait')
    a.label('unlock_start')
    a.write(0x5555, 0xAA)
    a.write(0xD401, 4)
    a.write(0xD400, 0)
    a.jump('wait')
    a.label('unlock_finish')
    a.write(0x2AAA, 0x55)
    a.write(0x5555, 0xA0)
    a.write(0xA124, 0x42)
    a.write(0xD401, 5)
    a.write(0xD400, 0)
    a.jump('wait')
    a.label('unlock_cancel')
    a.write(0x5555, 0xF0)
    a.write(0xD401, 6)
    a.write(0xD400, 0)
    a.jump('wait')
    code = a.finish()
    rom = bytearray(b''.join(bytes([0x40 + bank]) * 0x4000 for bank in range(32)))
    boot = bytes.fromhex('f331f0df2100021100c101') + struct.pack('<H',len(code)) + bytes.fromhex('edb0c300c1')
    rom[:len(boot)] = boot
    rom[0x200:0x200 + len(code)] = code
    rom[0x7FF0:0x7FF8] = b'TMR SEGA'
    rom[0x7FFF] = 0x40
    return forge_crc(rom, CRC), a.results

class Core(BaseCore):
    def __init__(self, path, rompath, data):
        super().__init__(path, rompath, data)
        self.video = None
        callback_type = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
        @callback_type
        def video(pointer, width, height, pitch):
            if pointer and pointer != C.c_void_p(-1).value:
                self.video = (width,height,pitch,C.string_at(pointer,pitch*height))
        self.keep.append(video)
        self.lib.retro_set_video_refresh(video)
        self.lib.retro_serialize.restype = C.c_bool
        self.lib.retro_unserialize.restype = C.c_bool

    def battery(self):
        size = self.lib.retro_get_memory_size(0)
        pointer = self.lib.retro_get_memory_data(0)
        return (C.c_ubyte * size).from_address(pointer) if pointer else bytearray()

    def save(self):
        size = self.lib.retro_serialize_size()
        state = C.create_string_buffer(size)
        assert self.lib.retro_serialize(state,size), 'native serialize failed'
        return state.raw

    def restore(self, state):
        buf = C.create_string_buffer(state)
        assert self.lib.retro_unserialize(buf,len(state)), 'native unserialize failed'

    def command(self, number):
        self.ram()[0x1401] = 0
        self.ram()[0x1400] = number
        self.frame()
        assert self.ram()[0x1401] == number, 'fixture command did not execute'

    def screenshot(self, path):
        assert self.video is not None
        width,height,pitch,pixels = self.video
        rows = bytearray()
        for y in range(height):
            rows.append(0)
            for x in range(width):
                pixel = struct.unpack_from('<H',pixels,y*pitch+2*x)[0]
                rows.extend(((pixel>>11)*255//31,((pixel>>5)&63)*255//63,(pixel&31)*255//31))
        def chunk(kind, data):
            return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
        Path(path).write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))

def run(corepath, directory, chip_erase=False, screenshot=None):
    data, expected = fixture(chip_erase)
    path = Path(directory) / ('chip_erase.sms' if chip_erase else 'IrataHack Original Mapper Test.sms')
    path.write_bytes(data)
    c = Core(corepath, path, data)
    c.frame(18)
    ram = c.ram()
    ready = ram[0x14FF] == 0xA7
    checks = [{'name':name,'expected':value,'observed':ram[0x1000+index],'pass':ram[0x1000+index]==value} for index,(name,value) in enumerate(expected)]
    checks.append({'name':'Z80_fixture_completed','pass':ready})
    checks.append({'name':'visual_self_check_failure_count','expected':0,'observed':ram[0x13FE],'pass':ram[0x13FE]==0})
    if screenshot:
        c.frame(100)
        c.screenshot(screenshot)
    battery = bytes(c.battery())
    checks.append({'name':'public_SAVE_RAM_is_full_512K_flash','expected':FLASH_SIZE,'observed':len(battery),'pass':len(battery)==FLASH_SIZE})
    if ready and not chip_erase and len(battery)==FLASH_SIZE:
        checks.append({'name':'public_SAVE_RAM_contains_mapped_flash','pass':battery[TARGET]==0xA5 and battery[NEIGHBOR]==0x0A})
        state = c.save()
        c.command(1)
        c.restore(state)
        c.command(3)
        observed = bytes(c.ram()[0x1410:0x1415])
        checks.append({'name':'native_state_restores_banks_flash_stays_separate','expected':'58595f240a','observed':observed.hex(),'pass':observed==bytes.fromhex('58595f240a')})
        C.memmove(c.lib.retro_get_memory_data(0),battery,len(battery))
        c.command(3)
        observed = bytes(c.ram()[0x1410:0x1415])
        checks.append({'name':'native_state_plus_SAVE_RAM_roundtrip','expected':'58595fa50a','observed':observed.hex(),'pass':observed==bytes.fromhex('58595fa50a')})
        c.command(4)
        pending = c.save()
        c.command(6)
        c.restore(pending)
        c.command(5)
        checks.append({'name':'native_state_restores_partial_flash_unlock','expected':0x42,'observed':c.battery()[TARGET+1],'pass':c.battery()[TARGET+1]==0x42})
        c.lib.retro_reset()
        checks.append({'name':'soft_reset_preserves_programmed_flash','pass':c.battery()[TARGET]==0xA5 and c.battery()[TARGET+1]==0x42})
        save_path = Path(directory) / 'original-flash.srm'
        save_path.write_bytes(battery)
        c.close()
        c = Core(corepath,path,data)
        loaded_battery = save_path.read_bytes()
        C.memmove(c.lib.retro_get_memory_data(0),loaded_battery,len(loaded_battery))
        checks.append({'name':'SAVE_RAM_reload_byte_for_byte','pass':bytes(c.battery())==battery})
        c.frame(18)
        if screenshot:
            c.frame(100)
            c.screenshot(str(Path(screenshot).with_name(Path(screenshot).stem+'-reloaded.png')))
        checks.append({'name':'cold_boot_reads_previously_saved_flash_marker','expected':0xA5,'observed':c.ram()[0x14F0],'pass':c.ram()[0x14F0]==0xA5})
        checks.append({'name':'saved_flash_still_boots_original_diagnostic','pass':c.ram()[0x14FF]==0xA7 and c.ram()[0x13FE]==0})
    if chip_erase and len(battery)==FLASH_SIZE:
        checks.append({'name':'chip_erase_all_512K_bytes','pass':battery==bytes([0xFF])*FLASH_SIZE})
    c.close()
    return {'mode':'chip_erase' if chip_erase else 'canonical','fixture_crc32':'%08X'%zlib.crc32(data),'fixture_sha256':hashlib.sha256(data).hexdigest(),'checks':checks,'pass':all(check['pass'] for check in checks)}

def compatibility(corepath, directory):
    original,_ = fixture()
    ordinary = bytearray(original)
    ordinary[-1] ^= 1
    md = bytearray(0x10000)
    struct.pack_into('>II',md,0,0xFFFF00,0x200)
    md[0x100:0x110] = b'SEGA GENESIS    '
    md[0x180:0x18E] = b'GM 00000000-00'
    md[0x1F0:0x1F3] = b'JUE'
    cases = []
    for label,data,suffix in [('irata',original,'.sms'),('ordinary_sms',bytes(ordinary),'.sms'),('ordinary_md_after_sms',bytes(md),'.md')]:
        path = Path(directory)/(label+suffix)
        path.write_bytes(data)
        core = Core(corepath,path,data)
        for name in ['retro_kegarefusion_recording_abi','retro_kegarefusion_network_state_abi']:
            getattr(core.lib,name).restype = C.c_char_p
        recording = core.lib.retro_kegarefusion_recording_abi().decode()
        network_bytes = core.lib.retro_kegarefusion_network_state_abi()
        network = None if network_bytes is None else network_bytes.decode()
        cases.append({'label':label,'recording_abi':recording,'network_abi':network})
        core.close()
    passed = (cases[0]['recording_abi']=='kegarefusion-recording-v3-peripherals-v1-sms-irata-v1'
        and cases[1]['recording_abi']==cases[2]['recording_abi']=='kegarefusion-recording-v3-peripherals-v1'
        and cases[0]['network_abi']=='kegarefusion-network-state-v3-peripherals-v1-gpx-f27655b-x64le-32x-wire-v1-sms-irata-v1-sms-pause-v1'
        and cases[1]['network_abi']=='kegarefusion-network-state-v3-peripherals-v1-gpx-f27655b-x64le-32x-wire-v1-sms-pause-v1'
        and cases[2]['network_abi']=='kegarefusion-network-state-v3-peripherals-v1-gpx-f27655b-x64le-32x-wire-v1')
    return {'pass':passed,'cases':cases}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core')
    parser.add_argument('--output')
    parser.add_argument('--export', help='Export the original visible diagnostic ROM')
    parser.add_argument('--screenshot', help='Save canonical real-core RGB565 frame as PNG')
    args = parser.parse_args()
    if args.export:
        data,_ = fixture()
        Path(args.export).write_bytes(data)
        print(json.dumps({'export':str(Path(args.export).resolve()),'bytes':len(data),'crc32':'%08X'%zlib.crc32(data),'sha256':hashlib.sha256(data).hexdigest()}))
    if not args.core:
        if args.export:
            return 0
        parser.error('--core or --export is required')
    if not args.output:
        parser.error('--output is required with --core')
    with tempfile.TemporaryDirectory(prefix='sms-irata-') as directory:
        results = [run(args.core,directory,False,args.screenshot),run(args.core,directory,True)]
        abi = compatibility(args.core,directory)
    result = {'core':str(Path(args.core).resolve()),'core_sha256':hashlib.sha256(Path(args.core).read_bytes()).hexdigest(),'fixture_scope':'Entirely original generated Z80 fixture; CRC collision selects IrataHack metadata. No copyrighted game bytes.','results':results,'compatibility':abi,'pass':all(item['pass'] for item in results) and abi['pass']}
    Path(args.output).write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'pass':result['pass'],'failed':[(item['mode'],check['name'],check.get('observed')) for item in results for check in item['checks'] if not check['pass']]}))
    return 0 if result['pass'] else 1

if __name__ == '__main__':
    raise SystemExit(main())
