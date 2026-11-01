#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
platform=${1:-}
pin=f27655b592ef43f317272d79883bcaa10d3ac8ad

case "$platform" in
  linux)
    extension=so
    make_args=(platform=unix)
    ;;
  windows)
    extension=dll
    make_args=(platform=win
      CC=x86_64-w64-mingw32-gcc-posix
      CXX=x86_64-w64-mingw32-g++-posix
      LD=x86_64-w64-mingw32-gcc-posix)
    command -v x86_64-w64-mingw32-gcc-posix >/dev/null || {
      echo 'Install gcc-mingw-w64-x86-64-posix and g++-mingw-w64-x86-64-posix.' >&2
      exit 1
    }
    ;;
  *)
    echo "Usage: $0 linux|windows" >&2
    exit 2
    ;;
esac

checkout="$root/build/deps/genesis-plus-gx-$platform"
seed="$root/build/deps/genesis-plus-gx"
if [[ ! -d "$checkout/.git" ]]; then
  mkdir -p "$(dirname "$checkout")"
  if [[ -d "$seed/.git" ]]; then
    git clone --quiet "$seed" "$checkout"
  else
    git clone --quiet https://github.com/libretro/Genesis-Plus-GX.git "$checkout"
  fi
  git -C "$checkout" checkout --quiet --detach "$pin"
fi

actual=$(git -C "$checkout" rev-parse HEAD)
if [[ "$actual" != "$pin" ]]; then
  echo "Core checkout at $checkout is $actual; expected $pin. Leaving it untouched." >&2
  exit 1
fi

patch="$root/tools/patches/genesis-plus-gx-battery.patch"
if git -C "$checkout" apply --check "$patch" >/dev/null 2>&1; then
  git -C "$checkout" apply "$patch"
elif ! git -C "$checkout" apply --reverse --check "$patch" >/dev/null 2>&1; then
  echo "Core battery patch does not match $checkout. Leaving it untouched." >&2
  exit 1
fi

jobs=$(nproc)
if (( jobs > 8 )); then jobs=8; fi
make -s -C "$checkout" -f Makefile.libretro "${make_args[@]}" -j "$jobs"
out="$root/build/$platform/core"
mkdir -p "$out"
cp "$checkout/genesis_plus_gx_libretro.$extension" "$out/"
echo "Core ready: $out/genesis_plus_gx_libretro.$extension"
