#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
qt_root=${QT_WINDOWS_ROOT:-/home/vova/.cache/kegarefusion-qt/6.10.2/mingw_64}
build="$root/build/windows"

if [[ ! -f "$qt_root/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
  echo "Qt Windows MinGW installation not found: $qt_root" >&2
  echo 'Install Qt 6.10.2 win64_mingw with qtmultimedia, or set QT_WINDOWS_ROOT.' >&2
  exit 1
fi

python3 "$root/tools/prepare-sdl-windows.py"
sdl_root="$root/build/deps/SDL2-2.32.10/x86_64-w64-mingw32"
python3 "$root/tools/prepare-archive.py" --platform windows
"$root/tools/build-core.sh" windows
cmake -S "$root" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$root/cmake/mingw-w64-x86_64.cmake" \
  -DCMAKE_PREFIX_PATH="$qt_root" \
  -DSDL2_DIR="$sdl_root/lib/cmake/SDL2" \
  -DQT_HOST_PATH=/usr \
  -DQT_HOST_PATH_CMAKE_DIR=/usr/lib/x86_64-linux-gnu/cmake \
  -DQt6CoreTools_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt6CoreTools \
  -DQt6GuiTools_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt6GuiTools \
  -DQt6WidgetsTools_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt6WidgetsTools \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build "$build" -j "$(nproc)"
QT_WINDOWS_ROOT="$qt_root" "$root/tools/package-windows.sh" "$@"
