#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
python3 "$root/tools/prepare-archive.py" --platform linux
python3 "$root/tools/prepare-video.py" --platform linux
"$root/tools/build-core.sh" linux
cmake -S "$root" -B "$root/build/linux" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DKEGA_TEST_CORE="$root/build/linux/core/genesis_plus_gx_libretro.so"
cmake --build "$root/build/linux" -j "$(nproc)"
mkdir -p "$root/build/linux/bin/cores"
cp "$root/build/linux/core/genesis_plus_gx_libretro.so" "$root/build/linux/bin/cores/"
ctest --test-dir "$root/build/linux" --output-on-failure
