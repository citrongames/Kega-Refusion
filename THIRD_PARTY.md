# Third-party components

- **Genesis Plus GX libretro core**: https://github.com/libretro/Genesis-Plus-GX, pinned commit `f27655b592ef43f317272d79883bcaa10d3ac8ad`. Upstream source compiled for each platform with the small frontend battery-memory extension in `tools/patches/genesis-plus-gx-battery.patch`; emulation and serialized state layout are unchanged. Its non-commercial license and embedded component notices are preserved in `third_party/Genesis-Plus-GX-LICENSE.txt`. This bundled core must not be treated as a permissively licensed commercial component.
- **libretro API header**: copied from that exact source revision, `libretro/libretro-common/include/libretro.h`; license/copyright notice is preserved in the header.
- **Qt 6**: dynamically linked Core, Gui, Widgets, Multimedia, OpenGL and OpenGLWidgets. Qt license notices and dependency licenses must accompany distributable Windows packages. See https://www.qt.io/licensing/open-source-lgpl-obligations and licenses supplied with the installed Qt distribution.

No game ROMs or console BIOS files are distributed. Integration test ROMs are original fixtures generated from the test source at runtime.

- **SDL 2.32.10**: joystick and controller discovery/input, dynamically linked; https://github.com/libsdl-org/SDL/tree/release-2.32.10. License preserved in `third_party/SDL2-LICENSE.txt`. Windows official MinGW archive is pinned by SHA-256 in `tools/prepare-sdl-windows.py`.
- **Press Start 2P**: pixel OSD typeface with Cyrillic by the Press Start 2P Project Authors; https://github.com/google/fonts/tree/main/ofl/pressstart2p. Font and SIL Open Font License are in `assets/fonts/`.

- **libarchive 3.8.10**: in-memory ZIP and 7z archive reading, statically linked; https://www.libarchive.org/. BSD notices preserved in third_party/libarchive-LICENSE.txt. The pinned build applies allocation/decompression caps to ZIP and 7z readers; see docs/archive-support.md and tools/prepare-archive.py.
- **XZ Utils / liblzma 5.8.4**: 7z LZMA/LZMA2 compression support, statically linked; https://tukaani.org/xz/. The linked liblzma is BSD Zero Clause licensed; notices preserved in third_party/liblzma-LICENSE.txt. XZ command-line tools are not bundled.
- **zlib 1.3.2**: ZIP Deflate compression support, statically linked; https://zlib.net/. License preserved in third_party/zlib-LICENSE.txt.

- **GDAPT v1.3**, Sp00kyFox (2014): unmodified two-pass GLSL from https://github.com/libretro/glsl-shaders at `cba01a48b0974a1232bea68d999a7c21bc4c233e`. Attribution, source hashes and the absence of an explicit upstream license grant are recorded in `third_party/gdapt-NOTICE.txt`, included with the Windows package. No RetroArch frontend implementation was copied.


## minimp3

- Source: https://github.com/lieff/minimp3
- Revision: `ea99364f61c14656440e8d77e9c233ccf3124633`
- License: CC0-1.0, retained in `third_party/minimp3/LICENSE`.
- Vendored unmodified `minimp3.h` and `minimp3_ex.h` provide on-demand MP3 audio decoding and indexed seeks for virtual CD tracks. No complete decoded audio or converted files are retained.
