# Licensing

Copyright © 2026 Vladimir Salata (vova791).

## Original Kega Refusion code

Original Kega Refusion source code and original documentation are licensed under
the **GNU General Public License, version 3 or (at your option) any later version**,
with the additional permission in [LICENSE-EXCEPTION](LICENSE-EXCEPTION) for
combining with the identified emulation core. The complete GPL text is in
[LICENSE](LICENSE). This grant applies only to contributions for which the
project's copyright holder can grant these rights. This includes the original
built-in Dither blending GLSL shader and preset in `assets/shaders/refusion-dither.*`.

Kega Refusion is distributed without any warranty; see the GPL for details.
The core exception does not turn third-party code into GPL code and does not
remove the core's non-commercial restrictions.

## Third-party and derived material

Existing third-party copyright notices and licenses take precedence for their
respective material. In particular:

- **Genesis Plus GX and its patches:** retain the upstream non-commercial
  license and component notices. The pinned core source and our patches are
  prepared by `tools/build-core.sh` / `tools/build-core32x.sh`.
- **32X and Paprium:** `src/core32x`, `src/paprium`, and the corresponding
  vendored sources contain derived code with their own notices. Do not treat
  these directories as uniformly relicensed under GPL.
- **Vendored libraries and API headers:** retain their existing licenses; see
  `third_party/` and individual source headers.
- **Fonts, shaders and other third-party assets:** retain their component
  notices. The GPL grant for original code does not replace asset licenses.
- **Qt, FFmpeg, SDL and other packaged libraries:** retain their own licenses
  and source obligations. Binary packages include a `licenses/` directory.

[THIRD_PARTY.md](THIRD_PARTY.md) records upstream sources, pinned revisions,
license files and provenance. Read it together with the component notices.
The complete emulator distribution is a combination of components, not a
blanket GPL relicensing of everything in this repository.

## Source for releases

Release tags identify the exact frontend source. Release assets include a
source bundle containing the project, the modified core source used for the
build, and pinned dependency sources needed to rebuild the integrated core
and recording libraries. Build instructions are in [docs/building.md](docs/building.md).
Third-party runtime source locations are also recorded in the release source
bundle; their licenses remain unchanged.
