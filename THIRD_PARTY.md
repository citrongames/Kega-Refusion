# Third-party components

- **Genesis Plus GX libretro core**: https://github.com/libretro/Genesis-Plus-GX, pinned commit `f27655b592ef43f317272d79883bcaa10d3ac8ad`. Unmodified upstream source compiled for each platform. Its non-commercial license and embedded component notices are preserved in `third_party/Genesis-Plus-GX-LICENSE.txt`. This bundled core must not be treated as a permissively licensed commercial component.
- **libretro API header**: copied from that exact source revision, `libretro/libretro-common/include/libretro.h`; license/copyright notice is preserved in the header.
- **Qt 6**: dynamically linked Core, Gui, Widgets and Multimedia. Qt license notices and dependency licenses must accompany distributable Windows packages. See https://www.qt.io/licensing/open-source-lgpl-obligations and licenses supplied with the installed Qt distribution.

No game ROMs or console BIOS files are distributed. Integration test ROMs are original fixtures generated from the test source at runtime.
