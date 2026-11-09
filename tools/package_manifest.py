"""Record ownership of files in a clean portable release package."""

import hashlib
import json
from pathlib import Path


def write_manifest(stage: Path) -> None:
    manifest = stage / "kega-package.json"
    entries = {}
    for path in sorted(stage.rglob("*")):
        if path.is_file() and path != manifest:
            with path.open("rb") as stream:
                digest = hashlib.sha256()
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            entries[path.relative_to(stage).as_posix()] = digest.hexdigest()
    manifest.write_text(json.dumps(entries, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
