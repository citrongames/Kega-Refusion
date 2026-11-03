"""Remove non-runtime symbols from package copies, never from build products."""
import struct
import subprocess
from pathlib import Path


def strip_binary(path: Path, tool: str) -> int:
    if path.is_symlink():
        raise RuntimeError(f"Refusing to strip a symlink: {path}")
    with path.open("rb") as source:
        header = source.read(4096)
    if header.startswith(b"MZ"):
        if len(header) < 64:
            raise RuntimeError(f"Truncated PE file: {path}")
        pe = struct.unpack_from("<I", header, 0x3C)[0]
        if pe + 26 > len(header) or header[pe:pe+4] != b"PE\0\0":
            raise RuntimeError(f"Invalid PE header: {path}")
        magic = struct.unpack_from("<H", header, pe + 24)[0]
        directory = pe + 24 + (112 if magic == 0x20B else 96)
        if magic not in (0x10B, 0x20B) or directory + 40 > len(header):
            raise RuntimeError(f"Unsupported PE header: {path}")
        # Authenticode covers the binary layout. Preserve signed vendor files.
        if any(struct.unpack_from("<II", header, directory + 32)):
            return 0
    elif not header.startswith(b"\x7fELF"):
        return 0
    before = path.stat().st_size
    subprocess.run([tool, "--preserve-dates", "--strip-unneeded", str(path)], check=True)
    return before - path.stat().st_size
