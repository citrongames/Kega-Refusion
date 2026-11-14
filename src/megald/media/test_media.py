#!/usr/bin/env python3
"""Original tiny fixtures exercise MMI bounds, ZIP64, random seek and stream concurrency.
Run from the repository: python3 src/megald/media/test_media.py
Optional --library PATH uses an existing (e.g. sanitizer-enabled) shared library.
No copyrighted fixtures, extraction or game-image copying is involved.
"""
import argparse
import concurrent.futures
import ctypes as C
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest
import warnings
import zipfile
import zlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
U8 = C.c_ubyte
I16 = C.c_int16


class Track(C.Structure):
    _fields_ = [("number", C.c_int), ("is_data", C.c_int), ("index00", C.c_int64),
                ("index01", C.c_int64), ("end_lba", C.c_int64)]


class Side(C.Structure):
    _fields_ = [
        ("name", C.c_char * 256), ("format", C.c_char * 32),
        ("sequence_no", C.c_int), ("volume_no", C.c_int), ("side_no", C.c_int),
        ("width", C.c_uint32), ("height", C.c_uint32), ("frame_count", C.c_uint32),
        ("frame_duration_us", C.c_uint32), ("lead_in_frames", C.c_uint32),
        ("active_frames", C.c_uint32), ("lead_out_frames", C.c_uint32),
        ("has_video", C.c_int), ("has_analog_audio", C.c_int), ("has_digital_audio", C.c_int),
        ("analog_sample_frames", C.c_uint64), ("digital_sectors", C.c_uint64),
        ("track_count", C.c_int), ("tracks", Track * 99)]


class Info(C.Structure):
    _fields_ = [("name", C.c_char * 256), ("system", C.c_char * 32),
                ("region", C.c_char * 16), ("catalog_id", C.c_char * 64),
                ("side_count", C.c_int)]


def bind(path):
    lib = C.CDLL(str(path))
    lib.megald_media_open.argtypes = [C.c_char_p, C.c_char_p, C.c_size_t]
    lib.megald_media_open.restype = C.c_void_p
    lib.megald_media_close.argtypes = [C.c_void_p]
    lib.megald_media_get_info.argtypes = [C.c_void_p]
    lib.megald_media_get_info.restype = C.POINTER(Info)
    lib.megald_media_get_side.argtypes = [C.c_void_p, C.c_uint]
    lib.megald_media_get_side.restype = C.POINTER(Side)
    lib.megald_media_read_video.argtypes = [
        C.c_void_p, C.c_uint, C.c_uint32, C.c_void_p, C.c_size_t, C.c_size_t]
    lib.megald_media_read_sector.argtypes = [C.c_void_p, C.c_uint, C.c_uint64, C.c_void_p]
    lib.megald_media_read_pcm.argtypes = [
        C.c_void_p, C.c_uint, C.c_uint64, C.c_void_p, C.c_size_t]
    lib.megald_media_read_pcm.restype = C.c_size_t
    return lib


def literal(pixels):
    assert len(pixels) == 12
    return b"\xff" + pixels  # Four RGB pixels, no file header/trailer.


PIXELS = bytes(range(12))
DELTAS = bytes([1] * 12)
PCM = struct.pack("<8h", -32768, 32767, -1234, 1234, -1, 0, 7, -7)
DIGITAL = b"".join(bytes([i]) * 2352 for i in range(6))
CUE = (b'FILE "DigitalAudio.img" BINARY\r\n  TRACK 01 MODE1/2352\r\n'
       b'    INDEX 01 00:00:00\r\n  TRACK 02 AUDIO\r\n'
       b'    INDEX 00 00:00:02\r\n    INDEX 01 00:00:03\r\n')


def qon(payloads=None, flags=None):
    payloads = payloads or [literal(PIXELS), literal(DELTAS), literal(DELTAS)]
    flags = flags if flags is not None else [0, 0x8000, 0x8000]
    count = len(payloads)
    header = struct.pack("<4sIIBBHII", b"qon1", 2, 2, 3, 0,
                         1 if any(flags) else 0, count, 0)
    offsets, data = [], bytearray()
    for payload, flag in zip(payloads, flags):
        offsets.append(struct.pack("<Q", len(data) | flag << 48))
        data += struct.pack("<I", len(payload)) + payload
    return header + b"".join(offsets) + data


def fixture(two_sides=False, video=None, cue=CUE):
    media, entries = [], {}
    # Reverse order intentionally: API must sort by sequenceNo, with streams attached.
    for seq in ([1, 0] if two_sides else [0]):
        prefix = "Disc1Side" + str(seq + 1)
        streams = [
            {"name": "DigitalAudio", "type": "Redbook", "file": prefix + "/DigitalAudio.cue"},
            {"name": "AnalogAudio", "type": "RawAudio", "format": "s16le", "channels": 2,
             "file": prefix + "/AnalogAudio.pcm"},
            {"name": "AnalogVideo", "type": "RawVideo", "format": "NTSC-J",
             "file": prefix + "/AnalogVideo.qon",
             "framesInLeadInRegion": 0, "framesInActiveRegion": 3, "framesInLeadOutRegion": 0}]
        media.append({"name": "Side " + str(seq + 1), "type": "LD", "format": "NTSC-CAV",
                      "sequenceNo": seq, "volumeNo": 1, "sideNo": seq + 1, "streams": streams})
        entries[prefix + "/DigitalAudio.cue"] = cue
        entries[prefix + "/DigitalAudio.img"] = DIGITAL
        entries[prefix + "/AnalogAudio.pcm"] = PCM
        entries[prefix + "/AnalogVideo.qon"] = video if video is not None else qon()
    info = {"name": "Original test fixture", "system": "MegaLD", "regionCode": "J",
            "catalogId": "TEST", "media": media}
    entries["MediaInfo.json"] = json.dumps(info).encode()
    return entries


def write_zip(path, entries, compression=zipfile.ZIP_STORED):
    with zipfile.ZipFile(path, "w", compression=compression) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)


def write_sparse_zip64(path, entries):
    """A few KiB of actual data past 4 GiB, with genuine 64-bit ZIP records."""
    central = []
    with open(path, "wb") as out:
        out.seek((1 << 32) + 4096)
        for name, data in entries.items():
            offset, name = out.tell(), name.encode()
            crc = zlib.crc32(data)
            extra = struct.pack("<HHQQ", 1, 16, len(data), len(data))
            out.write(struct.pack("<IHHHHHIIIHH", 0x04034b50, 45, 0, 0, 0, 0,
                                  crc, 0xffffffff, 0xffffffff, len(name), len(extra)))
            out.write(name + extra + data)
            extra = struct.pack("<HHQQQ", 1, 24, len(data), len(data), offset)
            central.append(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 45, 45, 0, 0,
                                       0, 0, crc, 0xffffffff, 0xffffffff, len(name),
                                       len(extra), 0, 0, 0, 0, 0xffffffff) + name + extra)
        cd_offset = out.tell()
        for record in central:
            out.write(record)
        cd_size, z64_offset = out.tell() - cd_offset, out.tell()
        count = len(central)
        out.write(struct.pack("<IQHHIIQQQQ", 0x06064b50, 44, 45, 45, 0, 0,
                              count, count, cd_size, cd_offset))
        out.write(struct.pack("<IIQI", 0x07064b50, 0, z64_offset, 1))
        out.write(struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 0xffff, 0xffff,
                              0xffffffff, 0xffffffff, 0))


class MediaTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="fixture-", dir=BUILD)
        self.path = Path(self.scratch.name) / "MMI \u65e5\u672c.mmi"
        self.handles = []

    def tearDown(self):
        for handle in self.handles:
            LIB.megald_media_close(handle)
        self.scratch.cleanup()

    def open(self, entries=None, *, sparse=False):
        (write_sparse_zip64 if sparse else write_zip)(self.path, entries or fixture())
        error = C.create_string_buffer(256)
        handle = LIB.megald_media_open(os.fsencode(self.path), error, len(error))
        self.assertTrue(handle, error.value)
        self.handles.append(handle)
        return handle

    def fail_open(self, entries):
        write_zip(self.path, entries)
        error = C.create_string_buffer(256)
        handle = LIB.megald_media_open(os.fsencode(self.path), error, len(error))
        if handle:
            self.handles.append(handle)
        self.assertFalse(handle)
        self.assertTrue(error.value)

    def video(self, handle, frame, side=0):
        out = (U8 * 12)()
        self.assertEqual(LIB.megald_media_read_video(handle, side, frame, out, 12, 6), 1)
        return bytes(out)

    def test_metadata_side_order_toc(self):
        handle = self.open(fixture(two_sides=True))
        info = LIB.megald_media_get_info(handle).contents
        self.assertEqual((info.system, info.region, info.side_count), (b"MegaLD", b"J", 2))
        for i in range(2):
            side = LIB.megald_media_get_side(handle, i).contents
            self.assertEqual((side.sequence_no, side.side_no), (i, i + 1))
            self.assertEqual((side.digital_sectors, side.analog_sample_frames), (6, 4))
            self.assertEqual([(t.number, t.is_data, t.index00, t.index01, t.end_lba)
                              for t in side.tracks[:side.track_count]],
                             [(1, 1, -1, 0, 2), (2, 0, 2, 3, 6)])
        self.assertFalse(LIB.megald_media_get_side(handle, 2))

    def test_sparse_zip64_original_path(self):
        handle = self.open(sparse=True)
        self.assertGreater(self.path.stat().st_size, 1 << 32)
        self.assertLess(self.path.stat().st_blocks * 512, 1024 * 1024)
        self.assertEqual(self.video(handle, 2), bytes(x + 2 for x in PIXELS))
        sector = (U8 * 2352)()
        self.assertEqual(LIB.megald_media_read_sector(handle, 0, 5, sector), 1)
        self.assertEqual(bytes(sector), DIGITAL[-2352:])

    def test_random_interframe_seek_and_stride(self):
        handle = self.open()
        for frame in [2, 0, 1, 2, 2, 0]:
            self.assertEqual(self.video(handle, frame), bytes(x + frame for x in PIXELS))
        out = (U8 * 32)(*([0xcd] * 32))
        self.assertEqual(LIB.megald_media_read_video(handle, 0, 1, out, 32, 16), 1)
        self.assertEqual(bytes(out)[:6], bytes(x + 1 for x in PIXELS[:6]))
        self.assertEqual(bytes(out)[6:16], b"\xcd" * 10)
        self.assertEqual(bytes(out)[16:22], bytes(x + 1 for x in PIXELS[6:]))
        for side, frame, size, stride in [(1, 0, 32, 16), (0, 3, 32, 16),
                                           (0, 0, 21, 16), (0, 0, 32, 5)]:
            self.assertEqual(LIB.megald_media_read_video(handle, side, frame, out, size, stride), 0)

    def test_streams_concurrent_and_audio_eof(self):
        handle = self.open()
        def sectors():
            out = (U8 * 2352)()
            for i in range(200):
                lba = i % 6
                self.assertEqual(LIB.megald_media_read_sector(handle, 0, lba, out), 1)
                self.assertEqual(bytes(out), DIGITAL[lba * 2352:(lba + 1) * 2352])
            self.assertEqual(LIB.megald_media_read_sector(handle, 0, 6, out), 0)
        def audio():
            out = (I16 * 8)()
            for _ in range(200):
                self.assertEqual(LIB.megald_media_read_pcm(handle, 0, 0, out, 4), 4)
                self.assertEqual(list(out), list(struct.unpack("<8h", PCM)))
            self.assertEqual(LIB.megald_media_read_pcm(handle, 0, 3, out, 10), 1)
            self.assertEqual(list(out[:2]), [7, -7])
            self.assertEqual(LIB.megald_media_read_pcm(handle, 0, 4, out, 1), 0)
        def video():
            for i in range(100):
                self.assertEqual(self.video(handle, i % 3), bytes(x + i % 3 for x in PIXELS))
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            for task in [pool.submit(sectors), pool.submit(audio), pool.submit(video)]:
                task.result()

    def test_corrupt_qoi_and_frame_bounds(self):
        cases = [qon([b"\xff\x01\x02\x03"] * 3, [0, 0, 0]),
                 qon([b"\xe0"] * 3, [0, 0, 0]),
                 qon([b"\x6a\x01\x02"] * 3, [0, 0, 0]),
                 qon(flags=[0x8000, 0x8000, 0x8000])]
        v = bytearray(qon())
        v[24:32] = struct.pack("<Q", (1 << 48) - 1)
        cases.append(bytes(v))
        v = bytearray(qon())
        v[24:32] = struct.pack("<Q", 1 << 48)  # unknown flag
        cases.append(bytes(v))
        for stream in cases:
            with self.subTest(stream=stream[:32]):
                handle = self.open(fixture(video=stream))
                out = (U8 * 12)()
                self.assertEqual(LIB.megald_media_read_video(handle, 0, 0, out, 12, 6), 0)

    def test_corrupt_metadata_and_cue(self):
        entries = fixture()
        for bad in [b'{"media":[', b'{"name":"' + b"x" * 1024 + b'"}',
                    b"[" * 40 + b"0" + b"]" * 40]:
            malformed = dict(entries)
            malformed["MediaInfo.json"] = bad
            self.fail_open(malformed)
        self.fail_open(fixture(cue=CUE.replace(b"00:00:03", b"00:00:75")))
        self.fail_open(fixture(cue=CUE.replace(b"00:00:03", b"00:00:06")))
        self.fail_open(fixture(cue=CUE.replace(b"MODE1/2352", b"MODE1/2048")))
        malformed = dict(entries)
        del malformed["Disc1Side1/DigitalAudio.img"]
        self.fail_open(malformed)

    def test_zip_encryption_compression_duplicate_and_truncation(self):
        entries = fixture()
        write_zip(self.path, entries, zipfile.ZIP_DEFLATED)
        self.assertFalse(LIB.megald_media_open(os.fsencode(self.path), None, 0))
        write_zip(self.path, entries)
        data = bytearray(self.path.read_bytes())
        central = data.index(b"PK\x01\x02")
        data[central + 8] |= 1
        self.path.write_bytes(data)
        self.assertFalse(LIB.megald_media_open(os.fsencode(self.path), None, 0))
        write_zip(self.path, entries)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            with zipfile.ZipFile(self.path, "a") as archive:
                archive.writestr("MediaInfo.json", entries["MediaInfo.json"])
        self.assertFalse(LIB.megald_media_open(os.fsencode(self.path), None, 0))
        write_zip(self.path, entries)
        data = self.path.read_bytes()
        for n in [0, 3, 21, len(data) // 2, len(data) - 1]:
            self.path.write_bytes(data[:n])
            self.assertFalse(LIB.megald_media_open(os.fsencode(self.path), None, 0))

    def test_mutated_container_never_crashes(self):
        write_zip(self.path, fixture())
        original, rng = self.path.read_bytes(), random.Random(92831)
        for _ in range(500):
            data = bytearray(original)
            for _ in range(rng.randint(1, 4)):
                pos = rng.randrange(len(data))
                data[pos] ^= 1 << rng.randrange(8)
            self.path.write_bytes(data)
            handle = LIB.megald_media_open(os.fsencode(self.path), None, 0)
            if handle:
                out = (U8 * 12)()
                LIB.megald_media_read_video(handle, 0, rng.randrange(3), out, 12, 6)
                LIB.megald_media_close(handle)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path)
    args, rest = parser.parse_known_args()
    BUILD = ROOT / "build" / "tests" / "megald-media"
    BUILD.mkdir(parents=True, exist_ok=True)
    library = args.library
    if library is None:
        library = BUILD / "libmegald-media-test.so"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
                        "-fPIC", "-shared", "-g", "-O2", str(HERE / "media.c"), "-o", str(library)],
                       check=True)
    LIB = bind(library)
    unittest.main(argv=[__file__] + rest)
