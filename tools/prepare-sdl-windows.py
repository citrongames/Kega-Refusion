#!/usr/bin/env python3
"""Fetch the pinned official SDL2 MinGW SDK into the ignored dependency directory."""
from pathlib import Path
import hashlib
import tarfile
import urllib.request
from safe_tar import extract_verified_tar
root = Path(__file__).resolve().parents[1]
deps = root / "build" / "deps"
archive = deps / "SDL2-devel-2.32.10-mingw.tar.gz"
expected = "83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2"
sdk = deps / "SDL2-2.32.10" / "x86_64-w64-mingw32"
if not (sdk / "lib/cmake/SDL2/sdl2-config.cmake").is_file():
    deps.mkdir(parents=True, exist_ok=True)
    if not archive.is_file():
        url = "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.tar.gz"
        with urllib.request.urlopen(url, timeout=60) as response:
            content = response.read()
        if hashlib.sha256(content).hexdigest() != expected:
            raise SystemExit("SDL2 archive checksum mismatch")
        archive.write_bytes(content)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
        raise SystemExit("SDL2 archive checksum mismatch")
    with tarfile.open(archive) as package:
        extract_verified_tar(package, deps)
print(sdk)
