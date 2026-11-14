"""Shared provenance notices for the integrated core in distributable packages."""

from pathlib import Path
import re
import shutil


def copy_core32x_notices(root: Path, notices: Path) -> None:
    # Retain component-local license/provenance filenames and references.
    for relative in (
        "third_party/picodrive32x/COPYING",
        "third_party/picodrive32x/ORIGIN.txt",
        "third_party/blargg/COPYING.LGPL-2.1",
        "third_party/blargg/PROVENANCE.txt",
        "src/core32x/video/LICENSE",
        "src/core32x/video/PROVENANCE.md",
    ):
        source = root / relative
        component = "core32x-video" if relative.startswith("src/") else source.parent.name
        target = notices / component / source.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)

    # GPX's bundled license does not include Buchmueller's separate SH2 terms,
    # the DRC authors' notices or the ares ISC notice retained by our SoC.
    parts = []
    for relative in (
        "src/core32x/bus.c",
        "src/core32x/machine.c",
        "src/core32x/audio/pwm.c",
        "src/core32x/video/compositor.c",
        "src/core32x/sh2/sh2_interpreter.c",
        "src/core32x/sh2/soc.c",
        "third_party/picodrive32x/cpu/sh2/mame/sh2.c",
        "third_party/picodrive32x/cpu/sh2/compiler.c",
        "third_party/picodrive32x/cpu/drc/emit_x86.c",
    ):
        text = (root / relative).read_text(encoding="utf-8")
        headers = []
        while match := re.match(r"\s*/\*.*?\*/", text, re.S):
            headers.append(match.group().strip())
            text = text[match.end():]
        if not headers:
            raise SystemExit(f"Missing original component notice: {relative}")
        parts.append(relative + "\n" + "\n".join(headers))
    (notices / "picodrive32x" / "COMPONENT-NOTICES.txt").write_text(
        "\n\n".join(parts) + "\n", encoding="utf-8"
    )



def copy_megald_notices(root: Path, notices: Path) -> None:
    component = notices / "megald"
    component.mkdir(parents=True, exist_ok=True)
    for relative, name in (
        ("src/megald/player/LICENSE", "LICENSE.ISC"),
        ("src/megald/media/LICENSE.qon-qoi2", "LICENSE.qon-qoi2"),
        ("src/megald/media/UPSTREAM.md", "UPSTREAM.md"),
    ):
        shutil.copy2(root / relative, component / name)
