# Third-party components

Original Kega Refusion code is GPL-3.0-or-later with the additional core linking permission in `LICENSE-EXCEPTION`. This does not replace any license below; see `LICENSING.md`.

- **Genesis Plus GX libretro core**: https://github.com/libretro/Genesis-Plus-GX, pinned commit `f27655b592ef43f317272d79883bcaa10d3ac8ad`. The current `tools/build-core.sh` delegates to `tools/build-core32x.sh`, applying the battery-memory extension and maintained GPX patches, including the `genesis-plus-gx-32x*.patch` hooks and `src/core32x` attachment. The integrated 32X hardware has its own versioned state extension; MD/CD execution remains in GPX. Its non-commercial license and embedded component notices are preserved in `third_party/Genesis-Plus-GX-LICENSE.txt`. This bundled core must not be treated as a permissively licensed commercial component.
- **Paprium cartridge HLE**: adapted from [pav1388/Genesis-Plus-GX-PUNCHiUM](https://github.com/pav1388/Genesis-Plus-GX-PUNCHiUM) at commit `3849f3d3432df1d6320574e73695dd379ecef2b3`. Its Paprium source credits Project Little Man and pav13. The donor's Genesis Plus GX non-commercial license and embedded notices are retained in `src/paprium/LICENSE`; source provenance, behavioral references, and experimental limitations are in `src/paprium/PROVENANCE.md`.
- **libretro API header**: copied from that exact source revision, `libretro/libretro-common/include/libretro.h`; license/copyright notice is preserved in the header.
- **Qt 6**: dynamically linked Core, Gui, Widgets, Multimedia, Network, OpenGL and OpenGLWidgets. Qt license notices and dependency licenses must accompany distributable Windows packages. See https://www.qt.io/licensing/open-source-lgpl-obligations and licenses supplied with the installed Qt distribution.

No game ROMs, Paprium soundtrack files, or console BIOS files are distributed. Integration test ROMs are original fixtures generated from the test source at runtime.

- **SDL 2.32.10**: joystick and controller discovery/input, dynamically linked; https://github.com/libsdl-org/SDL/tree/release-2.32.10. License preserved in `third_party/SDL2-LICENSE.txt`. Windows official MinGW archive is pinned by SHA-256 in `tools/prepare-sdl-windows.py`.
- **Press Start 2P**: pixel OSD typeface with Cyrillic by the Press Start 2P Project Authors; https://github.com/google/fonts/tree/main/ofl/pressstart2p. Font and SIL Open Font License are in `assets/fonts/`.

- **libarchive 3.8.10**: in-memory ZIP and 7z archive reading, statically linked; https://www.libarchive.org/. BSD notices preserved in third_party/libarchive-LICENSE.txt. The pinned build applies allocation/decompression caps to ZIP and 7z readers; see docs/archive-support.md and tools/prepare-archive.py.
- **XZ Utils / liblzma 5.8.4**: 7z LZMA/LZMA2 compression support, statically linked; https://tukaani.org/xz/. The linked liblzma is BSD Zero Clause licensed; notices preserved in third_party/liblzma-LICENSE.txt. XZ command-line tools are not bundled.
- **zlib 1.3.2**: ZIP Deflate compression support, statically linked; https://zlib.net/. License preserved in third_party/zlib-LICENSE.txt.

## minimp3

- Source: https://github.com/lieff/minimp3
- Revision: `ea99364f61c14656440e8d77e9c233ccf3124633`
- License: CC0-1.0, retained in `third_party/minimp3/LICENSE`.
- Vendored unmodified `minimp3.h` and `minimp3_ex.h` provide on-demand MP3 audio decoding and indexed seeks for virtual CD tracks. No complete decoded audio or converted files are retained.


## 32X source components

- **PicoDrive 32X devices and SH-2 CPU subset**: https://github.com/irixxxx/picodrive, pinned commit `07abb00550528f7bf53087d932dd3d747cf2d0df`. The reference interpreter, x86-64 DRC compiler/emitter and supporting headers are retained under `third_party/picodrive32x`; the bus, machine, PWM and SoC adaptations are under `src/core32x`. PicoDrive's complete non-commercial, MAME-style distribution terms are in `third_party/picodrive32x/COPYING`. Original file hashes, copied versus modified files, interpreter arithmetic fixes and DRC adaptations are recorded in `third_party/picodrive32x/ORIGIN.txt`; modified sources carry change notices and retain their original authorship notices.
- **Juergen Buchmueller's SH-2 interpreter**: the separate copyright and non-commercial freeware terms remain in full in `third_party/picodrive32x/cpu/sh2/mame/sh2.c`, including attribution to its earlier SH-2 implementation and contributors. These terms require author credit and notices on modified source files; they are additional to the PicoDrive notices. The local changes are identified before the original notice and detailed in `ORIGIN.txt`.
- **32X scanline compositor**: display decoding and priority rules in `src/core32x/video/compositor.c` are adapted from PicoDrive `pico/32x/draw.c` at the same pinned revision. Copyright notices credit notaz (2009–2010) and irixxxx (2019–2026). The complete accompanying non-commercial license and provenance are retained in `src/core32x/video/LICENSE` and `src/core32x/video/PROVENANCE.md`.
- **Blargg md_ntsc 0.1.2 RGB565 adaptation**: Copyright (C) 2006–2007 Shay Green, with the GPX adaptations credited to EkeEke. The kernel originates from `core/ntsc` at the GPX revision above. `third_party/blargg/md_ntsc_kernel_impl.h` and `src/core32x/video/ntsc565.c` retain LGPL 2.1-or-later notices. The optional full RGB565 adaptation uses a 65,536-entry table of fixed-width words (8 MiB), retaining all RGB565 input levels. Source provenance and local changes are in `third_party/blargg/PROVENANCE.txt`; the full LGPL 2.1 text is in `third_party/blargg/COPYING.LGPL-2.1`.
- **ares SH-2 FRT/DIVU reference**: behavior was cross-checked against ares revision `dc4f4659d49693ac91705389e6c37734c016bf16`. Copyright (c) 2004–2025 ares team, Near et al. The complete ISC permission and warranty notice is retained in the opening comment of `src/core32x/sh2/soc.c` alongside the PicoDrive provenance.

`copy_core32x_notices` in `tools/package_notices.py` includes these materials in both Windows and Linux packages: `licenses/picodrive32x/COPYING`, `ORIGIN.txt` and `COMPONENT-NOTICES.txt`; `licenses/blargg/COPYING.LGPL-2.1` and `PROVENANCE.txt`; and `licenses/core32x-video/LICENSE` and `PROVENANCE.md`. `COMPONENT-NOTICES.txt` preserves the original source-header notices, including the separate Buchmueller, DRC authors and ares terms. The GPX bundled license remains included separately.

## MegaLD source components

- **Pioneer PD6103A player and Sega PAC behavior**: adapted from [ares](https://github.com/ares-emulator/ares) revision `0b928930af4cc59ed99e942996f0d3c815379388`, including `ares/md/mcd/megald.cpp` and `cdd.cpp`. Copyright (c) 2004–2025 ares team, Near et al. The complete ISC license is retained in `src/megald/player/LICENSE`; source headers identify the adaptation.
- **QON / QOI2**: MIT-licensed headers from the same ares revision, by Roger Sanders (2025); QOI2 also credits Dominic Szablewski (2021). Full terms are in `src/megald/media/LICENSE.qon-qoi2`, with pinned source URLs and local decoder changes in `src/megald/media/UPSTREAM.md`. The MMI reader is original project code under GPL-3.0-or-later.

Both platform packages include `licenses/megald/LICENSE.ISC`, `LICENSE.qon-qoi2` and `UPSTREAM.md` through `copy_megald_notices` in `tools/package_notices.py`. No MMI images or PAC BIOS files are bundled.

## RetroAchievements runtime

- **rcheevos 12.5.0**: https://github.com/RetroAchievements/rcheevos at `1433173220a7eaede6a9ed7a18e94117be1821e0`. MIT license and copyright notices are retained in `third_party/rcheevos/LICENSE`. Unmodified public headers and runtime/API/hash/libretro-memory helper sources are vendored; `cmake/AchievementsDependencies.cmake` selects the linked subset. See `third_party/rcheevos/UPSTREAM.md`.
- Achievement definitions, badge artwork and account data are fetched from RetroAchievements for the signed-in user; they are not bundled with this project. This integration is not an endorsement or Hardcore approval by RetroAchievements.

- **GGPO (Flycast fork)**: MIT rollback and input networking SDK, pinned at Flycast `7adbf3caef251b153e348df3f469a0f35878339d`. Only the GGPO subtree is extracted; no Flycast emulator wrapper is copied. License and narrow transport/bounds changes are recorded in `third_party/GGPO-LICENSE.txt` and `third_party/GGPO-PROVENANCE.md`, included in both packages.

## MD Link Cable and cooperative execution

The controller-port wiring and external interrupt approach were studied from [Linn1024/zero-tolerance-link](https://github.com/Linn1024/zero-tolerance-link), a Genesis Plus GX based prototype. Hardware changes are adapted for this core with separate IRQ arbitration, portable paired execution and canonical audio state. See `tools/patches/genesis-plus-gx-link-cable.README.md` and the existing Genesis Plus GX license.

The x86-64 context switch implementation is libco by byuu (public domain), from libretro-common; its interface headers retain the RetroArch team's MIT notices. Exact provenance and notices are in `third_party/libco/README.txt` and `LICENSE.txt`.


## Super Eagle custom shader example

`assets/shaders/examples/super-eagle` contains the user-supplied Super Eagle preset and GLSL sources, retained unchanged. SuperEagle credits the DOSBox Team, guest(r), and Derek Liauw Kie Fa; its GNU GPL notice remains in the source and GPL-2 text is included as COPYING. The stock pass matches the style of [libretro/glsl-shaders](https://github.com/libretro/glsl-shaders/blob/master/stock.glsl). The files are shipped as editable external GLSL examples in `shaders/super-eagle`, not embedded in the executable.


## Minimal FFmpeg recording runtime

FFmpeg 7.1.2, LGPL-2.1-or-later, source: https://github.com/FFmpeg/FFmpeg/tree/n7.1.2. Both platforms build only PNG/AAC encoders and the MOV muxer (plus its upstream internal dependencies), with x86 assembly and runtime CPU dispatch. No GPL or nonfree options are enabled. Pinned source hashes and configuration are in `tools/prepare-video.py`; the distribution includes `licenses/ffmpeg` with LGPL text, source and rebuild instructions, and actual build configuration. PNG uses zlib. Qt multimedia plugins are not distributed; native Qt audio remains available.
