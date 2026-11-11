#!/usr/bin/env python3
"""Build pinned official OpenSSL DLLs for the Qt Windows TLS PSK backend."""

import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path
from safe_tar import extract_verified_tar

VERSION = "3.5.8"
SHA256 = "a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2"
URL = f"https://github.com/openssl/openssl/releases/download/openssl-{VERSION}/openssl-{VERSION}.tar.gz"
CACHE = Path.home() / ".cache" / "kegarefusion-openssl" / VERSION
ARCHIVE = CACHE / f"openssl-{VERSION}.tar.gz"
SOURCE = CACHE / f"openssl-{VERSION}"
DLLS = ("libcrypto-3-x64.dll", "libssl-3-x64.dll")


def archive_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def verified_archive() -> None:
    CACHE.mkdir(parents=True, exist_ok=True)
    if ARCHIVE.exists() and archive_hash(ARCHIVE) == SHA256:
        return
    temporary = ARCHIVE.with_suffix(".part")
    digest = hashlib.sha256()
    try:
        with urllib.request.urlopen(URL, timeout=120) as response, temporary.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                digest.update(chunk)
                output.write(chunk)
        if digest.hexdigest() != SHA256:
            raise RuntimeError("OpenSSL source checksum mismatch")
        temporary.replace(ARCHIVE)
    finally:
        temporary.unlink(missing_ok=True)


def run_logged(command: list[str], cwd: Path, environment: dict[str, str]) -> None:
    log = CACHE / "build.log"
    with log.open("a", encoding="utf-8") as output:
        output.write("\n$ " + " ".join(command) + "\n")
        output.flush()
        result = subprocess.run(command, cwd=cwd, env=environment, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"OpenSSL build failed ({result.returncode}); see {log}")


def main() -> None:
    if all((SOURCE / name).is_file() for name in DLLS):
        print(SOURCE)
        return
    for tool in ("perl", "make", "x86_64-w64-mingw32-gcc-posix", "x86_64-w64-mingw32-ar"):
        if shutil.which(tool) is None:
            raise RuntimeError(f"Required OpenSSL build tool missing: {tool}")
    verified_archive()
    if not SOURCE.exists():
        with tarfile.open(ARCHIVE, "r:gz") as package:
            extract_verified_tar(package, CACHE)
    environment = os.environ.copy()
    environment["CC"] = "x86_64-w64-mingw32-gcc-posix"
    environment["AR"] = "x86_64-w64-mingw32-ar"
    environment["RANLIB"] = "x86_64-w64-mingw32-ranlib"
    environment["WINDRES"] = "x86_64-w64-mingw32-windres"
    if not (SOURCE / "Makefile").exists():
        run_logged(["perl", "Configure", "mingw64", "shared", "no-asm", "no-tests", "no-docs",
                    "--prefix=" + str(CACHE / "installed"),
                    "--openssldir=" + str(CACHE / "installed" / "ssl")], SOURCE, environment)
    run_logged(["make", "-j4", "build_libs"], SOURCE, environment)
    missing = [name for name in DLLS if not (SOURCE / name).is_file()]
    if missing:
        raise RuntimeError("OpenSSL build did not produce: " + ", ".join(missing))
    print(SOURCE)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)
