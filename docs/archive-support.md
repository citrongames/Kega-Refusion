# ROM archive backend

RomArchive uses only libarchive's seekable ZIP and 7z readers. QFile callbacks
provide Unicode outer paths on Linux and Windows. All decompressed cartridge
bytes remain in memory; member paths, including nested paths, are never created
on disk. Selection uses an all-header ordinal plus name and size, preserving
duplicate names and rejecting stale metadata. The caller hashes content bytes
for save identity, independently of the container name.

Only regular cartridge entries with .md, .gen, .bin, .smd, .sms, .gg,
.sg or .68k extensions appear in the selection list. Directories, links and
other files are excluded. Cartridges shorter than 512 bytes, encrypted archives, unsupported
compression methods, bad CRCs, truncation and malformed metadata return errors.
An unsupported archive codec never falls back to an external command.

## Limits

Both listing and extraction validate every entry, including non-ROM data.
Streaming limits are 512 MiB for the outer file, 128 MiB per member, 128 MiB for
all decoded data and 4096 headers. Thus a solid 7z stream or a large ignored file
cannot bypass the decoded-data budget. Listing/extraction are synchronous and
may decode a solid archive more than once.

The checksum-pinned libarchive source receives a local
patch in tools/prepare-archive.py, before compilation:

- ZIP central directory: at most 4096 entries and 8 MiB of metadata.
- 7z counts parsed as sizes: at most 4096.
- 7z encoded and decoded headers: at most 8 MiB.
- 7z unpacked coder streams: at most 128 MiB.
- ZIP LZMA/XZ and ZIP/7z PPMd decoder memory: at most 128 MiB.
- 7z LZMA/LZMA2 decoder: reject dictionaries requiring over 128 MiB before
  allocation, using lzma_raw_decoder_memusage.

These checks cover allocation/decompression performed inside libarchive's
header reader before the application can observe an entry. Source replacements
are checked against the exact pinned release, and a mismatch stops the build.
No process-global resource settings or host tools are used at runtime.

## Reproducible dependencies

Run python3 tools/prepare-archive.py --platform linux or --platform windows
before configuring CMake directly. The normal build scripts already do this.
The Windows path uses the existing MinGW toolchain. Linux and Windows link the
same pinned static source dependencies, requiring no extra archive DLLs.

Source tarballs and SHA-256 values are recorded in that script. Downloads,
verified unpacked sources, incremental dependency builds and install prefixes
live under ignored build/deps/. A script-hash stamp invalidates dependency
builds whenever the pinned versions or local cap patch changes. Only libarchive,
zlib and liblzma are linked; optional crypto, XML and external program
frontends are disabled. Archives using other optional codecs return a clear
unsupported-compression error.

archive_tests generates original bytes and ZIP/7z fixtures with libarchive's
writers. It tests selection, duplicate/nested names, every supported extension,
Unicode outer paths, corrupt/truncated/encrypted files, no disk extraction, CRC
checks, missing/stale selections, and all public archive limits.
