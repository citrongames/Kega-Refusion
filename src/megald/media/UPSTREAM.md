# QON / QOI2 provenance

qon.h and qoi2.h are from ares-emulator/ares, commit
0b928930af4cc59ed99e942996f0d3c815379388 (retrieved 2026-11-14):

- https://github.com/ares-emulator/ares/blob/0b928930af4cc59ed99e942996f0d3c815379388/thirdparty/qon/qon.h
- https://github.com/ares-emulator/ares/blob/0b928930af4cc59ed99e942996f0d3c815379388/thirdparty/qon/qoi2.h

Both upstream files declare MIT (Roger Sanders, QOI2 additionally credits
Dominic Szablewski). Full terms are in LICENSE.qon-qoi2.

Local hardening: check the descriptor before dereferencing it, prevent pixel
size multiplication overflow, check literal RGB continuation bounds, reject
runs beyond the output image, and require full consumption of the QOI2 payload.
Removed one unused local in QON; expressed two binary constants as hex for C99. The heap API is disabled in media.c.
No dependency on nall, ares emulator code, external codecs, or writable mapping.

media.c and media.h are original project code under GPL-3.0-or-later.
They implement the stored ZIP/ZIP64 MMI container documented by ares:
https://github.com/ares-emulator/ares/blob/0b928930af4cc59ed99e942996f0d3c815379388/nall/nall/decode/mmi.hpp

Limits: 512 archive entries; 8 MiB central directory; 1 MiB JSON / 16384 tokens /
32 nesting levels; 256 KiB CUE; 16 sides; 99 tracks; 2048*2048 pixels;
2 million QON frames per side. Video indexes are read on demand. Random access
to interframe QON reconstructs at most 600 preceding dependency frames; normal
sequential access reuses the preceding decoded frame. The public API documents
the independent video/audio/digital reader thread contract.
