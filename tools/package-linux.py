#!/usr/bin/env python3
"""Package the current Linux test build with its Qt/SDL/TLS runtime."""

import argparse
import gzip
import os
import re
import shutil
import subprocess
import tarfile
from pathlib import Path
import sys
sys.dont_write_bytecode = True
from package_manifest import write_manifest
from package_symbols import strip_binary
from package_notices import copy_core32x_notices


PLUGIN_FILES = (
    "platforms/libqxcb.so",
    "platforms/libqoffscreen.so",
    "xcbglintegrations/libqxcb-glx-integration.so",
    "xcbglintegrations/libqxcb-egl-integration.so",
    "wayland-graphics-integration-client/libqt-plugin-wayland-egl.so",
    "wayland-graphics-integration-client/libshm-emulation-server.so",
    "wayland-decoration-client/libbradient.so",
    "wayland-shell-integration/libxdg-shell.so",
    "tls/libqopensslbackend.so",
    "tls/libqcertonlybackend.so",
    "imageformats/libqjpeg.so",
    "imageformats/libqsvg.so",
    "imageformats/libqgif.so",
    "imageformats/libqico.so",
    "iconengines/libqsvgicon.so",
    "platforminputcontexts/libcomposeplatforminputcontextplugin.so",
)


def wayland_plugins(directory: Path) -> tuple[str, ...]:
    # Qt 6.4 ships separate EGL and generic platform plugins.
    unified = "platforms/libqwayland.so"
    if (directory / unified).is_file():
        return (unified,)
    return ("platforms/libqwayland-generic.so", "platforms/libqwayland-egl.so")


def required_glibc(binaries: list[Path]) -> tuple[int, ...]:
    highest = (0,)
    for binary in binaries:
        output = subprocess.check_output(["readelf", "--version-info", "--wide", binary],
                                         text=True, env={**os.environ, "LC_ALL": "C"})
        needs = output.partition("Version needs section")[2]
        for name in re.findall(r"Name: GLIBC_([\w.]+)", needs):
            if name == "ABI_DT_RELR":
                version = (2, 36)
            elif re.fullmatch(r"\d+(?:\.\d+)+", name):
                version = tuple(map(int, name.split(".")))
            else:
                raise RuntimeError(f"Unknown glibc ABI requirement in {binary.name}: GLIBC_{name}")
            highest = max(highest, version)
    if highest == (0,):
        raise RuntimeError("Could not determine the glibc requirement of the package")
    return highest


def included_library(name: str, path: Path) -> bool:
    if name.startswith(("libc.so.", "libm.so.", "libpthread.so.", "libdl.so.",
                        "librt.so.", "libresolv.so.", "libutil.so.", "libnss_")):
        return False
    # System Mesa drivers and the window system must share these ABIs.
    if name.startswith(("libstdc++.so.", "libgcc_s.so.", "libwayland-", "libOpenGL",
                        "libGL", "libEGL", "libGLES", "libvulkan", "libdrm",
                        "libgbm", "libLLVM", "libnvidia", "libamdgpu")):
        return False
    if "/dri/" in str(path) or name.startswith("ld-linux"):
        return False
    return True


def dependencies(binary: Path) -> dict[str, Path]:
    output = subprocess.run(["ldd", str(binary)], check=True, capture_output=True,
                            text=True).stdout
    result: dict[str, Path] = {}
    for line in output.splitlines():
        if "not found" in line:
            raise RuntimeError(f"Missing dependency of {binary}: {line.strip()}")
        match = re.match(r"\s*(\S+)\s+=>\s+(/.+?)\s+\(0x[0-9a-fA-F]+\)\s*$", line)
        if match:
            name, source = match.groups()
            path = Path(source)
            if included_library(name, path):
                result[name] = path
    return result


def copy(source: Path, target: Path) -> None:
    if not source.is_file():
        raise RuntimeError(f"Missing package component: {source}")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    strip_binary(target, "strip")


def archive_tree(stage: Path, target: Path) -> None:
    temporary = target.with_name(target.name + ".part")
    if target.is_symlink() or temporary.is_symlink():
        raise RuntimeError("Refusing to replace symlinked archive")
    if temporary.exists():
        temporary.unlink()
    with temporary.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0,
                           compresslevel=6) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as archive:
                for path in [stage, *sorted(stage.rglob("*"))]:
                    info = archive.gettarinfo(str(path), arcname=str(path.relative_to(stage.parent)))
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    info.mtime = 0
                    if info.isfile():
                        with path.open("rb") as file:
                            archive.addfile(info, file)
                    else:
                        archive.addfile(info)
    os.replace(temporary, target)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--max-glibc", metavar="VERSION",
                        help="Reject archives requiring a newer glibc than this baseline")
    args = parser.parse_args()
    if args.max_glibc and not re.fullmatch(r"\d+\.\d+(?:\.\d+)?", args.max_glibc):
        parser.error("--max-glibc expects a version such as 2.36")
    root = args.root.resolve()
    build = root / "build/linux"
    app = build / "bin/KegaRefusion"
    core = build / "core/genesis_plus_gx_libretro.so"
    plugin_dir = Path(subprocess.run(["qtpaths6", "--plugin-dir"], check=True,
                                     capture_output=True, text=True).stdout.strip())
    stage = build / "package-linux" / "KegaRefusion-linux-x86_64"
    parent = stage.parent
    if parent.is_symlink() or parent.resolve().parent != build.resolve():
        raise RuntimeError(f"Unsafe package staging path: {parent}")
    if parent.exists():
        shutil.rmtree(parent)
    stage.mkdir(parents=True)

    copy(app, stage / "KegaRefusion")
    copy(build / "bin/KegaRefusionUpdater", stage / "KegaRefusionUpdater")
    copy(core, stage / "cores/genesis_plus_gx_libretro.so")
    copy(build / "core/genesis_plus_gx_libretro_linkpeer.so", stage / "cores/genesis_plus_gx_libretro_linkpeer.so")
    shutil.copytree(root / "languages", stage / "languages")
    shutil.copytree(root / "assets/shaders/examples", stage / "shaders")
    qt_ru = Path("/usr/share/qt6/translations/qtbase_ru.qm")
    if qt_ru.is_file():
        copy(qt_ru, stage / "languages/qtbase_ru.qm")

    binaries = [app, build / "bin/KegaRefusionUpdater", core,
                build / "core/genesis_plus_gx_libretro_linkpeer.so"]
    for relative in (*PLUGIN_FILES, *wayland_plugins(plugin_dir)):
        source = plugin_dir / relative
        copy(source, stage / "plugins" / relative)
        binaries.append(source)
    libraries: dict[str, Path] = {}
    for binary in binaries:
        for name, source in dependencies(binary).items():
            if name in libraries and libraries[name].resolve() != source.resolve():
                raise RuntimeError(f"Conflicting runtime libraries: {name}")
            libraries[name] = source
    # Some Qt builds dlopen OpenSSL. Ship libssl from the same installation
    # as the libcrypto linked by netplay, including its runtime dependencies.
    crypto = libraries["libcrypto.so.3"]
    ssl = crypto.with_name("libssl.so.3")
    libraries.update(dependencies(ssl))
    libraries[ssl.name] = ssl
    for name, source in sorted(libraries.items()):
        copy(source.resolve(), stage / "lib" / name)

    launcher = stage / "run.sh"
    launcher.write_text("""#!/usr/bin/env bash
set -e
here="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec env LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \\
  QT_PLUGIN_PATH="$here/plugins" QT_QPA_PLATFORM_PLUGIN_PATH="$here/plugins/platforms" \\
  "$here/KegaRefusion" "$@"
""", encoding="utf-8")
    launcher.chmod(0o755)

    floor = required_glibc([*binaries, *libraries.values()])
    glibc = ".".join(map(str, floor))
    if args.max_glibc and floor > tuple(map(int, args.max_glibc.split("."))):
        raise SystemExit(f"Package requires glibc {glibc}, above requested {args.max_glibc}. "
                         "Rebuild the application, cores and runtime on the release baseline.")
    instruction = f"""Kega Refusion — запуск Linux x86_64

1. Распакуйте архив в доступный для записи каталог домашней папки.
2. Откройте терминал в каталоге KegaRefusion-linux-x86_64 и запустите ./run.sh.
3. Для игр и BIOS используйте свои файлы. Они не входят в архив.

Для запуска нужен
64-битный Linux с glibc не ниже {glibc}, графическим сеансом X11 или Wayland,
системным OpenGL/EGL-драйвером и работающим аудиоустройством. Более старые
дистрибутивы не проверены и могут не поддерживаться. Библиотеки Qt, SDL2,
OpenSSL и их зависимости находятся внутри архива; glibc и драйверы видеокарты
используются из системы. Настройки и сохранения по умолчанию создаются в
каталоге data рядом с приложением после первого запуска.

Для сетевой игры используйте одинаковую версию Kega Refusion на обоих ПК.
Windows и Linux x86_64 используют общий контракт сетевых состояний. Порт хоста:
33445 TCP и UDP (или выбранный им порт). В одной домашней сети копируйте
приглашение кнопкой «Копировать для локальной сети».
"""
    (stage / "ЗАПУСК-LINUX.txt").write_text(instruction, encoding="utf-8")
    notice = stage / "licenses"
    copy_core32x_notices(root, notice)
    for name in ("LICENSE", "PROVENANCE.md"):
        copy(root / "src/paprium" / name, notice / "paprium" / name)
    shutil.copytree(root / "build/deps/video-minimal/linux/share/kegarefusion-ffmpeg", notice / "ffmpeg")
    for name in ("LICENSE", "LICENSE-EXCEPTION", "LICENSING.md"):
        shutil.copy2(root / name, notice / name)
    for source, name in (
        (root / "THIRD_PARTY.md", "THIRD_PARTY.md"),
        (root / "assets/fonts/OFL.txt", "PressStart2P-OFL.txt"),
        (root / "third_party/minimp3/LICENSE", "minimp3-CC0.txt"),
        (root / "third_party/rcheevos/LICENSE", "rcheevos-LICENSE.txt"),
        (root / "third_party/libarchive-LICENSE.txt", "libarchive-LICENSE.txt"),
        (root / "third_party/liblzma-LICENSE.txt", "liblzma-LICENSE.txt"),
        (root / "third_party/zlib-LICENSE.txt", "zlib-LICENSE.txt"),
        (root / "third_party/libco/LICENSE.txt", "libco-LICENSE.txt"),
        (root / "third_party/libco/README.txt", "libco-PROVENANCE.txt"),
        (root / "tools/patches/genesis-plus-gx-link-cable.README.md", "LinkCable-PROVENANCE.md"),
        (root / "third_party/GGPO-LICENSE.txt", "GGPO-LICENSE.txt"),
        (root / "third_party/GGPO-PROVENANCE.md", "GGPO-PROVENANCE.md"),
        (root / "third_party/Genesis-Plus-GX-LICENSE.txt", "Genesis-Plus-GX-LICENSE.txt"),
        (root / "third_party/SDL2-LICENSE.txt", "SDL2-LICENSE.txt"),
        (root / "third_party/Wayland-Protocols-LICENSE.txt", "Wayland-Protocols-LICENSE.txt"),
        (Path("/usr/share/doc/libqt6core6/copyright"), "Qt-copyright.txt"),
        (Path("/usr/share/doc/libqt6core6t64/copyright"), "Qt-copyright.txt"),
        (Path("/usr/share/doc/libssl3/copyright"), "OpenSSL-copyright.txt"),
        (Path("/usr/share/doc/libssl3t64/copyright"), "OpenSSL-copyright.txt"),
        (Path("/usr/share/common-licenses/LGPL-3"), "LGPL-3.txt"),
        (Path("/usr/share/common-licenses/GPL-2"), "GPL-2.txt"),
    ):
        if source.is_file():
            copy(source, notice / name)

    destination = args.destination.absolute()
    destination.parent.mkdir(parents=True, exist_ok=True)
    write_manifest(stage)
    archive_tree(stage, destination)
    shutil.rmtree(parent)
    print(f"Created {destination} ({destination.stat().st_size:,} bytes), glibc {glibc}, {len(libraries)} bundled libraries")


if __name__ == "__main__":
    main()
