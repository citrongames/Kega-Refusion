#!/usr/bin/env python3
"""Extract a pinned Flycast GGPO core and apply the audited Kega transport patch."""

import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / "build" / "deps"
COMMIT = "7adbf3caef251b153e348df3f469a0f35878339d"
URL = f"https://codeload.github.com/flyinghead/flycast/tar.gz/{COMMIT}"
ARCHIVE_SHA256 = "84e4d734f839bd68a6ec19353ecbfb2286f93ca7969fd32dc5b9dd7b48f3519e"
ARCHIVE = DEPS / "downloads" / f"flycast-{COMMIT}.tar.gz"
TARGET = DEPS / "ggpo-src"
STAGE = DEPS / "ggpo-stage"
PATCH = ROOT / "tools" / "patches" / "ggpo-kega.patch"


def checksum(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pinned_archive() -> None:
    ARCHIVE.parent.mkdir(parents=True, exist_ok=True)
    if not ARCHIVE.exists():
        temporary = ARCHIVE.with_suffix(".download")
        try:
            with urllib.request.urlopen(URL, timeout=90) as source, temporary.open("wb") as output:
                total = 0
                while chunk := source.read(1024 * 1024):
                    total += len(chunk)
                    if total > 64 * 1024 * 1024:
                        raise RuntimeError("Flycast source archive exceeds 64 MiB")
                    output.write(chunk)
            if checksum(temporary) != ARCHIVE_SHA256:
                raise RuntimeError("Flycast archive SHA-256 mismatch")
            temporary.replace(ARCHIVE)
        finally:
            temporary.unlink(missing_ok=True)
    if checksum(ARCHIVE) != ARCHIVE_SHA256:
        raise RuntimeError(f"Flycast archive SHA-256 mismatch: {ARCHIVE}")


def extract_ggpo() -> int:
    prefix = f"flycast-{COMMIT}/core/deps/ggpo/"
    count = 0
    with tarfile.open(ARCHIVE, "r:gz") as archive:
        for member in archive:
            if not member.name.startswith(prefix) or not member.isfile():
                continue
            relative = Path(member.name[len(prefix):])
            if not relative.parts or ".." in relative.parts or relative.is_absolute():
                raise RuntimeError(f"Unsafe GGPO archive member: {member.name}")
            if member.size > 2 * 1024 * 1024:
                raise RuntimeError(f"Oversized GGPO source file: {member.name}")
            output = STAGE / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            source = archive.extractfile(member)
            if source is None:
                raise RuntimeError(f"Missing GGPO archive member: {member.name}")
            with source, output.open("wb") as target:
                shutil.copyfileobj(source, target)
            count += 1
    if count < 25 or not (STAGE / "include" / "ggponet.h").is_file():
        raise RuntimeError("Pinned archive did not contain the expected GGPO source")
    return count


def main() -> None:
    pinned_archive()
    patch_bytes = PATCH.read_bytes()
    identity = hashlib.sha256(COMMIT.encode() + patch_bytes).hexdigest()
    stamp = TARGET / ".kega-source-id"
    if stamp.is_file() and stamp.read_text() == identity:
        print(TARGET)
        return
    if STAGE.exists():
        shutil.rmtree(STAGE)
    STAGE.mkdir(parents=True)
    try:
        count = extract_ggpo()
        subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-d", str(STAGE),
                        "-i", str(PATCH)], check=True)
        (STAGE / ".kega-source-id").write_text(identity)
        if TARGET.exists():
            shutil.rmtree(TARGET)
        STAGE.replace(TARGET)
    finally:
        if STAGE.exists():
            shutil.rmtree(STAGE)
    print(f"{TARGET} ({count} GGPO source files)")


if __name__ == "__main__":
    main()