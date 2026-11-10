/* Optional 32X replacement boot ROMs. The 68000 vectors/routines and SH-2
 * programs at 0x200 are adapted from PicoDrive pico/32x/memory.c, commit
 * 07abb005. (C) notaz, 2009, 2010, 2013; (C) irixxxx, 2019-2024.
 * License: see third_party/picodrive32x/COPYING.
 * The added SH-2 entry code performs PicoDrive's reset-time cart setup on
 * the emulated processors. No original BIOS bytes or host-side ROM copy. */
#ifndef KEGA32X_BOOTROM_H
#define KEGA32X_BOOTROM_H
#include <stdint.h>
#include <string.h>
static const uint16_t master_program[] = {
  // trap instructions
  0xaffe, // 200 bra <self>
  0x0009, // 202 nop
  // have to wait a bit until m68k initial program finishes clearing stuff
  // to avoid races with game SH2 code, like in Tempo
  0xd406, // 204 mov.l   @(_m_ok,pc), r4
  0xc400, // 206 mov.b   @(h'0,gbr),r0
  0xc801, // 208 tst     #1, r0
  0x8b0f, // 20a bf      cd_start
  0xd105, // 20c mov.l   @(_cnt,pc), r1
  0xd206, // 20e mov.l   @(_start,pc), r2
  0x71ff, // 210 add     #-1, r1
  0x4115, // 212 cmp/pl  r1
  0x89fc, // 214 bt      -2
  0x6043, // 216 mov     r4, r0
  0xc208, // 218 mov.l   r0, @(h'20,gbr)
  0x6822, // 21a mov.l   @r2, r8
  0x482b, // 21c jmp     @r8
  0x0009, // 21e nop
  ('M'<<8)|'_', ('O'<<8)|'K', // 220 _m_ok
  0x0001, 0x0000,             // 224 _cnt
  0x2200, 0x03e0, // master start pointer in ROM
  // cd_start:
  0xd20d, // 22c mov.l   @(__cd_,pc), r2
  0xc608, // 22e mov.l   @(h'20,gbr), r0
  0x3200, // 230 cmp/eq  r0, r2
  0x8bfc, // 232 bf      #-2
  0xe000, // 234 mov     #0, r0
  0xcf80, // 236 or.b    #0x80,@(r0,gbr)
  0xd80b, // 238 mov.l   @(_start_cd,pc), r8 // 24000018
  0xd30c, // 23a mov.l   @(_max_len,pc), r3
  0x5b84, // 23c mov.l   @(h'10,r8), r11     // master vbr
  0x5a82, // 23e mov.l   @(8,r8), r10        // entry
  0x5081, // 240 mov.l   @(4,r8), r0         // len
  0x5980, // 242 mov.l   @(0,r8), r9         // dst
  0x3036, // 244 cmp/hi  r3,r0
  0x8b00, // 246 bf      #1
  0x6033, // 248 mov     r3,r0
  0x7820, // 24a add     #0x20, r8
  // ipl_copy:
  0x6286, // 24c mov.l   @r8+, r2
  0x2922, // 24e mov.l   r2, @r9
  0x7904, // 250 add     #4, r9
  0x70fc, // 252 add     #-4, r0
  0x8800, // 254 cmp/eq  #0, r0
  0x8bf9, // 256 bf      #-5
  //
  0x4b2e, // 258 ldc     r11, vbr
  0x6043, // 25a mov     r4, r0              // M_OK
  0xc208, // 25c mov.l   r0, @(h'20,gbr)
  0x4a2b, // 25e jmp     @r10
  0x0009, // 260 nop
  0x0009, // 262 nop          //     pad
  ('_'<<8)|'C', ('D'<<8)|'_', // 264 __cd_
  0x2400, 0x0018,             // 268 _start_cd
  0x0001, 0xffe0,             // 26c _max_len
};

static const uint16_t slave_program[] = {
  0xaffe, // 200 bra <self>
  0x0009, // 202 nop
  // code to wait for master, in case authentic master BIOS is used
  0xd106, // 204 mov.l   @(_m_ok,pc), r1
  0xd208, // 206 mov.l   @(_start,pc), r2
  0xc608, // 208 mov.l   @(h'20,gbr), r0
  0x3100, // 20a cmp/eq  r0, r1
  0x8bfc, // 20c bf      #-2
  0xc400, // 20e mov.b   @(h'0,gbr),r0
  0xc801, // 210 tst     #1, r0
  0xd004, // 212 mov.l   @(_s_ok,pc), r0
  0x8b0a, // 214 bf      cd_start
  0xc209, // 216 mov.l   r0, @(h'24,gbr)
  0x6822, // 218 mov.l   @r2, r8
  0x482b, // 21a jmp     @r8
  0x0009, // 21c nop
  0x0009, // 21e nop
  ('M'<<8)|'_', ('O'<<8)|'K', // 220
  ('S'<<8)|'_', ('O'<<8)|'K', // 224
  0x2200, 0x03e4,  // slave start pointer in ROM
  // cd_start:
  0xd803, // 22c mov.l   @(_start_cd,pc), r8 // 24000018
  0x5b85, // 22e mov.l   @(h'14,r8), r11     // slave vbr
  0x5a83, // 230 mov.l   @(h'0c,r8), r10     // entry
  0x4b2e, // 232 ldc     r11, vbr
  0xc209, // 234 mov.l   r0, @(h'24,gbr)     // write S_OK
  0x4a2b, // 236 jmp     @r10
  0x0009, // 238 nop
  0x0009, // 23a nop
  0x2400, 0x0018, // 23c _start_cd
};


static const uint16_t slave_entry[] = {
    0xd004, /* mov.l sys,r0 */
    0x401e, /* ldc r0,gbr */
    0xc400, /* mov.b @(0,gbr),r0 */
    0xc801, /* tst #1,r0 */
    0x8b02, /* bf start */
    0xd103, /* mov.l header,r1 */
    0x5016, /* mov.l @(24,r1),r0 */
    0x402e, /* ldc r0,vbr */
    0xafc0, /* bra guest_start */
    0x0009, /* nop */
    0x2000, /* sys */
    0x4000,
    0x2200, /* header */
    0x03d4,
};

/* Cartridge master, revision 2. The 68000 consumes COMM4 during its own
 * hardware initialization. Compute the word sum on the SH-2 clock, publish
 * it before copying the IPL, and only then allow the M_OK/S_OK handshake.
 * This also preserves the meaning of a zero checksum field (skip the sum).
 * The header end is inclusive and the boot ROM masks the word count to 22 bits. */
static const uint16_t cart_checksum[] = {
    0x402e, /* ldc r0,vbr (r1 is the IPL header and must survive this call) */
    0xd20a, /* mov.l checksum_field,r2 */
    0x6021, /* mov.w @r2,r0 */
    0x2008, /* tst r0,r0 */
    0x890d, /* bt publish */
    0xd209, /* mov.l rom_end_field,r2 */
    0x6322, /* mov.l @r2,r3 */
    0xd409, /* mov.l first_offset,r4 */
    0x3348, /* sub r4,r3 */
    0x4301, /* shlr r3 */
    0xd408, /* mov.l word_mask,r4 */
    0x2349, /* and r4,r3 */
    0x7301, /* add #1,r3 */
    0xd208, /* mov.l first_word,r2 */
    0xe000, /* mov #0,r0 */
    0x6525, /* mov.w @r2+,r5 */
    0x305c, /* add r5,r0 */
    0x4310, /* dt r3 */
    0x8bfb, /* bf sum */
    0xc114, /* mov.w r0,@(0x28,gbr) */
    0x000b, /* rts */
    0x0009, /* nop */
    0x2200, /* checksum_field */
    0x018e,
    0x2200, /* rom_end_field */
    0x01a4,
    0x0000, /* first_offset */
    0x0200,
    0x003f, /* word_mask */
    0xffff,
    0x2200, /* first_word */
    0x0200,
};

static void boot_word(uint8_t *p, unsigned offset, uint16_t v)
{
    p[offset] = (uint8_t)(v >> 8);
    p[offset + 1] = (uint8_t)v;
}
static void boot_long(uint8_t *p, unsigned offset, uint32_t v)
{
    boot_word(p, offset, (uint16_t)(v >> 16));
    boot_word(p, offset + 2, (uint16_t)v);
}
static void boot_words(uint8_t *p, unsigned offset, const uint16_t *code, unsigned bytes)
{
    for (unsigned i = 0; i < bytes / 2; ++i)
        boot_word(p, offset + 2 * i, code[i]);
}
/* Executed once after reset: establish GBR and, for a cartridge, VBR.
 * The master also transfers the initial data through the cartridge mapper.
 * CD software instead sends its program, entry points and vectors through
 * the framebuffer after the _CD_ handshake in master_program. */
static void write_master_entry(uint8_t *p, int cartridge)
{
    const uint16_t code[] = {
        0xd012, /* mov.l sys,r0 */
        0x401e, /* ldc r0,gbr */
        0xc400, /* mov.b @(0,gbr),r0 */
        0xc801, /* tst #1,r0 */
        0x8b1b, /* bf start */
        0xd111, /* mov.l header,r1 */
        cartridge ? 0xb040 : 0x5015, /* bsr cart_checksum / mov.l @(20,r1),r0 */
        cartridge ? 0x5015 : 0x402e, /* delay: load VBR / ldc r0,vbr */
        0x6212, /* mov.l @r1,r2      ; initial data offset in cartridge */
        0x5311, /* mov.l @(4,r1),r3  ; initial data offset in SDRAM */
        0x5412, /* mov.l @(8,r1),r4  ; byte count */
        0xd00f, /* mov.l capacity,r0 */
        0x3406, /* cmp/hi r0,r4 */
        0x8914, /* bt trap */
        0x4409, /* shlr2 r4 */
        0x2448, /* tst r4,r4 */
        0x890c, /* bt sum */
        0xd00d, /* mov.l rom_mask,r0 */
        0x2209, /* and r0,r2 */
        0xd00d, /* mov.l cart,r0 */
        0x220b, /* or r0,r2 */
        0xd00d, /* mov.l ram_mask,r0 */
        0x2309, /* and r0,r3 */
        0xd00d, /* mov.l ram,r0 */
        0x230b, /* or r0,r3 */
        0x6026, /* mov.l @r2+,r0 */
        0x2302, /* mov.l r0,@r3 */
        0x7304, /* add #4,r3 */
        0x4410, /* dt r4 */
        0x8bfa, /* bf copy */
        /* The current cart path has already published COMM4. Keep the CD
           boot image byte-identical to revision 1, including its unused cart path. */
        cartridge ? 0xafaa : 0xd10a, /* bra guest_start / mov.l checksum,r1 */
        cartridge ? 0x0009 : 0x6011, /* nop / mov.w @r1,r0 */
        0xc114, /* mov.w r0,@(0x28,gbr) */
        0xafa7, /* bra guest_start */
        0x0009, /* nop */
        0xafa3, /* bra guest_trap */
        0x0009, /* nop */
        0x0009, /* alignment */
        0x2000, /* sys */
        0x4000,
        0x2200, /* header */
        0x03d4,
        0x0004, /* capacity */
        0x0000,
        0x003f, /* rom_mask */
        0xfffc,
        0x2200, /* cart */
        0x0000,
        0x0003, /* ram_mask */
        0xfffc,
        0x2600, /* ram */
        0x0000,
        0x2200, /* checksum */
        0x018e,
    };
    boot_words(p, 0x270, code, sizeof(code));
}

static void builtin_boot_68k(uint8_t *p)
{
    static const uint16_t write_byte[] = {
        0x1280, 0x0239, 0x00fe, 0x00a1, 0x5107, 0x4e75
    };
    static const uint16_t write_banks[] = {
        0x48e7, 0x8040,               /* movem.l d0/a1,-(sp) */
        0x227c, 0x00a1, 0x30f1,       /* movea.l #0xa130f1,a1 */
        0x7007, 0x12d8, 0x5289,       /* copy eight bank registers */
        0x51c8, 0xfffa,
        0x0239, 0x00fe, 0x00a1, 0x5107, /* clear RV */
        0x4cdf, 0x0201                /* movem.l (sp)+,d0/a1 */
    };
    memset(p, 0, 256);
    for (unsigned i = 4; i < 0xc0; i += 4)
        boot_long(p, i, 0x880200 + (i / 4 - 1) * 6);
    boot_long(p, 0x70, 0);
    for (unsigned i = 0xc0; i < 0x100; i += 2)
        boot_word(p, i, 0x4e71);
    boot_words(p, 0xc8, write_byte, sizeof(write_byte));
    boot_words(p, 0xd4, write_banks, sizeof(write_banks));
    boot_word(p, 0xfe, 0x4e75);
}
static void builtin_boot_sh2(uint8_t *p, int slave, int cartridge)
{
    unsigned vector_end = slave ? 0x200 : 0x140;
    memset(p, 0, slave ? 1024 : 2048);
    for (unsigned i = 0; i < vector_end; i += 4)
        boot_long(p, i, 0x200);
    boot_long(p, 0, 0x270);
    boot_long(p, 4, slave ? 0x0603f800 : 0x06040000);
    boot_long(p, 8, 0x270);
    boot_long(p, 12, slave ? 0x0603f800 : 0x06040000);
    if (slave) {
        boot_words(p, 0x200, slave_program, sizeof(slave_program));
        boot_words(p, 0x270, slave_entry, sizeof(slave_entry));
    } else {
        /* Digital Pictures CD titles jump here directly to restart. */
        for (unsigned i = 0x140; i < 0x1fc; i += 2)
            boot_word(p, i, 0x0009);
        boot_word(p, 0x1fc, 0xa038); /* bra 0x270 */
        boot_word(p, 0x1fe, 0x0009);
        boot_words(p, 0x200, master_program, sizeof(master_program));
        write_master_entry(p, cartridge);
        if (cartridge)
            boot_words(p, 0x300, cart_checksum, sizeof(cart_checksum));
    }
}
#endif
