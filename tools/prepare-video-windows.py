#!/usr/bin/env python3
"""Prepare FFmpeg 7.1.2 headers/import libraries for Qt's existing Windows DLLs.

This does not build or bundle another FFmpeg. All generated/downloaded files
stay in ignored build/deps storage; only public headers are extracted.
"""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = "n7.1.2"
URL = "https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/" + VERSION
SHA256 = "8cb1bb8cfa9aeae13279b4da42ae8307ae6777456d4270f2e603c95aa08ca8ef"
COMPONENTS = {"avformat": 61, "avcodec": 61, "avutil": 59}


def run(*args):
    return subprocess.run(args, check=True, text=True, stdout=subprocess.PIPE).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-root", type=Path,
                        default=Path.home() / ".cache/kegarefusion-qt/6.10.2/mingw_64")
    parser.add_argument("--prefix", type=Path, default=ROOT / "build/deps/video/windows")
    parser.add_argument("--tool-prefix", default="x86_64-w64-mingw32-")
    args = parser.parse_args()
    archive = ROOT / "build/deps/downloads" / ("ffmpeg-" + VERSION + ".tar.gz")
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.is_file():
        with urllib.request.urlopen(URL, timeout=45) as response:
            data = response.read(32 * 1024 * 1024 + 1)
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise SystemExit("FFmpeg source checksum mismatch.")
        archive.write_bytes(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise SystemExit("FFmpeg cached source checksum mismatch.")

    include = args.prefix / "include"
    library = args.prefix / "lib"
    include.mkdir(parents=True, exist_ok=True)
    library.mkdir(parents=True, exist_ok=True)
    # Copy regular header files explicitly, without extracting archive paths.
    with tarfile.open(archive) as package:
        for member in package.getmembers():
            parts = Path(member.name).parts
            if (member.isfile() and len(parts) >= 3 and
                    parts[1] in ("libavformat", "libavcodec", "libavutil") and
                    parts[-1].endswith(".h")):
                destination = include.joinpath(*parts[1:])
                destination.parent.mkdir(parents=True, exist_ok=True)
                with package.extractfile(member) as source, destination.open("wb") as output:
                    shutil.copyfileobj(source, output)
    # The only public generated platform header. Our supported Windows target
    # is x86-64 little-endian, with unaligned access.
    (include / "libavutil/avconfig.h").write_text(
        "#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n"
        "#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n#endif\n")
    for name, major in COMPONENTS.items():
        dll = args.qt_root / "bin" / f"{name}-{major}.dll"
        if not dll.is_file():
            raise SystemExit(f"Missing Qt FFmpeg runtime: {dll}")
        # Confirm the actual runtime build before using headers with public
        # structure layouts. Qt 6.10.2 ships the official FFmpeg n7.1.2 ABI.
        raw = dll.read_bytes()
        if name == "avcodec" and b"Lavc61.19.101" not in raw:
            raise SystemExit("Unexpected Qt FFmpeg build; expected FFmpeg 7.1.2.")
        dump = run(args.tool_prefix + "objdump", "-p", str(dll))
        table = dump.split("[Ordinal/Name Pointer] Table", 1)
        if len(table) != 2:
            raise SystemExit(f"Cannot inspect exports: {dll}")
        exports = re.findall(
            r"^\s*\[\s*\d+\]\s+(?:\+base\[\s*\d+\]\s+[0-9a-f]+\s+)?([A-Za-z_]\w*)\s*$",
            table[1], re.MULTILINE)
        exports = sorted(set(symbol for symbol in exports if symbol.startswith("av")))
        if len(exports) < 20:
            raise SystemExit(f"Unexpected export table: {dll}")
        definition = library / f"{name}.def"
        definition.write_text("LIBRARY " + dll.name + "\nEXPORTS\n" +
                              "\n".join(exports) + "\n")
        run(args.tool_prefix + "dlltool", "-m", "i386:x86-64", "-D", dll.name,
            "-d", str(definition), "-l", str(library / f"lib{name}.dll.a"))
    print(f"FFmpeg {VERSION} headers/import libraries ready: {args.prefix}")


if __name__ == "__main__":
    main()
