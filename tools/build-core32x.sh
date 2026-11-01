#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
platform=${1:-}
pin=f27655b592ef43f317272d79883bcaa10d3ac8ad
use_drc=0
case "$platform" in
  linux)
    extension=so
    make_args=(platform=unix)
    if [[ $(uname -m) == x86_64 ]]; then use_drc=1; fi
    ;;
  windows)
    extension=dll
    use_drc=1 # This target always uses the x86_64 MinGW compiler.
    make_args=(platform=win
      CC=x86_64-w64-mingw32-gcc-posix
      CXX=x86_64-w64-mingw32-g++-posix
      LD=x86_64-w64-mingw32-gcc-posix)
    for compiler in x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-g++-posix; do
      command -v "$compiler" >/dev/null || {
        echo 'Install gcc-mingw-w64-x86-64-posix and g++-mingw-w64-x86-64-posix.' >&2
        exit 1
      }
    done
    ;;
  *) echo "Usage: $0 linux|windows" >&2; exit 2 ;;
esac

battery_patch="$root/tools/patches/genesis-plus-gx-battery.patch"
x32_patch="$root/tools/patches/genesis-plus-gx-32x.patch"
normal_patches=(
  "$root/tools/patches/genesis-plus-gx-32x-render-hooks.patch"
  "$root/tools/patches/genesis-plus-gx-32x-libretro.patch")
normal_receipts=(.kegarefusion-32x-render-hooks-applied.patch .kegarefusion-32x-libretro-applied.patch)
required_patches=("$battery_patch" "$x32_patch")
if [[ "${KEGA32X_FOUNDATION_ONLY:-0}" != 1 ]]; then
  required_patches+=("${normal_patches[@]}")
fi
for patch in "${required_patches[@]}"; do
  [[ -s "$patch" ]] || { echo "Required core patch is missing or empty: $patch" >&2; exit 1; }
done
for source in src/core32x third_party/picodrive32x third_party/blargg; do
  [[ -d "$root/$source" ]] || { echo "Required source directory is missing: $source" >&2; exit 1; }
done
for tool in git make python3 rsync nproc; do command -v "$tool" >/dev/null; done

# Never reuse or modify the released core's checkout or output directories.
checkout="$root/build/deps/genesis-plus-gx-32x-$platform"
if [[ ! -d "$checkout/.git" ]]; then
  [[ ! -e "$checkout" && ! -L "$checkout" ]] || {
    echo "Checkout path already exists without a Git repository: $checkout" >&2; exit 1;
  }
  mkdir -p "$(dirname "$checkout")"
  seed=""
  for candidate in "$root/build/deps/genesis-plus-gx" "$root/build/deps/genesis-plus-gx-linux" "$root/build/deps/genesis-plus-gx-windows"; do
    if [[ -d "$candidate/.git" ]] && git -C "$candidate" cat-file -e "$pin^{commit}" 2>/dev/null; then
      seed="$candidate"; break
    fi
  done
  if [[ -n "$seed" ]]; then
    git clone --quiet "$seed" "$checkout"
  else
    git clone --quiet https://github.com/libretro/Genesis-Plus-GX.git "$checkout"
  fi
  git -C "$checkout" checkout --quiet --detach "$pin"
fi
actual=$(git -C "$checkout" rev-parse HEAD)
if [[ "$actual" != "$pin" ]]; then
  echo "Experimental checkout is $actual; expected $pin. Leaving it untouched." >&2
  exit 1
fi
if [[ "${KEGA32X_FOUNDATION_ONLY:-0}" == 1 ]]; then
  for i in "${!normal_patches[@]}"; do
    receipt="$checkout/${normal_receipts[$i]}"
    if [[ -e "$receipt" || -L "$receipt" ]] ||
       { [[ -s "${normal_patches[$i]}" ]] && git -C "$checkout" apply --reverse --check "${normal_patches[$i]}" >/dev/null 2>&1; }; then
      echo 'Foundation-only build refuses a checkout with applied render/loader hooks; no patches removed.' >&2
      exit 1
    fi
  done
fi

# Keep the released core's battery-patch policy unchanged.
patch="$battery_patch"
if git -C "$checkout" apply --check "$patch" >/dev/null 2>&1; then
  git -C "$checkout" apply "$patch"
elif ! git -C "$checkout" apply --reverse --check "$patch" >/dev/null 2>&1; then
  echo "Core battery patch does not match $checkout. No reset was performed." >&2
  exit 1
fi

# BEGIN managed 32X patch update
apply_managed_patch() {
  local current_patch=$1 receipt=$2 temporary
  if [[ -e "$receipt" || -L "$receipt" ]]; then
    [[ -f "$receipt" && ! -L "$receipt" && -s "$receipt" ]] || {
      echo "Invalid applied-patch receipt: $receipt. Leaving checkout untouched." >&2; return 1;
    }
  fi
  if [[ -f "$receipt" ]] && ! cmp -s "$receipt" "$current_patch"; then
    # Only remove the exact previous revision, and refuse conflicting edits.
    if ! git -C "$checkout" apply --reverse --check "$receipt"; then
      echo 'Experimental checkout conflicts with its recorded patch; no changes made.' >&2
      return 1
    fi
    git -C "$checkout" apply --reverse "$receipt" || return 1
    if ! git -C "$checkout" apply --check "$current_patch" ||
       ! git -C "$checkout" apply "$current_patch"; then
      if ! git -C "$checkout" apply "$receipt"; then
        echo "Could not restore previous patch; retain receipt for manual recovery: $receipt" >&2
        return 1
      fi
      echo 'New 32X patch does not apply; previous revision restored. No Git reset performed.' >&2
      return 1
    fi
  elif git -C "$checkout" apply --check "$current_patch" >/dev/null 2>&1; then
    git -C "$checkout" apply "$current_patch" || return 1
  elif ! git -C "$checkout" apply --reverse --check "$current_patch" >/dev/null 2>&1; then
    echo "32X patch does not match $checkout; unknown changes preserved without reset." >&2
    return 1
  fi
  if ! cmp -s "$receipt" "$current_patch"; then
    temporary="$receipt.tmp.$$"
    cp "$current_patch" "$temporary" && mv -f "$temporary" "$receipt"
  fi
}
apply_managed_patch "$x32_patch" "$checkout/.kegarefusion-32x-applied.patch"
if [[ "${KEGA32X_FOUNDATION_ONLY:-0}" != 1 ]]; then
  for i in "${!normal_patches[@]}"; do
    apply_managed_patch "${normal_patches[$i]}" "$checkout/${normal_receipts[$i]}"
  done
fi
# END managed 32X patch update

sources_changed=0
copy_excludes=(--exclude=CMakeLists.txt --exclude='*.o' --exclude='*.d')
# Bootstrap check while the GPX bridge is being authored. Normal builds include
# every bridge source; this opt-in mode never copies a half-written .c file.
if [[ "${KEGA32X_FOUNDATION_ONLY:-0}" == 1 ]]; then
  copy_excludes+=(--exclude='gpx/*.c')
  echo 'Building 32X foundation only: GPX bridge C sources are excluded.'
fi
copy_tree() {
  local source=$1 destination=$2 resolved changes
  resolved=$(realpath -m "$destination")
  case "$resolved" in "$checkout"/*) ;; *) echo "Copy target escapes experimental checkout: $destination" >&2; exit 1 ;; esac
  mkdir -p "$destination"
  # Compare bytes, not source timestamps. Leave unchanged files and useful
  # object/dependency files untouched; prune only obsolete copied source files.
  changes=$(rsync -rc --delete "${copy_excludes[@]}" \
    --out-format='%i %n' "$source/" "$destination/")
  if [[ -n "$changes" ]]; then sources_changed=1; fi
}
copy_tree "$root/src/core32x" "$checkout/core/kega32x"
copy_tree "$root/third_party/picodrive32x" "$checkout/third_party/picodrive32x"
copy_tree "$root/third_party/blargg" "$checkout/third_party/blargg"

python3 - "$checkout" "$use_drc" <<'PY'
from pathlib import Path
import sys
checkout = Path(sys.argv[1])
use_drc = sys.argv[2] == '1'
common = checkout / 'libretro/Makefile.common'
data = common.read_bytes()
marker = b'# Kega Refusion experimental 32X sources'
anchor = b'SOURCES_C += $(foreach dir,$(GENPLUS_SRC_DIR),$(wildcard $(dir)/*.c))'
block = b"""# Kega Refusion experimental 32X sources
GENPLUS_SRC_DIR += $(CORE_DIR)/core/kega32x
GENPLUS_SRC_DIR += $(CORE_DIR)/core/kega32x/sh2
GENPLUS_SRC_DIR += $(CORE_DIR)/core/kega32x/audio
GENPLUS_SRC_DIR += $(CORE_DIR)/core/kega32x/video
GENPLUS_SRC_DIR += $(CORE_DIR)/core/kega32x/gpx
INCFLAGS += -I$(CORE_DIR)/third_party -I$(CORE_DIR)/third_party/picodrive32x
INCFLAGS += -I$(CORE_DIR)/core/kega32x/sh2
INCFLAGS += -I$(LIBRETRO_COMM_DIR)/include/streams
FLAGS += -DKEGAREFUSION_32X -MMD -MP
KEGA32X_DRC_ENABLED := @DRC@
ifeq ($(KEGA32X_DRC_ENABLED),1)
FLAGS += -DKEGA_SH2_DRC=1 -DDRC_SH2=1
KEGA32X_DRC_OBJECTS := $(addprefix $(CORE_DIR)/core/kega32x/sh2/,drc_backend.o drc_memory.o sh2_interpreter.o)
$(KEGA32X_DRC_OBJECTS): CFLAGS += -fno-lto -fno-strict-aliasing -fwrapv
endif

""".replace(b'@DRC@', b'1' if use_drc else b'0')
if data.count(anchor) != 1:
    raise SystemExit('Cannot locate unique GPX source-list expansion; Makefile left unchanged.')
if marker in data:
    start = data.index(marker)
    end = data.index(anchor)
    if start >= end:
        raise SystemExit('Experimental Makefile block is misplaced; Makefile left unchanged.')
    updated = data[:start] + block + data[end:]
else:
    updated = data.replace(anchor, block + anchor, 1)
if updated != data:
    common.write_bytes(updated)
# Both headers and compile settings participate in incremental rebuilds. The
# first DRC enable rebuilds old interpreter objects; unchanged runs touch none.
makefile = checkout / 'Makefile.libretro'
data = makefile.read_bytes()
marker = b'# Kega Refusion generated header dependencies'
end_marker = b'# END Kega Refusion generated header dependencies'
old_block = marker + b'\n-include $(OBJECTS:.o=.d)\n'
block = marker + b"""
$(OBJECTS): $(CORE_DIR)/libretro/Makefile.common $(CORE_DIR)/Makefile.libretro
-include $(OBJECTS:.o=.d)
# END Kega Refusion generated header dependencies
"""
if marker not in data:
    updated = data + b'\n' + block
elif data.count(marker) != 1:
    raise SystemExit('Duplicate managed dependency block; Makefile left unchanged.')
elif end_marker in data:
    start = data.index(marker)
    end = data.index(end_marker, start) + len(end_marker)
    if data[end:end+1] == b'\n':
        end += 1
    updated = data[:start] + block + data[end:]
elif old_block in data:
    updated = data.replace(old_block, block, 1)
else:
    raise SystemExit('Unknown managed dependency block; Makefile left unchanged.')
if updated != data:
    makefile.write_bytes(updated)

PY

# A removed source also requires relinking, even if every remaining .o is old.
if (( sources_changed )); then rm -f "$checkout/genesis_plus_gx_libretro.$extension"; fi
jobs=$(nproc)
if (( jobs > 8 )); then jobs=8; fi
make -s -C "$checkout" -f Makefile.libretro "${make_args[@]}" -j "$jobs" all
out="$root/build/core32x-$platform/core"
mkdir -p "$out"
if ! cmp -s "$checkout/genesis_plus_gx_libretro.$extension" "$out/genesis_plus_gx_libretro.$extension"; then
  staged="$out/.genesis_plus_gx_libretro.$extension.tmp.$$"
  cp "$checkout/genesis_plus_gx_libretro.$extension" "$staged"
  mv -f "$staged" "$out/genesis_plus_gx_libretro.$extension"
fi
echo "Experimental 32X core ready: $out/genesis_plus_gx_libretro.$extension"
