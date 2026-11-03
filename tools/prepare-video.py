#!/usr/bin/env python3
"""Build the pinned PNG/AAC/MOV-only FFmpeg recording libraries in build/deps.

Requires the matching prepare-archive.py output on Windows. Linux uses a PIC
build of the same pinned zlib. NASM is reused from PATH or bootstrapped locally
from a checksum-pinned Ubuntu package; nothing is installed system-wide.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / "build/deps"
VERSION = "n7.1.2"
URL = "https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/" + VERSION
SHA256 = "8cb1bb8cfa9aeae13279b4da42ae8307ae6777456d4270f2e603c95aa08ca8ef"
COMPONENTS = {"avformat": 61, "avcodec": 61, "avutil": 59}
NASM_VERSION = "3.01-1"
NASM_SHA256 = "a611d0d8f35d49e8c9f4b76ff6ef77fba2183ef1b46d0699431a848ba8d2cb46"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def output(args):
    return subprocess.check_output([str(arg) for arg in args], text=True).strip()


def nasm():
    executable = shutil.which("nasm")
    if executable:
        return Path(executable)
    archive = DEPS / "downloads" / f"nasm_{NASM_VERSION}_amd64.deb"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.is_file():
        # apt verifies this download against the signed Ubuntu package index.
        run(["apt-get", "download", f"nasm={NASM_VERSION}"], cwd=archive.parent)
    if digest(archive) != NASM_SHA256:
        raise SystemExit("NASM package checksum mismatch.")
    executable = DEPS / "nasm/usr/bin/nasm"
    if not executable.is_file():
        run(["dpkg-deb", "-x", archive, DEPS / "nasm"])
    return executable


def source():
    archive = DEPS / "downloads" / f"ffmpeg-{VERSION}.tar.gz"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.is_file():
        with urllib.request.urlopen(URL, timeout=60) as response:
            content = response.read(32 * 1024 * 1024 + 1)
        if hashlib.sha256(content).hexdigest() != SHA256:
            raise SystemExit("FFmpeg source checksum mismatch.")
        archive.write_bytes(content)
    if digest(archive) != SHA256:
        raise SystemExit("FFmpeg cached source checksum mismatch.")
    parent = DEPS / "video-minimal/source"
    directory = parent / f"FFmpeg-{VERSION}"
    if not directory.is_dir():
        parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as package:
            package.extractall(parent, filter="data")
    return directory


def archive_module():
    spec = importlib.util.spec_from_file_location("prepare_archive", ROOT / "tools/prepare-archive.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prepare_zlib(platform, work, jobs):
    stage = work / "zlib"
    if platform == "windows":
        original = DEPS / "archive/windows"
        if not (original / "lib/libzs.a").is_file():
            raise SystemExit("Run python3 tools/prepare-archive.py --platform windows first.")
        (stage / "lib").mkdir(parents=True, exist_ok=True)
        (stage / "include").symlink_to(os.path.relpath(original / "include", stage))
        (stage / "lib/libz.a").symlink_to(os.path.relpath(original / "lib/libzs.a", stage / "lib"))
        return
    archive = archive_module()
    directory = archive.source(*archive.SOURCES[0])
    run(["cmake", "-S", directory, "-B", work / "zlib-build", "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
         "-DCMAKE_INSTALL_PREFIX=" + str(stage), "-DCMAKE_INSTALL_LIBDIR=lib",
         "-DZLIB_BUILD_SHARED=OFF", "-DZLIB_BUILD_STATIC=ON", "-DZLIB_BUILD_TESTING=OFF"])
    run(["cmake", "--build", work / "zlib-build", "-j", jobs])
    run(["cmake", "--install", work / "zlib-build"])


def topology(work):
    config = (work / "config_components.h").read_text()
    enabled = {}
    # The MOV muxer requires the AC3 parser and these two packet helpers even
    # when only PNG/AAC are encoded. They do not enable a decoder or demuxer.
    for category, expected in (
            ("ENCODER", {"AAC", "PNG"}), ("DECODER", set()),
            ("MUXER", {"MOV"}), ("DEMUXER", set()), ("PARSER", {"AC3"}),
            ("BSF", {"AAC_ADTSTOASC", "VP9_SUPERFRAME"}), ("PROTOCOL", set()), ("INDEV", set()),
            ("OUTDEV", set()), ("FILTER", set()), ("HWACCEL", set())):
        found = set(re.findall(r"^#define CONFIG_(\w+)_" + category + r" 1$", config, re.MULTILINE))
        if found != expected:
            raise SystemExit(f"Unexpected FFmpeg {category.lower()} topology: {sorted(found)}")
        enabled[category.lower()] = sorted(found)
    general = (work / "config.h").read_text()
    for name in ("GPL", "NONFREE", "VERSION3", "NETWORK", "AVDEVICE", "AVFILTER",
                 "SWSCALE", "SWRESAMPLE", "POSTPROC", "STATIC"):
        if not re.search(rf"^#define CONFIG_{name} 0$", general, re.MULTILINE):
            raise SystemExit(f"Unexpected enabled FFmpeg feature: {name}")
    for name in ("SHARED", "PIC", "RUNTIME_CPUDETECT", "ZLIB", "X86ASM"):
        if not re.search(rf"^#define (?:CONFIG|HAVE)_{name} 1$", general, re.MULTILINE):
            raise SystemExit(f"Missing required FFmpeg feature: {name}")
    return enabled


def products(prefix, platform):
    result = [prefix / "include" / f"lib{name}/version.h" for name in COMPONENTS]
    for name, major in COMPONENTS.items():
        if platform == "windows":
            result += [prefix / "bin" / f"{name}-{major}.dll",
                       prefix / "lib" / f"lib{name}.dll.a"]
        else:
            result += [prefix / "lib" / f"lib{name}.so.{major}"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 12))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    prefix = DEPS / "video-minimal" / args.platform
    work = prefix / "build"
    assembler = nasm()
    compiler = "x86_64-w64-mingw32-gcc-posix" if args.platform == "windows" else "gcc"
    # Relative build paths keep FFmpeg's makefiles usable when ROOT has spaces.
    flags = [
        "--prefix=" + str(prefix), "--arch=x86_64", "--cpu=x86-64",
        "--cc=" + compiler, "--x86asmexe=./nasm",
        "--disable-everything", "--disable-autodetect", "--disable-programs",
        "--disable-doc", "--disable-debug", "--disable-static", "--enable-shared",
        "--enable-pic", "--enable-runtime-cpudetect", "--disable-network",
        "--disable-avdevice", "--disable-avfilter", "--disable-swscale",
        "--disable-swresample", "--disable-postproc", "--disable-iconv",
        "--disable-sdl2", "--disable-bzlib", "--disable-lzma",
        "--enable-zlib", "--enable-encoder=png,aac", "--enable-muxer=mov",
        "--extra-cflags=-I./zlib/include -march=x86-64 -mtune=generic",
    ]
    if args.platform == "windows":
        flags += ["--target-os=mingw32", "--enable-cross-compile",
                  "--cross-prefix=x86_64-w64-mingw32-", "--disable-pthreads",
                  "--enable-w32threads",
                  "--extra-ldflags=-L./zlib/lib -static-libgcc"]
    else:
        flags += ["--extra-ldflags=-L./zlib/lib"]
    archive = archive_module()
    zlib_id = (digest(DEPS / "archive/windows/lib/libzs.a") if args.platform == "windows"
               else archive.SOURCES[0][2])
    identity = {
        "schema": 1, "source": VERSION, "source_sha256": SHA256,
        "platform": args.platform, "configure": flags,
        "compiler": output([compiler, "--version"]).splitlines()[0],
        "assembler": output([assembler, "-v"]), "assembler_sha256": digest(assembler),
        "zlib_sha256": zlib_id,
    }
    stamp = prefix / "build-manifest.json"
    if stamp.is_file():
        manifest = json.loads(stamp.read_text())
        if manifest.get("identity") == identity and manifest.get("builder_sha256") == digest(Path(__file__)) and all(path.is_file() and manifest.get("products", {}).get(str(path.relative_to(prefix))) == digest(path)
                                                        for path in products(prefix, args.platform)):
            topology(work)
            print(prefix)
            return
    configured = work / "configuration.json"
    reuse = configured.is_file() and json.loads(configured.read_text()) == identity
    if not reuse:
        # Delete only this script's known output, inside its fixed platform root.
        if prefix.is_symlink() or prefix.resolve().parent != (DEPS / "video-minimal").resolve():
            raise SystemExit("Unsafe FFmpeg output path.")
        for name in ("build", "include", "lib", "bin", "share"):
            path = prefix / name
            if path.is_symlink():
                path.unlink()
            elif path.exists():
                shutil.rmtree(path)
        work.mkdir(parents=True)
        directory = source()
        (work / "src").symlink_to(os.path.relpath(directory, work))
        (work / "nasm").symlink_to(os.path.relpath(assembler, work))
        prepare_zlib(args.platform, work, args.jobs)
        run(["./src/configure", *flags], cwd=work)
        topology(work)
        configured.write_text(json.dumps(identity, indent=2) + "\n")
    if args.platform == "linux":
        # configure evaluates extra linker flags before writing config.mak.
        # Add this after configure. library.mak expands its rules via eval,
        # so the loader's dollar token must survive two make expansions.
        makefile = work / "ffbuild/config.mak"
        original = makefile.read_text()
        text = re.sub(r"^LDSOFLAGS \+= -Wl,-rpath,.*\n", "", original,
                      flags=re.MULTILINE).rstrip()
        text += "\n\nLDSOFLAGS += -Wl,-rpath,'$$$$ORIGIN'\n"
        if text != original:
            makefile.write_text(text)
            # A changed make variable alone does not invalidate link targets.
            for name in COMPONENTS:
                version_script = work / f"lib{name}/lib{name}.ver"
                if version_script.is_file():
                    version_script.touch()
    run(["make", "-j", args.jobs], cwd=work)
    run(["make", "install"], cwd=work)
    missing = [str(path) for path in products(prefix, args.platform) if not path.is_file()]
    if missing:
        raise SystemExit("Missing FFmpeg build products: " + ", ".join(missing))
    if args.platform == "linux":
        for name, major in COMPONENTS.items():
            dynamic = output(["readelf", "-d", prefix / "lib" / f"lib{name}.so.{major}"])
            if not re.search(r"(?:RUNPATH|RPATH).*\[\$ORIGIN\]", dynamic):
                raise SystemExit(f"Missing relative runtime search path in lib{name}.")
            if re.search(r"NEEDED.*(?:swscale|swresample|avfilter|avdevice|postproc)", dynamic):
                raise SystemExit(f"Unexpected FFmpeg runtime dependency in lib{name}.")
    capabilities = topology(work)
    notice = prefix / "share/kegarefusion-ffmpeg"
    notice.mkdir(parents=True, exist_ok=True)
    for name in ("COPYING.LGPLv2.1", "LICENSE.md"):
        shutil.copy2(work / "src" / name, notice / name)
    shutil.copy2(DEPS / archive.SOURCES[0][0] / "LICENSE", notice / "ZLIB-LICENSE.txt")
    (notice / "BUILD-CONFIG.txt").write_text(
        f"FFmpeg {VERSION}\nSource: {URL}\nSHA256: {SHA256}\n"
        f"Platform: {args.platform}\nCompiler: {identity['compiler']}\n"
        f"Assembler: {identity['assembler']}\nZlib SHA256: {zlib_id}\n"
        "Recording: PNG RGB24 + native AAC in MOV; application-owned custom AVIO.\n"
        "No decoders, demuxers, protocols, network, filters, devices or hardware acceleration.\n"
        f"Rebuild from the project checkout:\npython3 tools/prepare-archive.py --platform {args.platform}\n"
        f"python3 tools/prepare-video.py --platform {args.platform}\n"
        "Configure arguments (one per line):\n" + "\n".join(flags) + "\n" +
        ("Make override: LDSOFLAGS += -Wl,-rpath,'$$$$ORIGIN'\n" if args.platform == "linux" else ""))
    manifest = {"identity": identity, "builder_sha256": digest(Path(__file__)),
                "capabilities": capabilities,
                "products": {str(path.relative_to(prefix)): digest(path)
                             for path in products(prefix, args.platform)}}
    stamp.write_text(json.dumps(manifest, indent=2) + "\n")
    print(prefix)


if __name__ == "__main__":
    main()
