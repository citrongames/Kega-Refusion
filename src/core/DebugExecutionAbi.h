#ifndef KEGA_DEBUG_EXECUTION_ABI_H
#define KEGA_DEBUG_EXECUTION_ABI_H

#include <stddef.h>
#include <stdint.h>

/* Runtime-only interface. Calls and callbacks share the owning emulation
 * thread. A paused instruction may hold the emulated frame's native stack. */
#define KR_DEBUG_VERSION 1u
#define KR_DEBUG_MAX_REGS 32u
#define KR_DEBUG_MAX_PEEK 4096u

enum kr_debug_cpu_id { KR_DEBUG_M68K = 1, KR_DEBUG_Z80 = 2 };

enum kr_debug_event {
    KR_DEBUG_EXEC = 1,     /* Before opcode fetch. size, value are zero. */
    KR_DEBUG_READ = 2,     /* After a CPU data read. */
    KR_DEBUG_WRITE = 4    /* After a CPU data write has reached the bus. */
};

enum kr_debug_instruction_flag { KR_DEBUG_CALL = 1, KR_DEBUG_RETURN = 2 };

typedef void (*kr_debug_callback)(void *opaque, unsigned cpu, unsigned event,
                                  uint32_t address, uint32_t value, unsigned size);

typedef struct kr_debug_reg {
    char name[12];
    uint32_t value, digits;
} kr_debug_reg;

typedef struct kr_debug_cpu {
    uint32_t cpu, pc, sp, count;
    uint64_t cycles;     /* Core-local master cycles within the current frame. */
    kr_debug_reg regs[KR_DEBUG_MAX_REGS];
} kr_debug_cpu;

typedef struct kr_debug_instruction {
    uint32_t address, length, flags, valid;
    uint8_t bytes[16];
    char text[160];
} kr_debug_instruction;

/* bool retro_kegarefusion_debug_connect(kr_debug_callback, void *opaque,
 *                                      unsigned m68k_mask, unsigned z80_mask);
 * A null callback detaches regardless of the masks. The masks combine event
 * bits. An execute event has size zero. Trap addresses are logical.
 *
 * bool retro_kegarefusion_debug_state(unsigned cpu, kr_debug_cpu *, size_t);
 * bool retro_kegarefusion_debug_disassemble(unsigned cpu, uint32_t address,
 *                                          kr_debug_instruction *, size_t);
 *
 * size_t retro_kegarefusion_debug_peek(unsigned cpu, uint32_t address,
 *                                     uint8_t *bytes, uint8_t *validity, size_t);
 * Peek reads logical data without calling bus handlers. Bytes/validity have
 * the same length. Zero validity denotes unavailable data and zeroes its byte.
 * Disassembly instead follows the core's actual opcode-fetch map.
 */
#endif
