# Building and packaging

[← Project overview](../README.md)

The maintained build path is **Linux x86-64 / WSL2**, using native GCC for Linux and MinGW-w64 POSIX for Windows. The reference environment is Ubuntu 26.04, Qt 6.10.2 and CMake/Ninja. Native MSVC and macOS builds are not documented or validated.

Keep the checkout in the Linux filesystem when using WSL. Run the commands below from the repository root. Initial preparation downloads pinned dependency sources; subsequent builds reuse `build/` and external caches.

## 1. Prepare the Linux host

On the reference Ubuntu environment:

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build git python3 pkg-config \
  rsync perl nasm ca-certificates qt6-base-dev qt6-base-dev-tools \
  qt6-multimedia-dev libsdl2-dev libssl-dev \
  libwayland-dev wayland-protocols qt6-wayland
```

Qt Core, Gui, Widgets, Network, Multimedia, OpenGL and OpenGLWidgets are required. Wayland headers/protocols enable native mouse capture on Wayland. CMake declares Qt 6.5 as its lower bound, but the maintained binaries are built with **6.10.2**; older Qt versions are not a tested compatibility promise.

Use Python 3.12+ for the dependency tools. Installing NASM explicitly avoids reliance on the optional Ubuntu-package bootstrap in `prepare-video.py`. The full system FFmpeg development libraries are **not** required: the project builds its own minimal recording libraries.

## 2. Build Linux

Prepare the dependencies and integrated core, then build the application:

```sh
python3 tools/prepare-archive.py --platform linux
python3 tools/prepare-video.py --platform linux
./tools/build-core.sh linux
cmake -S . -B build/linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DKEGA_TEST_CORE="$PWD/build/linux/core/genesis_plus_gx_libretro.so"
cmake --build build/linux --target KegaRefusion -j "$(nproc)"
./build/linux/bin/KegaRefusion \
  --core "$PWD/build/linux/core/genesis_plus_gx_libretro.so"
```

The explicit `--core` path also locates the sibling Link Cable module. For a complete build **including all test targets and the registered test suite**, `./tools/build-linux.sh` is the convenience wrapper. Do not run it for every small edit when a targeted build is sufficient.

Package the runtime:

```sh
python3 tools/package-linux.py \
  --destination "$PWD/dist/KegaRefusion-linux-x86_64.tar.gz"
```

The archive includes Qt, SDL, TLS and other runtime libraries, plus a `run.sh` launcher. It uses the host's glibc and graphics drivers; building on a newer distribution does not produce a universally compatible Linux binary. The package records its glibc requirement.

## 3. Cross-build Windows from WSL/Linux

Install the POSIX MinGW toolchain in addition to the host dependencies:

```sh
sudo apt install gcc-mingw-w64-x86-64-posix \
  g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64
```

Provide a **Qt 6.10.2 Windows x64 MinGW kit with Qt Multimedia** and set `QT_WINDOWS_ROOT` to its `mingw_64` directory. The cross-build uses the Linux Qt host tools installed under `/usr`; keep their version aligned with the Windows kit. The reference compiler is MinGW GCC 13.2 POSIX.

```sh
export QT_WINDOWS_ROOT="$HOME/Qt/6.10.2/mingw_64"
./tools/build-windows.sh "$PWD/dist/windows"
```

Replace the example Qt path with your actual installation. Always supply an output directory: without one, the wrapper uses the maintainer's local Windows test folder.

The script prepares pinned SDL2, archive libraries, FFmpeg, OpenSSL and the integrated core; it builds Release targets and packages `KegaRefusion.exe` with its dependencies. Cross-built test executables are compiled but not automatically run on Windows.

Keep the packaged compiler runtimes. In particular, replacing `libwinpthread-1.dll` with an older Qt-kit copy can cause the `nanosleep64` entry-point error. No external `qt.conf` is needed; the application sets its plugin paths.

## Incremental work

For a frontend-only change, reuse the configured build directory:

```sh
cmake --build build/linux --target KegaRefusion -j "$(nproc)"
cmake --build build/windows --target KegaRefusion -j "$(nproc)"
```

Re-run `tools/build-core.sh linux` or `windows` when core sources or patches change. Re-run the matching dependency preparer when its pinned sources or configuration change. Keep the two core files separate: Link Cable relies on isolated native module state.

To package an existing Windows build without rebuilding it:

```sh
QT_WINDOWS_ROOT="$HOME/Qt/6.10.2/mingw_64" \
  ./tools/package-windows.sh "$PWD/dist/windows"
```

Close the Windows application before updating its directory. Packaging preserves user settings, saves and other content; it removes only explicitly retired packaged components. Do not replace the user's `data/` directory.

## Focused checks

Build and run the tests relevant to a change. For example, after an audio queue change:

```sh
cmake --build build/linux --target audio_queue_tests audio_output_tests -j "$(nproc)"
ctest --test-dir build/linux --output-on-failure \
  -R '^(audio_queue_tests|audio_output_tests)$'
```

`ctest --test-dir build/linux -N` lists registered checks. Some UI checks need a display; an offscreen Qt backend works for many, while GPU checks need a working OpenGL environment. Real-core checks generate original ROM fixtures. Specific compatibility probes may also require separately supplied game or BIOS files.

Video round-trip checks use system `ffmpeg` and `ffprobe` **as external validators**, not as application runtime dependencies. Install the Ubuntu `ffmpeg` package before configuring if those checks are needed. Build the corresponding test target before running it.

## Repository layout

| Path | Purpose |
|---|---|
| `src/` | Frontend, core integration, networking and attached hardware |
| `assets/`, `languages/` | UI artwork, shaders, fonts and external translations |
| `cmake/`, `tools/` | Build configuration, dependency preparation, patches and packaging |
| `tests/` | Focused unit, UI and real-core checks |
| `third_party/`, `THIRD_PARTY.md` | Vendored components, provenance and license notices |
| `docs/` | Architecture, feature guides and compatibility reports |
| `build/` | Ignored generated files, dependency sources and incremental builds |

Do not commit games, BIOS files, account credentials, saves or generated binaries. Keep the upstream license/provenance notices when distributing a package. See the [documentation index](README.md) for feature-specific details.
