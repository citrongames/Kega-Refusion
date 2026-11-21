#ifndef KR_CORE_DEBUG_HOOKS_H
#define KR_CORE_DEBUG_HOOKS_H

#include "DebugExecutionAbi.h"

extern unsigned kr_debug_m68k_mask, kr_debug_z80_mask;
void kr_debug_notify(unsigned cpu, unsigned event, uint32_t address,
                     uint32_t value, unsigned size);

unsigned kr_debug_m68k_sr(void);
int kr_debug_m68k_prefetch(uint32_t address, uint16_t *word);
int kr_debug_peek_sms(uint32_t address, uint8_t *byte);

#endif
