#ifndef KR_CORE_DEBUG_HOOKS_H
#define KR_CORE_DEBUG_HOOKS_H

#include "DebugExecutionAbi.h"
#include <stdbool.h>

/* Private effective-mask bits shared with the native coverage observer. */
enum {
  KR_DEBUG_COV_EXEC = 0x100, KR_DEBUG_COV_READ = 0x200,
  KR_DEBUG_COV_BITS = KR_DEBUG_COV_EXEC | KR_DEBUG_COV_READ
};
#if defined(__GNUC__) && defined(__ELF__)
#define KR_DEBUG_LOCAL __attribute__((visibility("hidden")))
#else
#define KR_DEBUG_LOCAL
#endif
extern KR_DEBUG_LOCAL unsigned kr_debug_event_masks[KR_DEBUG_MAX_CPUS];
#define kr_debug_m68k_mask (kr_debug_event_masks[0])
#define kr_debug_z80_mask  (kr_debug_event_masks[1])
#define kr_debug_sub68k_mask (kr_debug_event_masks[2])
#define kr_debug_sh2_master_mask (kr_debug_event_masks[3])
#define kr_debug_sh2_slave_mask  (kr_debug_event_masks[4])
#define kr_debug_ssp_mask (kr_debug_event_masks[5])

extern int kr_debug_z80_irq_recheck;
void kr_debug_notify(unsigned cpu, unsigned event, uint32_t address,
                     uint32_t value, unsigned size);

unsigned kr_debug_m68k_sr(void);
unsigned kr_debug_sub68k_sr(void);
void kr_debug_write_m68k(unsigned reg, uint32_t value, int before_instruction);
void kr_debug_write_sub68k(unsigned reg, uint32_t value, int before_instruction);
int kr_debug_m68k_prefetch(uint32_t address, uint16_t *word);
int kr_debug_peek_sms(uint32_t address, uint8_t *byte);
void kr_debug_ssp_state(kr_debug_cpu *out);
uint8_t kr_debug_ssp_peek(uint32_t byte_address);
bool kr_debug_ssp_writable(const char *name);
bool kr_debug_ssp_write(const char *name, uint32_t value);
int kr_debug_peek_cd(unsigned cpu, uint32_t address, uint8_t *byte);
int kr_debug_peek_cd_graphics(unsigned int (*read_byte)(unsigned int),
                            uint32_t address, uint8_t *byte);

#ifdef KEGAREFUSION_32X
void kr_debug_32x_connect(kr_debug_callback callback, void *opaque,
                          unsigned master_mask, unsigned slave_mask);
bool kr_debug_32x_state(unsigned cpu, kr_debug_cpu *out);
bool kr_debug_32x_peek(unsigned cpu, uint32_t address, uint8_t *byte);
bool kr_debug_32x_peek_md(unsigned cpu, uint32_t address, uint8_t *byte);
bool kr_debug_32x_disassemble(unsigned cpu, uint32_t address, kr_debug_instruction *out);
bool kr_debug_32x_writable(unsigned cpu, const char *name);
bool kr_debug_32x_write(unsigned cpu, const char *name, uint32_t value);
#endif

#endif
