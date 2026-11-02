#!/usr/bin/env python3
"""Copy a Qt/MinGW Windows build and its transitive DLLs into a test folder."""

import argparse
import re
import shutil
import subprocess
from pathlib import Path


def imports(binary: Path) -> set[str]:
    result = subprocess.run(
        ["x86_64-w64-mingw32-objdump", "-p", str(binary)],
        check=True,
        capture_output=True,
        text=True,
    )
    return set(re.findall(r"^\s*DLL Name:\s*(\S+)", result.stdout, re.M))


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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--qt-root", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()

    root = args.root.resolve()
    qt_root = args.qt_root.resolve()
    destination = args.destination.resolve()
    build = root / "build" / "windows"
    executable = build / "bin" / "KegaRefusion.exe"
    core = build / "core" / "genesis_plus_gx_libretro.dll"
    for source in (executable, core):
        if not source.is_file():
            raise SystemExit(f"Missing Windows build product: {source}")

    # This staging directory belongs to the script; the deployed folder is never cleared.
    stage = build / "package"
    if stage.is_symlink() or stage.resolve().parent != build.resolve():
        raise SystemExit(f"Refusing to replace unsafe package staging path: {stage}")
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    sdl_root = root / "build/deps/SDL2-2.32.10/x86_64-w64-mingw32"
    sources = {
        Path("SDL2.dll"): sdl_root / "bin/SDL2.dll",
        Path("KegaRefusion.exe"): executable,
        Path("cores/genesis_plus_gx_libretro.dll"): core,
        Path("plugins/platforms/qwindows.dll"): qt_root / "plugins/platforms/qwindows.dll",
        Path("plugins/imageformats/qjpeg.dll"): qt_root / "plugins/imageformats/qjpeg.dll",
        Path("plugins/tls/qschannelbackend.dll"): qt_root / "plugins/tls/qschannelbackend.dll",
        Path("plugins/multimedia/ffmpegmediaplugin.dll"): qt_root / "plugins/multimedia/ffmpegmediaplugin.dll",
        Path("plugins/multimedia/windowsmediaplugin.dll"): qt_root / "plugins/multimedia/windowsmediaplugin.dll",
    }

    runtime = {path.name.lower(): path for path in (qt_root / "bin").glob("*.dll")}
    # Prefer the exact compiler runtime. GCC 13.2 imports nanosleep64, which is
    # absent from the older libwinpthread DLL included with Qt's GCC 13.1 kit.
    for name in ("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"):
        result = subprocess.run(
            ["x86_64-w64-mingw32-g++-posix", f"-print-file-name={name}"],
            check=True,
            capture_output=True,
            text=True,
        )
        candidate = Path(result.stdout.strip())
        if not candidate.is_file():
            raise SystemExit(f"Missing compiler runtime DLL: {name}")
        runtime[name] = candidate

    queue = list(sources.values())
    visited = set()
    while queue:
        binary = queue.pop()
        if binary in visited:
            continue
        visited.add(binary)
        if not binary.is_file():
            raise SystemExit(f"Missing runtime component: {binary}")
        for name in imports(binary):
            key = name.lower()
            if key in runtime and Path(name) not in sources:
                dependency = runtime[key]
                sources[Path(dependency.name)] = dependency
                queue.append(dependency)

    for relative, source in sources.items():
        output = stage / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, output)
    # Plugin search paths are configured by the executable before QApplication starts.

    notices = stage / "licenses"
    notices.mkdir()
    shutil.copy2(root / "THIRD_PARTY.md", notices / "THIRD_PARTY.md")
    copy_core32x_notices(root, notices)
    paprium_notices = notices / "paprium"
    paprium_notices.mkdir()
    for name in ("LICENSE", "PROVENANCE.md"):
        shutil.copy2(root / "src/paprium" / name, paprium_notices / name)
    if (root / "README.md").is_file():
        shutil.copy2(root / "README.md", stage / "README.md")
    shutil.copy2(
        root / "third_party/Genesis-Plus-GX-LICENSE.txt",
        notices / "Genesis-Plus-GX-LICENSE.txt",
    )
    shutil.copy2(root / "assets/fonts/OFL.txt", notices / "PressStart2P-OFL.txt")
    shutil.copy2(root / "third_party/SDL2-LICENSE.txt", notices / "SDL2-LICENSE.txt")
    shutil.copy2(root / "third_party/minimp3/LICENSE", notices / "minimp3-CC0.txt")
    shutil.copy2(root / "third_party/rcheevos/LICENSE", notices / "rcheevos-LICENSE.txt")
    for name in ("libarchive-LICENSE.txt", "liblzma-LICENSE.txt", "zlib-LICENSE.txt", "gdapt-NOTICE.txt"):
        shutil.copy2(root / "third_party" / name, notices / name)

    for source, name in (
        (Path("/usr/share/common-licenses/LGPL-3"), "Qt-LGPL-3.txt"),
        (Path("/usr/share/common-licenses/LGPL-2.1"), "FFmpeg-LGPL-2.1.txt"),
    ):
        if source.is_file():
            shutil.copy2(source, notices / name)

    destination.mkdir(parents=True, exist_ok=True)
    for source in stage.rglob("*"):
        if source.is_file():
            target = destination / source.relative_to(stage)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    generated_qt_conf = destination / "qt.conf"
    if generated_qt_conf.is_file() and generated_qt_conf.read_bytes() == b"[Paths]\nPlugins = plugins\n":
        generated_qt_conf.unlink()
    print(f"Packaged {len(sources)} binaries in {destination}")
    print("Existing saves, ROMs, and other files in the destination were retained.")


if __name__ == "__main__":
    main()
