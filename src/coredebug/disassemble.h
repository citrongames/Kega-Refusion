#ifndef KR_CORE_DEBUG_DISASSEMBLE_H
#define KR_CORE_DEBUG_DISASSEMBLE_H

#include "DebugExecutionAbi.h"

/* Decode only the bounded canonical opcode bytes already copied to out. */
unsigned kr_debug_disassemble_m68k(kr_debug_instruction *out, unsigned available);
unsigned kr_debug_disassemble_z80(kr_debug_instruction *out, unsigned available);

#endif
