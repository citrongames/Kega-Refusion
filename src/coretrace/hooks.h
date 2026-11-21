#ifndef KEGA_CORE_TRACE_HOOKS_H
#define KEGA_CORE_TRACE_HOOKS_H

#include "DebugExecutionAbi.h"
#include "DebugTraceAbi.h"

extern unsigned kr_trace_cpu_mask, kr_trace_filters;

void kr_trace_exec(unsigned cpu, uint32_t pc, uint32_t opcode, unsigned width,
                   int64_t local_ticks);
void kr_trace_hardware(unsigned kind, unsigned device, unsigned cpu, int64_t local_ticks,
                       uint32_t address, uint32_t value, uint32_t aux, unsigned width);
/* SH-2 supplies both timestamps from its native scheduler and machine epoch. */
void kr_trace_sh2(unsigned cpu, unsigned kind, int64_t mclk, int64_t local_ticks,
                  uint32_t address, uint32_t value, uint32_t aux);

#endif
