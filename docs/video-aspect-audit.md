# Video aspect audit: «Как на консоли»

3 November 2026. Scope: the supported Genesis Plus GX hardware modes and the local 32X extension, from core geometry to GameView. This is a code-path audit with a focused filter test, not a ROM-by-ROM visual survey.

## Presentation contract

- The core computes `vaspect_ratio` in `libretro/libretro.c:1319-1347` and reports it through `retro_get_system_av_info()` and dynamic `SET_GEOMETRY` / `SET_SYSTEM_AV_INFO`. The host stores the update and `LibretroHost::aspectRatio()` returns the positive core value. `EmulationSession::publishVideo()` carries it with the frame. `GameView::gameRect()` uses it for Console mode; SquarePixels uses native raster dimensions, and Stretch uses the widget dimensions.
- Therefore «Как на консоли» means **core-provided display aspect**, not a forced 4:3 rectangle. The selected core PAR, game resolution, region and crop can all alter the resulting rectangle. This follows the [libretro geometry contract](https://github.com/libretro/libretro-common/blob/master/include/libretro.h) and the [official core options](https://docs.libretro.com/library/genesis_plus_gx/).
- Integer scaling uses native raster height as its integer unit and multiplies width by the selected display aspect. Blargg changes horizontal sample count only, so its filtered width must not become the native unit.

## Core formula and systems

With core option `genesis_plus_gx_aspect_ratio=auto`, except for the default GG special case:

```
dotrate = system_clock / (H40 ? 8 : 10)
sample_rate = vdp_pal ? 14,750,000 : 135,000,000 / 11
display_aspect = (sample_rate / dotrate) * (native_viewport_width - SMS_border_crop) / (native_viewport_height * 2)
```

`system_clock` defaults to 53,693,175 Hz NTSC or 53,203,424 Hz PAL; explicit clock/VDP options can override them separately. H40 is 320 pixels; H32 and 8-bit modes use 256. The local 32X patch composes onto a 320-pixel raster and selects H40 timing even when the MD VDP starts in H32. The formula is computed *before* Blargg expands samples and before interlace doubles the output height. This preserves display aspect across those transforms.

| Hardware path | Native viewport and aspect behavior |
| --- | --- |
| MD/Genesis cartridge, Sega/Mega CD, Pico | MD H32/H40 and 192/224/240 active lines; core PAR formula above. CD and Pico use the same display geometry path. |
| 32X cartridge and CD+32X | Local extension outputs a composed 320-pixel MD raster. Aspect uses H40 dot clock and the same region/height formula; Blargg outputs 640 samples without horizontal overscan, 668 with it. |
| Master System I/II, Mark III, Power Base Converter | H32 256-pixel raster, 192/224/240 active lines and the core PAR formula. Optional SMS side-border crop removes 8 or 16 native pixels, and the core includes this in its aspect. |
| SG-1000, SG-1000 II, SG-1000 II RAM extension | 8-bit/H32 geometry and core PAR. The distinct hardware variants do not require a separate GameView policy. |
| Game Gear | With default crop and no overscan, 160×144 with a special 6:5 pixel aspect, yielding 4:3 for that **specific** crop. Extended screen or overscan returns to the generic PAR formula. |
| GGMS (SMS game on Game Gear) | Its default viewport is SMS-like; the core special case still applies a 6:5 pixel aspect while `overscan=disabled` and `gg_extra=disabled`. For 256×192 this gives 1.6, not 4:3. |

Examples of the core formula with default clocks/options, rounded: NTSC 320×224 MD/32X = 1.3061; NTSC 256×224 MD H32 = 1.3061; NTSC 256×192 SMS/SG = 1.5238; PAL 320×224 MD = 1.5842; PAL 320×240 MD = 1.4786; PAL 256×192 SMS/SG = 1.8483. These are the *visible raster* proportions selected by the core, not claims that every television or cartridge has exactly these physical proportions. Forced NTSC PAR, PAL PAR, 4:3 and Uncorrected core options intentionally change the contract.

Vertical overscan adds blank/edge lines to the viewport (region dependent). Horizontal overscan adds seven pixels on each side for ordinary modes; cropped GG uses a different viewport transition. The core recomputes aspect after those changes. With `genesis_plus_gx_render=double field`, interlaced output height doubles; aspect is calculated from the pre-doubled height, so Console mode remains visually stable. SquarePixels uses the doubled native raster dimensions, as specified by that mode.

## Confirmed defects and repair

1. The old host inferred native width by rounding Blargg output back to a multiple of four. Horizontal overscan permits 270 or 334 native pixels, so it inferred 272 for H32 and 332 for H40. For 32X, 334→668 was not recognized at all. This affected SquarePixels and integer scaling, **not** Console aspect, because Console consumes the separate core ratio.
2. The 32X RGB565 Blargg implementation rejected 334 input pixels despite the core sending that width with horizontal overscan. The row was left unwritten. `src/core32x/video/ntsc565.c` now handles the two-pixel tail and emits exactly 668 samples. The focused `core32x_ntsc565_tests` case checks output and row bounds.
3. The host now prefers an exact per-frame native-size getter from the bundled core; its older-core fallback recognizes the concrete viewport widths. The bundled core exports this getter in the debug-video patch. It returns the viewport width after native SMS crop and the final interlaced height, independent of filtered sample count.

The default core overscan setting is **disabled**. The 334-pixel issue requires enabling horizontal/full overscan through a core option or restoring such a saved option set. No evidence shows that the current compact UI exposes that option directly.

## Validation and limits

The standalone RGB565 filter test passed after the 334-pixel change. Existing integration tests cover native metadata for default MD H32/H40, SMS and GG with Blargg, but not every region, overscan, SMS crop, GG extended display, interlace, Pico or CD+32X combination. The code-path conclusions for those combinations are based on the inspected core and frontend formulas. The final Linux and Windows builds succeeded. The Linux `core_integration_tests --video-geometry` run passed MD H32/H40, SMS and GG metadata/aspect preservation across Blargg presets. Visual comparison with physical hardware was not performed.

Primary references: [Genesis Plus GX documentation](https://docs.libretro.com/library/genesis_plus_gx/), [upstream libretro core source](https://github.com/ekeeke/Genesis-Plus-GX/blob/master/libretro/libretro.c), [upstream VDP viewport source](https://github.com/ekeeke/Genesis-Plus-GX/blob/master/core/vdp_ctrl.c). The local 32X behavior is project-specific and must be verified against the local patch/source, not upstream Genesis Plus GX (which does not support 32X).
