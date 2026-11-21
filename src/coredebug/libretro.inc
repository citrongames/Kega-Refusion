/* Called by libretro.c after the inspection lifecycle flag is declared. */
#include "hooks.h"
#include "disassemble.h"

unsigned kr_debug_m68k_mask, kr_debug_z80_mask;
static kr_debug_callback kr_debug_callback_fn;
static void *kr_debug_callback_user;

static void kr_debug_detach(void)
{
  kr_debug_m68k_mask = kr_debug_z80_mask = 0;
  kr_debug_callback_fn = NULL;
  kr_debug_callback_user = NULL;
}

static unsigned kr_debug_cpu_set(void)
{
  if (!kr_inspect_content || io_link_yield || gg_link_yield)
    return 0;
  switch (system_hw)
  {
    case SYSTEM_MD:
      if (kr_firecore_active || svp)
        return 0;
#ifdef KEGAREFUSION_32X
      if (kegarefusion_32x_attached)
        return 0;
#endif
      return KR_DEBUG_M68K | KR_DEBUG_Z80;
    case SYSTEM_MARKIII:
    case SYSTEM_SMS:
    case SYSTEM_SMS2:
    case SYSTEM_PBC:
    case SYSTEM_GG:
    case SYSTEM_GGMS:
    case SYSTEM_SG:
    case SYSTEM_SGII:
    case SYSTEM_SGII_RAM_EXT:
      return KR_DEBUG_Z80;
    default:
      return 0;
  }
}

static unsigned kr_debug_address_mask(unsigned cpu)
{
  return cpu == KR_DEBUG_M68K ? UINT32_C(0xffffff) : UINT32_C(0xffff);
}

static int kr_debug_cpu_available(unsigned cpu)
{
  return (cpu == KR_DEBUG_M68K || cpu == KR_DEBUG_Z80) &&
         (kr_debug_cpu_set() & cpu);
}

RETRO_API bool retro_kegarefusion_debug_connect(kr_debug_callback callback, void *opaque,
                                               unsigned m68k_mask, unsigned z80_mask)
{
  if (!callback)
  {
    kr_debug_detach();
    return true;
  }
  const unsigned cpus = kr_debug_cpu_set();
  const unsigned events = KR_DEBUG_EXEC | KR_DEBUG_READ | KR_DEBUG_WRITE;
  if (!cpus || ((m68k_mask | z80_mask) & ~events) ||
      (m68k_mask && !(cpus & KR_DEBUG_M68K)) || (z80_mask && !(cpus & KR_DEBUG_Z80)))
    return false;

  kr_debug_callback_user = opaque;
  kr_debug_callback_fn = callback;
  kr_debug_m68k_mask = m68k_mask;
  kr_debug_z80_mask = z80_mask;
  return true;
}

void kr_debug_notify(unsigned cpu, unsigned event, uint32_t address,
                     uint32_t value, unsigned size)
{
  if (kr_debug_callback_fn)
    kr_debug_callback_fn(kr_debug_callback_user, cpu, event,
                         address & kr_debug_address_mask(cpu), value, size);
}

static void kr_debug_register(kr_debug_cpu *out, const char *name,
                              unsigned digits, unsigned value)
{
  kr_debug_reg *entry = &out->regs[out->count++];
  snprintf(entry->name, sizeof(entry->name), "%s", name);
  entry->value = value;
  entry->digits = digits;
}

static void kr_debug_state_m68k(kr_debug_cpu *out)
{
  static const char *const names[] = {
    "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
    "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"
  };

  out->pc = m68k_get_reg(M68K_REG_PC) & kr_debug_address_mask(KR_DEBUG_M68K);
  out->sp = m68k_get_reg(M68K_REG_SP) & kr_debug_address_mask(KR_DEBUG_M68K);
  out->cycles = m68k.cycles;
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    kr_debug_register(out, names[i], 8, m68k_get_reg((m68k_register_t)(M68K_REG_D0 + i)));
  kr_debug_register(out, "PC", 6, out->pc);
  kr_debug_register(out, "SR", 4, kr_debug_m68k_sr());
  kr_debug_register(out, "USP", 8, m68k_get_reg(M68K_REG_USP));
  kr_debug_register(out, "SSP", 8, m68k_get_reg(M68K_REG_ISP));
  kr_debug_register(out, "IR", 4, m68k.ir);
  kr_debug_register(out, "STOP", 2, m68k.stopped);
}

static void kr_debug_state_z80(kr_debug_cpu *out)
{
  out->pc = Z80.pc.w.l;
  out->sp = Z80.sp.w.l;
  out->cycles = Z80.cycles;
  kr_debug_register(out, "PC", 4, out->pc);
  kr_debug_register(out, "SP", 4, out->sp);
  kr_debug_register(out, "AF", 4, Z80.af.w.l);
  kr_debug_register(out, "BC", 4, Z80.bc.w.l);
  kr_debug_register(out, "DE", 4, Z80.de.w.l);
  kr_debug_register(out, "HL", 4, Z80.hl.w.l);
  kr_debug_register(out, "IX", 4, Z80.ix.w.l);
  kr_debug_register(out, "IY", 4, Z80.iy.w.l);
  kr_debug_register(out, "AF'", 4, Z80.af2.w.l);
  kr_debug_register(out, "BC'", 4, Z80.bc2.w.l);
  kr_debug_register(out, "DE'", 4, Z80.de2.w.l);
  kr_debug_register(out, "HL'", 4, Z80.hl2.w.l);
  kr_debug_register(out, "WZ", 4, Z80.wz.w.l);
  kr_debug_register(out, "I", 2, Z80.i);
  kr_debug_register(out, "R", 2, (Z80.r & 0x7f) | (Z80.r2 & 0x80));
  kr_debug_register(out, "IM", 1, Z80.im);
  kr_debug_register(out, "IFF1", 1, !!Z80.iff1);
  kr_debug_register(out, "IFF2", 1, !!Z80.iff2);
  kr_debug_register(out, "IRQ", 1, !!Z80.irq_state);
  kr_debug_register(out, "NMI", 1, !!Z80.nmi_state);
  kr_debug_register(out, "HALT", 1, !!Z80.halt);
  if (system_hw == SYSTEM_MD)
    kr_debug_register(out, "BUS", 2, zstate);
}

RETRO_API bool retro_kegarefusion_debug_state(unsigned cpu, kr_debug_cpu *out, size_t capacity)
{
  if (!out || capacity < sizeof(*out) || !kr_debug_cpu_available(cpu))
    return false;

  memset(out, 0, sizeof(*out));
  out->cpu = cpu;
  if (cpu == KR_DEBUG_M68K)
    kr_debug_state_m68k(out);
  else
    kr_debug_state_z80(out);
  return true;
}

static int kr_debug_peek_m68k(uint32_t address, uint8_t *byte)
{
  const cpu_memory_map *map = &m68k.memory_map[address >> 16];
  if (map->read8 == sram_read_byte)
  {
    *byte = sram.sram[address & 0xffff];
    return 1;
  }
  if (!map->base || map->read8)
    return 0;
  *byte = READ_BYTE(map->base, address & 0xffff);
  return 1;
}

static int kr_debug_peek_md_z80(uint32_t address, uint8_t *byte)
{
  if (address < 0x4000)
  {
    *byte = zram[address & 0x1fff];
    return 1;
  }
  if (address < 0x8000)
    return 0; /* FM, VDP, bank control, and floating-bus ranges. */

  /* The Z80 bank can use a different handler for the same logical MD page. */
  address = (zbank | (address & 0x7fff)) & 0xffffff;
  const unsigned page = address >> 16;
  const uint8_t *base = m68k.memory_map[page].base;
  if (!base || zbank_memory_map[page].read)
    return 0;
  *byte = READ_BYTE(base, address & 0xffff);
  return 1;
}

RETRO_API size_t retro_kegarefusion_debug_peek(unsigned cpu, uint32_t address,
                                               uint8_t *bytes, uint8_t *validity, size_t capacity)
{
  const uint32_t mask = kr_debug_address_mask(cpu);
  if (!bytes || !validity || address > mask || !kr_debug_cpu_available(cpu))
    return 0;
  if (capacity > KR_DEBUG_MAX_PEEK)
    capacity = KR_DEBUG_MAX_PEEK;

  int (*peek)(uint32_t, uint8_t *) = cpu == KR_DEBUG_M68K ? kr_debug_peek_m68k :
        system_hw == SYSTEM_MD ? kr_debug_peek_md_z80 : kr_debug_peek_sms;
  for (size_t i = 0; i < capacity; ++i)
  {
    bytes[i] = 0;
    validity[i] = (uint8_t)peek((address + i) & mask, &bytes[i]);
  }
  return capacity;
}

static unsigned kr_debug_code(unsigned cpu, uint32_t address, uint8_t *bytes, unsigned capacity)
{
  const uint32_t mask = kr_debug_address_mask(cpu);
  unsigned count;
  for (count = 0; count < capacity; ++count)
  {
    const uint32_t offset = (address + count) & mask;
    if (cpu == KR_DEBUG_M68K)
    {
      const uint8_t *base = m68k.memory_map[offset >> 16].base;
      if (!base) break;
      bytes[count] = READ_BYTE(base, offset & 0xffff);
    }
    else
    {
      const uint8_t *base = z80_readmap[offset >> 10];
      if (!base) break;
      bytes[count] = base[offset & 0x3ff];
    }
  }

  /* The next instruction may use a cached opcode after a bank switch or
   * self-modifying code. Later rows continue to show mapped memory. */
  if (cpu == KR_DEBUG_M68K && count >= 2)
  {
    uint16_t opcode;
    if (kr_debug_m68k_prefetch(address, &opcode))
    {
      bytes[0] = (uint8_t)(opcode >> 8);
      bytes[1] = (uint8_t)opcode;
    }
  }
  return count;
}

RETRO_API bool retro_kegarefusion_debug_disassemble(unsigned cpu, uint32_t address,
                                                    kr_debug_instruction *out, size_t capacity)
{
  if (!out || capacity < sizeof(*out) || !kr_debug_cpu_available(cpu) ||
      address > kr_debug_address_mask(cpu))
    return false;

  memset(out, 0, sizeof(*out));
  out->address = address;
  const unsigned alignment = cpu == KR_DEBUG_M68K ? 2 : 1;
  if (address % alignment)
    return true;
  const unsigned available = kr_debug_code(cpu, address, out->bytes, sizeof(out->bytes));
  if (available < alignment)
    return true;
  const unsigned length = cpu == KR_DEBUG_M68K ? kr_debug_disassemble_m68k(out, available) :
                                                  kr_debug_disassemble_z80(out, available);
  if (length && length <= available)
  {
    out->length = length;
    out->valid = 1;
    memset(out->bytes + length, 0, sizeof(out->bytes) - length);
  }
  else
  {
    memset(out->bytes, 0, sizeof(out->bytes));
    out->text[0] = '\0';
    out->flags = 0;
  }
  return true;
}
