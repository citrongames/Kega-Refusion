#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
qt_root=${QT_WINDOWS_ROOT:-/home/vova/.cache/kegarefusion-qt/6.10.2/mingw_64}
destination=${1:-/mnt/d/emulation/Sega Mega Drive/emu/KegaRefusion/testbuild}

python3 "$root/tools/package-windows.py" \
  --root "$root" --qt-root "$qt_root" --destination "$destination"
