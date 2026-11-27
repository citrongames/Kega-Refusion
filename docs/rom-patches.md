# IPS and BPS patches

Enable **Automatically apply IPS/BPS patches** in emulator settings (off by default).
Open the ROM normally; there is no separate patch-loading dialog. This applies to
supported cartridge systems, including ROMs selected inside ZIP/7z archives.
Disc images (CUE/BIN, ISO, CHD and MegaLD MMI) are not patched.

## File names and priority

For `Game.md`, use `Game.ips` or `Game.bps`. Names are matched case-insensitively
on Windows and Linux. For an archive member `folder/Game.md`, use the basename
`Game`, with the external patch beside the archive or in the configured folder.
No recursive folder search or patch extraction from inside archives is performed.

Search order:

1. Beside the ROM/archive.
2. **Settings → Folders → Patches**, only when no matching patches are beside it.

The default Patches path is relative to the data directory (`data/patches` in a
portable package). Choosing a missing directory creates it. A chain is always
loaded entirely from one search location; the folders are never combined.

A numbered chain can mix both formats:

```text
Game.bps       # optional initial patch
Game.1.ips     # first numbered step
Game.2.bps     # second numbered step
```

Numbered steps start at 1 without gaps, with or without an initial unnumbered
patch. Leading zeros are allowed (`Game.01.ips`). Sorting is numeric. At a step
with both IPS and BPS, only BPS is applied. Ambiguous duplicates of the same
format, such as `.1.ips` plus `.01.ips`, are errors. There are at most 32 applied
patches per chain. An invalid preferred BPS does not silently fall back to IPS.

## Loading and saved data

Patches are read and applied on the emulation worker before hardware detection,
region selection, achievement hashing and optional ROM checksum correction.
IPS supports normal/RLE records and the optional output-size extension. BPS
supports all four operations and verifies source, target and patch CRC32.
IPS has no source checksum; use the ROM revision specified by the patch author.

The output ROM is limited to 128 MiB, each patch to 256 MiB, aggregate selected
patch input to 512 MiB. A failed chain is discarded before replacing the running
game. Original ROM and patch files are never rewritten. Successful loading
shows the patch name, or the number of applied patches, in the ordinary OSD.

Battery saves, state slots and cheats use the readable original name plus
` [p-xxxxxxxxxxxx]`, the first 12 hexadecimal digits of the final ROM's SHA-256.
Different patch encodings that produce the same ROM share that suffix; original
and modified games do not share their ordinary save files. Full content identity
continues to guard states, input recordings and network compatibility.

Changing the checkbox or files affects the next game load. Reset, power cycle,
rewind, input recording and replay retain the already loaded patched bytes.
There is no periodic patch scanning or additional work per emulated frame.

Netplay transfers the loaded patched ROM, including supported external music,
through the existing file-transfer worker. The receiver bypasses local patch
lookup for downloaded media. A participant opening their own original ROM can
apply matching local patches normally; final content must match the host.
