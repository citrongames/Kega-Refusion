/* Kaiser Wave cartridge identification. Shared by the core and netplay packer.
 * byte_xor is 0 for file order and 1 for word-swapped 68000 memory. */
#ifndef KEGA_YX5200_DETECT_H
#define KEGA_YX5200_DETECT_H
#include <string.h>

static inline int yx5200_match(const unsigned char *rom, unsigned offset, unsigned byte_xor,
                               const unsigned char *pattern, unsigned length)
{
    unsigned i;
    for (i = 0; i < length; ++i)
        if (rom[(offset + i) ^ byte_xor] != pattern[i]) return 0;
    return 1;
}

static inline int yx5200_detect(const unsigned char *rom, unsigned size, unsigned byte_xor)
{
    /* LEA $A13000,A1; construct a ten-bit UART frame; output D0 on /UWR.
     * The RAM location, code address, timing constant and ROM header can change. */
    static const unsigned char port[] = {0x43,0xf9,0x00,0xa1,0x30,0x00};
    static const unsigned char frame[] = {0x00,0x41,0x01,0x00,0xe3,0x49,0x74,0x09};
    static const unsigned char tx[] = {0x02,0x00,0x00,0xfe,0x80,0x03,0x12,0x80,0xe2,0x49};
    const unsigned char *cursor, *end;
    if (!rom || size < 0x230 || size > 0x800000 || byte_xor > 1) return 0;
    end = rom + (size & ~1u);
    cursor = rom + 0x200;
    while (cursor < end)
    {
        const unsigned char *hit = (const unsigned char *)memchr(cursor, 0xa1, (size_t)(end-cursor));
        unsigned at;
        if (!hit) break;
        at = (((unsigned)(hit-rom)) ^ byte_xor) - 3;
        if (!(at & 1) && at <= (unsigned)(end-rom) - 46 &&
            yx5200_match(rom, at, byte_xor, port, sizeof(port)) &&
            yx5200_match(rom, at+12, byte_xor, frame, sizeof(frame)) &&
            yx5200_match(rom, at+24, byte_xor, tx, sizeof(tx))) return 1;
        cursor = hit + 1;
    }
    return 0;
}

/* Four Winds copies its AMD/Spansion CFI driver from ROM to work RAM. Identify
 * the program unlock sequence and its sector erase operation in bank $3F.
 * Check only after finding the board UART; ordinary SGDK saves use other maps. */
static inline int yx5200_flash_save(const unsigned char *rom, unsigned size, unsigned byte_xor)
{
    static const unsigned char program[] = {0x30,0x7c,0x0a,0xab,0x10,0xbc,0xff,0xaa,
        0x11,0xfc,0x00,0x55,0x05,0x55,0x10,0xbc,0xff,0xa0};
    static const unsigned char erase[] = {0x13,0xfc,0x00,0x30,0x00,0x3f,0xf0,0x01};
    const unsigned char *cursor = rom, *end = rom + (size & ~1u);
    while (cursor < end)
    {
        const unsigned char *hit = (const unsigned char *)memchr(cursor, 0xab, (size_t)(end-cursor));
        unsigned at, pos, limit;
        if (!hit) break;
        at = (((unsigned)(hit-rom)) ^ byte_xor) - 3;
        if (!(at & 1) && at <= (unsigned)(end-rom) - sizeof(program) &&
            yx5200_match(rom, at, byte_xor, program, sizeof(program)))
        {
            limit = at + 512;
            if (limit > (unsigned)(end-rom) - sizeof(erase)) limit = (unsigned)(end-rom) - sizeof(erase);
            for (pos = at + sizeof(program); pos <= limit; pos += 2)
                if (yx5200_match(rom, pos, byte_xor, erase, sizeof(erase))) return 1;
        }
        cursor = hit + 1;
    }
    return 0;
}
#endif
