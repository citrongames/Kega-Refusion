#ifndef KEGA_CORE_COVERAGE_HOOKS_H
#define KEGA_CORE_COVERAGE_HOOKS_H

#include "DebugCoverageAbi.h"
#include "DebugExecutionAbi.h"
#include <stdbool.h>

enum kr_cov_source_id {
  KR_COV_CART_ROM = 1, KR_COV_MAIN_RAM, KR_COV_Z80_RAM, KR_COV_SAVE_RAM,
  KR_COV_MD_BOOT, KR_COV_SMS_BOOT, KR_COV_CD_BOOT, KR_COV_CD_PRG, KR_COV_CD_WORD, KR_COV_CD_BRAM,
  KR_COV_32X_BOOT, KR_COV_SH2_MASTER_BOOT, KR_COV_SH2_SLAVE_BOOT, KR_COV_32X_SDRAM,
  KR_COV_SH2_MASTER_DATA, KR_COV_SH2_SLAVE_DATA, KR_COV_32X_FB0, KR_COV_32X_FB1,
  KR_COV_SVP_IRAM, KR_COV_SVP_ROM, KR_COV_SVP_DRAM, KR_COV_SMS_FLASH
};


typedef struct kr_cov_access {
  const uint8_t *bytes; /* Canonical byte offset within native backing storage. */
  unsigned width;
} kr_cov_access;

/* The opcode dispatcher and generated instruction handlers are separate TUs. */
typedef struct kr_cov_prefetch_tag {
  unsigned epoch, address, value;
  kr_cov_location location[2];
} kr_cov_prefetch_tag;
extern kr_cov_prefetch_tag kr_cov_prefetch_tags[2];

#if defined(__GNUC__)
#define KR_COV_COLD __attribute__((cold,noinline))
#define KR_COV_ALWAYS_INLINE __attribute__((always_inline))
#else
#define KR_COV_COLD
#define KR_COV_ALWAYS_INLINE
#endif
#if defined(__GNUC__) && defined(__ELF__)
#define KR_COV_LOCAL __attribute__((visibility("hidden")))
#else
#define KR_COV_LOCAL
#endif
extern KR_COV_LOCAL unsigned kr_cov_exec_mask, kr_cov_read_mask, kr_cov_epoch;
kr_cov_location kr_cov_locate(uint32_t address, const uint8_t *bytes);
void kr_cov_emit(unsigned cpu, unsigned kind, kr_cov_location location, unsigned width);
void kr_cov_observe(unsigned cpu, unsigned kind, uint32_t address, const uint8_t *bytes, unsigned width);

kr_cov_access kr_cov_68k_data(unsigned cpu, uint32_t address, unsigned width);
kr_cov_access kr_cov_cd_data(unsigned cpu, uint32_t address, unsigned width);
kr_cov_access kr_cov_cd_pixel_data(unsigned cpu, uint32_t address, unsigned width);
kr_cov_access kr_cov_z80_data(uint32_t address);
kr_cov_access kr_cov_sms_data(uint32_t address);
unsigned kr_cov_sms_bios_size(void);
bool kr_cov_firecore_location(uint32_t address, const uint8_t *bytes, uint32_t *offset);

#ifdef KEGAREFUSION_32X
const uint8_t *kr_cov_32x_code(unsigned cpu, uint32_t address);
const uint8_t *kr_cov_32x_md_code_base(uint32_t address, const uint8_t *base);
void kr_cov_32x_bind(unsigned exec_cpus, unsigned read_cpus);
const uint8_t *kr_cov_32x_storage(unsigned id, unsigned *size);
kr_cov_access kr_cov_32x_md_data(unsigned cpu, uint32_t address, unsigned width);
#endif

#endif
