"""Extract checksum-verified source tarballs without unsafe tar member types.

The caller must verify the archive hash before calling this function. This
implementation works on Python 3.11.2, which has no tarfile data filter.
"""
import os
from pathlib import Path, PurePosixPath
import shutil


def extract_verified_tar(package, destination, members=None):
    root = Path(destination).resolve()
    selected = list(package.getmembers() if members is None else members)
    entries = []
    for member in selected:
        name = member.name
        relative = PurePosixPath(name)
        if (not name or relative.is_absolute() or not relative.parts
                or "\\" in name or ".." in name.split("/")
                or ":" in relative.parts[0]):
            raise ValueError(f"Unsafe tar path: {name!r}")
        if not (member.isfile() or member.isdir()):
            raise ValueError(f"Unsupported tar member type: {name!r}")
        target = root.joinpath(*relative.parts)
        current = root
        for part in relative.parts:
            current = current / part
            if current.is_symlink():
                raise ValueError(f"Tar path crosses a symlink: {name!r}")
        entries.append((member, target))

    root.mkdir(parents=True, exist_ok=True)
    for member, target in entries:
        if member.isdir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        source = package.extractfile(member)
        if source is None:
            raise ValueError(f"Cannot read tar file: {member.name!r}")
        with source, target.open("wb") as output:
            shutil.copyfileobj(source, output)
        mode = member.mode & 0o755
        if not mode & 0o111:
            mode &= 0o644
        target.chmod(mode)
        os.utime(target, (member.mtime, member.mtime))
