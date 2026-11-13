#!/usr/bin/env python3
"""Generated original GG fixture: EXT pin/UART/state/audio real-core checks.

No commercial ROM, emulator debug API, or game-specific behavior is involved.
Local hidden ELF symbols are resolved from the exact built Linux core.
"""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct

from link_hardware_tests import Core, P, U, SZ, YIELD, require


def fixture():
    rom = bytearray([0xff]) * 32768
    rom[0:3] = bytes([0xc3, 0x00, 0x01])
    # NMI counts to C100, then RETN; ordinary fixture loops with IRQs disabled.
    rom[0x66:0x6f] = bytes([0xf5, 0x3a, 0x00, 0xc1, 0x3c, 0x32, 0x00, 0xc1, 0xf1])
    rom[0x6f:0x71] = bytes([0xed, 0x45])
    program = [0xf3, 0x31, 0xf0, 0xdf]
    for value in (0x84, 0x13, 0x90, 0xe7, 0xf2):
        program += [0x3e, value, 0xd3, 0x7f]
    program += [0xc3, 0x18, 0x01]
    rom[0x100:0x100+len(program)] = bytes(program)
    rom[0x7ff0:0x7ff8] = b'TMR SEGA'
    rom[0x7fff] = 0x6c
    return bytes(rom)


class GG(Core):
    def __init__(self, path, audio_filter='disabled'):
        super().__init__(path, fixture(), audio_filter, fixture_extension='.gg')
        self.io = self.library.retro_kegarefusion_link_io()
        self.read = self.function('io_gg_read', U, U)
        self.write = self.function('io_gg_write', None, U, U)
        self.sync = self.function('gg_link_sync', None, U)
        self.take_nmi = self.function('gg_link_nmi', U)
        self.mirror = self.function('io_z80_read', U, U)
        # Z80 cycles follows thirteen 32-bit pairs and eleven one-byte flags.
        self.cycles = U.from_address(self.address('Z80') + 64)
        self.hw = C.c_ubyte.from_address(self.address('system_hw'))
        require(self.hw.value == 0x40, 'native GG mode')
        self.library.retro_kegarefusion_gg_link_state_size.restype = SZ
        for name in ('save', 'load'):
            fn = getattr(self.library, 'retro_kegarefusion_gg_link_state_' + name)
            fn.argtypes, fn.restype = [P, SZ], C.c_bool

    def at(self, cycle):
        self.cycles.value = cycle
        self.sync(cycle)

    def wire_save(self):
        size = self.library.retro_kegarefusion_gg_link_state_size()
        require(size == 80, 'fixed GG wire extension size')
        out = C.create_string_buffer(size)
        require(self.library.retro_kegarefusion_gg_link_state_save(out, size), 'GG wire save')
        return out.raw

    def wire_load(self, raw):
        out = C.create_string_buffer(bytes(raw))
        return bool(self.library.retro_kegarefusion_gg_link_state_load(out, len(raw)))


def checks(path):
    core = GG(path)
    try:
        peer = (C.c_ubyte * 16)()
        peer[7] = 0x7f
        core.connect(peer)
        core.cycles.value = 0
        core.write(2, 0xff)
        for pin, crossed in enumerate((2, 3, 0, 1, 5, 4, 6)):
            peer[7] = 0x7f ^ (1 << pin)
            core.write(1, 0x80)
            require(core.read(1) == (0xff ^ (1 << crossed)), 'EXT cross pin %d' % pin)
            require((core.mirror(0) >> 6) == (core.read(1) & 3), 'DC EXT mirror')
            require((core.mirror(1) & 0x8f) == (((core.read(1) >> 2) & 15) | ((core.read(1) & 64) << 1)), 'DD EXT mirror')
        peer[7] = 0
        core.write(2, 0x80)
        core.write(1, 0xa5)
        require(core.read(1) == 0xa5, 'local output overrides cable')
        core.write(2, 0xc0)
        peer[7] = 0x7f
        core.at(100)
        core.write(2, 0x40)
        require(core.take_nmi() == 0, 'direction change invents no NMI')
        peer[7] &= ~64
        core.at(200)
        require(core.take_nmi() == 1 and core.take_nmi() == 0, 'PC6 falling NMI is queued once')
        core.at(300)
        require(core.take_nmi() == 0, 'no repeated NMI while PC6 low')
        peer[7] = 0x7f
        core.at(400)
        core.write(2, 0x80)
        core.write(1, 0)
        core.write(1, 64)
        core.write(1, 0)
        require(core.take_nmi() == 0, 'local PC6 writes never generate external NMI')

        rows = []
        for speed, baud in enumerate((4800, 2400, 1200, 300)):
            # TX line decodes as 8N1, status busy lasts ten bit periods.
            core.disconnect(); peer[7] = 0x7f; core.connect(peer)
            core.cycles.value = 0
            core.write(2, 0xff); core.write(5, speed << 6 | 0x38)
            core.write(3, 0xa6)
            period = (53693175 + baud // 2) // baud
            require(core.read(5) & 1, 'TXFL starts on TX write')
            core.write(3, 0x11)
            require(core.read(3) == 0xa6, 'busy TX ignores replacement')
            actual = []
            for bit in range(10):
                core.at(bit * period + period // 2)
                actual.append((core.io[7] >> 4) & 1)
                require(core.read(5) & 1, 'TXFL remains until stop bit ends')
            require(actual == [0] + [(0xa6 >> n) & 1 for n in range(8)] + [1], '8N1 TX bits')
            core.at(10 * period)
            require(not core.read(5) & 1, 'TXFL clears after stop bit')

            # RX at same rate while TX remains enabled, with one bad stop bit.
            for value, stop in ((0x53, 1), (0x8c, 0)):
                core.disconnect(); peer[7] = 0x7f; core.connect(peer)
                core.cycles.value = 0
                core.write(2, 0xff); core.write(5, speed << 6 | 0x38)
                core.write(3, value ^ 0xff)
                # Establish an idle high before the start edge.
                core.at(100)
                base = 1000
                peer[7] &= ~16
                core.at(base)
                core.at(base + period // 2)
                for bit in range(8):
                    peer[7] = (peer[7] & ~16) | (((value >> bit) & 1) << 4)
                    core.at(base + (bit + 1) * period + period // 2)
                peer[7] = (peer[7] & ~16) | (stop << 4)
                core.at(base + 9 * period + period // 2)
                status = core.read(5)
                require(status & 2 and bool(status & 4) == (not stop), 'RX ready/framing status')
                require(core.take_nmi() == 1, 'serial NMI independent of port02 mask')
                require(core.read(4) == value and core.read(5) & 6 == 0, 'receive read clears ready/error')
                require(core.read(5) & 1, 'RX completion does not clear simultaneous TX')
                core.at(10 * period)
                require(not core.read(5) & 1, 'full-duplex TX completes independently')
            rows.append({'baud': baud, 'period_master_cycles': period, 'tx_8n1': True, 'rx_nmi_and_framing': True, 'full_duplex': True})

        # Capture and restore both TX and RX mid-byte without creating an edge.
        core.disconnect(); peer[7] = 0x7f; core.connect(peer)
        core.cycles.value = 0
        core.write(2, 0xff); core.write(5, 0x38); core.write(3, 0xb5)
        peer[7] &= ~16
        core.at(100)
        core.at(6000)
        wire = core.wire_save()
        native = core.native_save()
        expected = (bytes(core.io[:8]), wire)
        for index in (0, 40, 48, 65, 72):
            corrupt = bytearray(wire); corrupt[index] ^= 0xff
            require(not core.wire_load(corrupt), 'malformed wire state rejected')
            require(core.wire_save() == wire, 'malformed restore changes nothing')
        def continue_uart():
            trace = []
            for cycle in (16000, 30000, 60000, 90000, 120000):
                core.at(cycle)
                trace.append((core.read(5), int(core.io[7]), core.take_nmi()))
            trace.append(core.read(4))
            return trace
        first = continue_uart()
        core.disconnect()
        require(core.native_load(native), 'GG native in-flight restore')
        core.connect(peer)
        require(core.wire_load(wire), 'GG in-flight extension after reconnect')
        require((bytes(core.io[:8]), core.wire_save()) == expected, 'in-flight exact restore')
        require(continue_uart() == first, 'in-flight UART timing replay')

        # RX disable aborts reception and does not synthesize a completion.
        core.disconnect(); peer[7] = 0x7f; core.connect(peer)
        core.cycles.value = 0; core.write(5, 0x30); core.at(100)
        peer[7] &= ~16; core.at(1000); core.at(7000)
        core.write(5, 0x10)
        peer[7] = 0x7f
        core.at(200000)
        require(core.read(5) & 6 == 0, 'RX disable aborts incomplete byte')
        core.disconnect()

        # At 300 baud a complete byte spans two NTSC video frames. Exercise
        # actual system_frame_sms rebasing plus the host restore ordering.
        core.close(); core = GG(path)
        for _ in range(3): core.frame()
        peer[7] = 0x7f; core.connect(peer)
        core.write(5, 0xf8); core.write(3, 0x39)
        peer[7] &= ~16
        core.frame()
        saved_native, saved_wire, saved_audio = core.native_save(), core.wire_save(), core.audio_save()
        require(struct.unpack_from('<I', saved_wire, 40)[0] == 1, '300-baud TX remains in flight at frame boundary')
        require(struct.unpack_from('<I', saved_wire, 44)[0] == 1, '300-baud RX remains in flight at frame boundary')
        def finish_frames():
            result = []
            for _ in range(3):
                core.frame()
                result.append((core.wire_save(), core.last_video,
                               hashlib.sha256(core.last_audio).digest()))
            require(core.read(5) & 7 == 6, '300-baud RX completes with framing error and TX becomes empty')
            return result
        first = finish_frames()
        core.disconnect()
        require(core.native_load(saved_native), 'multi-frame native restore')
        core.connect(peer)
        require(core.wire_load(saved_wire), 'multi-frame wire restore')
        require(core.audio_load(saved_audio), 'multi-frame audio restore')
        require(finish_frames() == first, '300-baud multi-frame UART/audio/video replay')
        core.disconnect()
        # Native/audio restoration includes GG stereo PSG state and filters.
        audio_rows = []
        for filter_index, audio_filter in enumerate(('disabled', 'low-pass', 'EQ')):
            core.close()
            core = GG(path, audio_filter)
            for _ in range(4): core.frame()
            core.write(6, 0x1f)
            core.frame()
            stereo = struct.iter_unpack('<hh', core.last_audio)
            require(any(left != right for left, right in stereo), 'GG asymmetric PSG stereo')
            saved = core.snapshot()
            require(struct.unpack_from('<I', saved[1], 12)[0] == filter_index, 'selected audio filter')
            first = core.trace(24)
            core.restore(saved)
            second = core.trace(24)
            require(first == second, 'GG PSG audio/video replay ' + audio_filter)
            audio_rows.append({'filter': audio_filter, 'frames': 24, 'asymmetric_stereo': True, 'audio_video_equal': True})
        return {'core': str(Path(path).resolve()),
                'core_sha256': hashlib.sha256(Path(path).read_bytes()).hexdigest(),
                'fixture_sha256': hashlib.sha256(fixture()).hexdigest(),
                'scope': 'Original generated GG ROM; local ELF inspection; no external game ROMs',
                'gpio_cross_and_direction': True, 'controller_mirrors': True,
                'pc6_queued_nmi': True, 'uart': rows, 'inflight_state_exact': True,
                'malformed_state_atomic': True, 'rx_disable_abort': True,
                'multi_frame_300_baud_replay': True,
                'audio_video_replay': audio_rows, 'pass': True}
    finally:
        core.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--core', required=True)
    parser.add_argument('--output')
    args = parser.parse_args()
    result = checks(args.core)
    output = json.dumps(result, indent=2)
    if args.output: Path(args.output).write_text(output + '\n')
    print(output)
