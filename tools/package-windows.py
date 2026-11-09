#!/usr/bin/env python3
"""Copy a Qt/MinGW Windows build and its transitive DLLs into a test folder."""

import argparse
import filecmp
import re
import shutil
import subprocess
from pathlib import Path
import sys
sys.dont_write_bytecode = True
from package_manifest import write_manifest
from package_symbols import strip_binary
from package_notices import copy_core32x_notices


def imports(binary: Path) -> set[str]:
    result = subprocess.run(
        ["x86_64-w64-mingw32-objdump", "-p", str(binary)],
        check=True,
        capture_output=True,
        text=True,
    )
    return set(re.findall(r"^\s*DLL Name:\s*(\S+)", result.stdout, re.M))


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

    openssl_root = Path.home() / ".cache/kegarefusion-openssl/3.5.8/openssl-3.5.8"
    openssl_dlls = ("libcrypto-3-x64.dll", "libssl-3-x64.dll")
    if any(not (openssl_root / name).is_file() for name in openssl_dlls):
        subprocess.run(["python3", str(root / "tools/prepare-openssl-windows.py")], check=True)
    if not (openssl_root / "LICENSE.txt").is_file():
        raise SystemExit("Missing OpenSSL license: " + str(openssl_root / "LICENSE.txt"))

    sdl_root = root / "build/deps/SDL2-2.32.10/x86_64-w64-mingw32"
    sources = {
        Path("SDL2.dll"): sdl_root / "bin/SDL2.dll",
        Path("KegaRefusion.exe"): executable,
        Path("KegaRefusionUpdater.exe"): build / "bin/KegaRefusionUpdater.exe",
        Path("cores/genesis_plus_gx_libretro.dll"): core,
        Path("cores/genesis_plus_gx_libretro_linkpeer.dll"): build / "core/genesis_plus_gx_libretro_linkpeer.dll",
        Path("plugins/platforms/qwindows.dll"): qt_root / "plugins/platforms/qwindows.dll",
        Path("plugins/imageformats/qjpeg.dll"): qt_root / "plugins/imageformats/qjpeg.dll",
        Path("plugins/tls/qschannelbackend.dll"): qt_root / "plugins/tls/qschannelbackend.dll",
        Path("plugins/tls/qopensslbackend.dll"): qt_root / "plugins/tls/qopensslbackend.dll",
        Path("libcrypto-3-x64.dll"): openssl_root / "libcrypto-3-x64.dll",
        Path("libssl-3-x64.dll"): openssl_root / "libssl-3-x64.dll",
    }

    runtime = {path.name.lower(): path for path in (qt_root / "bin").glob("*.dll")}
    video_root = root / "build/deps/video-minimal/windows"
    # Application recording uses our small encoder/muxer build, not Qt's general media stack.
    for name in ("avcodec-61.dll", "avformat-61.dll", "avutil-59.dll"):
        library = video_root / "bin" / name
        if not library.is_file():
            raise SystemExit(f"Missing minimal FFmpeg runtime: {library}")
        runtime[name] = library
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

    removed_symbols = 0
    for relative, source in sources.items():
        output = stage / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, output)
        removed_symbols += strip_binary(output, "x86_64-w64-mingw32-strip")
    print(f"Removed {removed_symbols / 1048576:.2f} MiB of package-only debug/symbol data")
    # Plugin search paths are configured by the executable before QApplication starts.

    shutil.copytree(root / "languages", stage / "languages")
    shutil.copytree(root / "assets/shaders/examples", stage / "shaders")
    qtbase_ru = qt_root / "translations/qtbase_ru.qm"
    if not qtbase_ru.is_file():
        raise SystemExit(f"Missing Qt Russian translation: {qtbase_ru}")
    shutil.copy2(qtbase_ru, stage / "languages/qtbase_ru.qm")

    notices = stage / "licenses"
    notices.mkdir()
    shutil.copytree(video_root / "share/kegarefusion-ffmpeg", notices / "ffmpeg")
    for name in ("LICENSE", "LICENSE-EXCEPTION", "LICENSING.md"):
        shutil.copy2(root / name, notices / name)
    shutil.copy2(root / "THIRD_PARTY.md", notices / "THIRD_PARTY.md")
    copy_core32x_notices(root, notices)
    shutil.copy2(root / "third_party/libco/LICENSE.txt", notices / "libco-LICENSE.txt")
    shutil.copy2(root / "third_party/libco/README.txt", notices / "libco-PROVENANCE.txt")
    shutil.copy2(root / "tools/patches/genesis-plus-gx-link-cable.README.md", notices / "LinkCable-PROVENANCE.md")
    shutil.copy2(openssl_root / "LICENSE.txt", notices / "OpenSSL-LICENSE.txt")
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
    for name in ("GGPO-LICENSE.txt", "GGPO-PROVENANCE.md", "libarchive-LICENSE.txt", "liblzma-LICENSE.txt", "zlib-LICENSE.txt", "gdapt-NOTICE.txt"):
        shutil.copy2(root / "third_party" / name, notices / name)

    for source, name in (
        (Path("/usr/share/common-licenses/LGPL-3"), "Qt-LGPL-3.txt"),
    ):
        if source.is_file():
            shutil.copy2(source, notices / name)

    write_manifest(stage)
    destination.mkdir(parents=True, exist_ok=True)
    for source in stage.rglob("*"):
        if source.is_file():
            target = destination / source.relative_to(stage)
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.is_symlink():
                raise SystemExit(f"Refusing to replace destination symlink: {target}")
            if target.is_file() and filecmp.cmp(source, target, shallow=False):
                continue
            shutil.copy2(source, target)
    # Retire only known components previously shipped by this packager.
    # Native QAudioSink/device enumeration does not require a media playback plugin.
    for relative in ("plugins/multimedia/ffmpegmediaplugin.dll",
                     "plugins/multimedia/windowsmediaplugin.dll", "swscale-8.dll",
                     "swresample-5.dll", "licenses/FFmpeg-LGPL-2.1.txt"):
        target = destination / relative
        if target.is_symlink() or not target.resolve().is_relative_to(destination):
            raise SystemExit(f"Refusing to remove unsafe retired component: {target}")
        if target.is_file() and not (stage / relative).exists():
            target.unlink()
    media_directory = destination / "plugins/multimedia"
    if media_directory.is_dir() and not any(media_directory.iterdir()):
        media_directory.rmdir()
    generated_qt_conf = destination / "qt.conf"
    if generated_qt_conf.is_file() and generated_qt_conf.read_bytes() == b"[Paths]\nPlugins = plugins\n":
        generated_qt_conf.unlink()
    print(f"Packaged {len(sources)} binaries in {destination}")
    print("Existing saves, ROMs, and other files in the destination were retained.")


if __name__ == "__main__":
    main()
