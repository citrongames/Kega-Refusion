#ifndef KEGA_DEBUG_INSPECTION_ABI_H
#define KEGA_DEBUG_INSPECTION_ABI_H
#include <stdint.h>
#include <stddef.h>
/* Runtime-only, versioned C interface. All data is copied by the owner thread.
 * Reads never call a guest bus handler. Byte offsets are domain-relative.
 * The ABI uses canonical guest byte order, independent of host endianness.
 * No structure in this file is serialized into native machine state. */
#define KR_INSPECT_VERSION 1u
#define KR_INSPECT_MAX_DOMAINS 32u
#define KR_INSPECT_MAX_VALUES 384u
#define KR_INSPECT_MAX_READ (256u * 1024u)
#define KR_INSPECT_MAX_WRITE 4u
enum kr_inspect_part { KR_INSPECT_MEMORY=1, KR_INSPECT_VIDEO=2,
    KR_INSPECT_AUDIO=4, KR_INSPECT_HARDWARE=8 };
enum kr_inspect_domain_id { KR_MEM_ROM=1, KR_MEM_RAM, KR_MEM_ZRAM,
    KR_MEM_VRAM, KR_MEM_CRAM, KR_MEM_VSRAM, KR_MEM_VDP_REGS, KR_MEM_SAVE,
    KR_MEM_CD_PRG, KR_MEM_CD_WORD, KR_MEM_CD_PCM, KR_MEM_CD_BRAM,
    KR_MEM_32X_SDRAM, KR_MEM_32X_FB0, KR_MEM_32X_FB1, KR_MEM_32X_CRAM,
    KR_MEM_SVP_IRAM, KR_MEM_SVP_DRAM };
enum kr_inspect_video { KR_VIDEO_NONE=0, KR_VIDEO_MD=1, KR_VIDEO_SMS=2,
    KR_VIDEO_SG=3, KR_VIDEO_FIRECORE_BITMAP=4 };
enum kr_inspect_hardware { KR_HW_MD=1, KR_HW_SMS=2, KR_HW_GG=4,
    KR_HW_SG=8, KR_HW_CD=16, KR_HW_32X=32, KR_HW_MEGALD=64,
    KR_HW_FIRECORE=128, KR_HW_PICO=256, KR_HW_SVP=512 };
typedef struct kr_inspect_domain {
    uint32_t id, size, address_base, big_endian;
    char name[48];
} kr_inspect_domain;
typedef struct kr_inspect_value {
    uint32_t part, digits;
    uint64_t value;
    char device[32], name[48];
} kr_inspect_value;
typedef struct kr_inspect_info {
    uint32_t version, struct_size, hardware, video_format;
    uint32_t width, height, domain_count, value_count;
    char system[48];
    uint8_t vdp_regs[32];
    kr_inspect_domain domains[KR_INSPECT_MAX_DOMAINS];
    kr_inspect_value values[KR_INSPECT_MAX_VALUES];
} kr_inspect_info;
/* bool retro_kegarefusion_inspect_info(unsigned version, unsigned parts,
 *                                      kr_inspect_info*, size_t capacity);
 * size_t retro_kegarefusion_inspect_read(unsigned domain, uint32_t offset,
 *                                        void* destination, size_t capacity);
 * bool retro_kegarefusion_inspect_writable(unsigned domain);
 * size_t retro_kegarefusion_inspect_write(unsigned domain, uint32_t offset,
 *                                         const void* source, size_t length);
 * Write only at a full-frame stop. The frontend authorizes the write and
 * validates the current snapshot. A rejected native write changes no bytes. */
#endif
