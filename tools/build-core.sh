#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
platform=${1:-}
case "$platform" in
  linux) extension=so ;;
  windows) extension=dll ;;
  *) echo "Usage: $0 linux|windows" >&2; exit 2 ;;
esac
if [[ "${KEGA32X_FOUNDATION_ONLY:-0}" == 1 ]]; then
  echo 'A foundation-only core cannot be published as the application core.' >&2
  exit 2
fi

# One GPX core owns MD/CD and its 32X attachment. Pin, patches and dependencies
# are managed in one place; retain the separate checkout and verification output.
"$root/tools/build-core32x.sh" "$platform"
source="$root/build/core32x-$platform/core/genesis_plus_gx_libretro.$extension"
[[ -s "$source" ]] || { echo "Built core is missing or empty: $source" >&2; exit 1; }
out="$root/build/$platform/core"
mkdir -p "$out"
target="$out/genesis_plus_gx_libretro.$extension"
staged=''
trap 'if [[ -n "$staged" ]]; then rm -f -- "$staged"; fi' EXIT
if ! cmp -s "$source" "$target"; then
  staged=$(mktemp "$out/.genesis_plus_gx_libretro.$extension.tmp.XXXXXX")
  cp -p -- "$source" "$staged"
  mv -f -- "$staged" "$target"
  staged=''
fi
echo "Core ready: $target"

# A distinct module identity isolates the second console's native globals.
# This is a build artifact, never a runtime copy of user content.
peer="$out/genesis_plus_gx_libretro_linkpeer.$extension"
if ! cmp -s "$target" "$peer"; then cp -p -- "$target" "$peer"; fi
