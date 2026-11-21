/* Called by libretro.c after the inspection lifecycle flag is declared. */
#include "hooks.h"
#include "disassemble.h"

unsigned kr_debug_event_masks[KR_DEBUG_MAX_CPUS];
int kr_debug_z80_irq_recheck;
static kr_debug_callback kr_debug_callback_fn;
static void *kr_debug_callback_user;
static unsigned kr_debug_callback_cpu, kr_debug_callback_event;

static void kr_debug_detach(void)
{
#ifdef KEGAREFUSION_32X
  kr_debug_32x_connect(NULL, NULL, 0, 0);
#endif
  for (unsigned i = 0; i < KR_DEBUG_MAX_CPUS; ++i) kr_debug_event_masks[i] &= KR_DEBUG_COV_BITS;
  kr_debug_callback_fn = NULL;
  kr_debug_callback_user = NULL;
}

static unsigned kr_debug_cpu_set(void)
{
  if (!kr_inspect_content)
    return 0;
  unsigned addon = 0;
#ifdef KEGAREFUSION_32X
  if (kegarefusion_32x_attached)
    addon = KR_DEBUG_SH2_MASTER | KR_DEBUG_SH2_SLAVE;
#endif
  switch (system_hw)
  {
    case SYSTEM_MCD:
      return addon | KR_DEBUG_M68K | KR_DEBUG_Z80 | KR_DEBUG_SUB68K;
    case SYSTEM_MD:
      return addon | KR_DEBUG_M68K | KR_DEBUG_Z80 | (svp ? KR_DEBUG_SSP1601 : 0);
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
  switch (cpu)
  {
    case KR_DEBUG_M68K:
    case KR_DEBUG_SUB68K:
      return UINT32_C(0xffffff);
    case KR_DEBUG_SH2_MASTER:
    case KR_DEBUG_SH2_SLAVE:
      return UINT32_MAX;
    default:
      return UINT32_C(0xffff);
  }
}

static int kr_debug_cpu_available(unsigned cpu)
{
  return cpu && (cpu & (cpu - 1)) == 0 && (kr_debug_cpu_set() & cpu);
}

static int kr_debug_is_sh2(unsigned cpu)
{
  return cpu == KR_DEBUG_SH2_MASTER || cpu == KR_DEBUG_SH2_SLAVE;
}

static int kr_debug_target_info(unsigned cpu, kr_debug_target *out)
{
  if (!kr_debug_cpu_available(cpu))
    return 0;

  const char *name;
  out->cpu = cpu;
  out->address_shift = 0;
  out->capabilities = KR_DEBUG_CAP_EXEC | KR_DEBUG_CAP_READ | KR_DEBUG_CAP_WRITE |
                     KR_DEBUG_CAP_STEP | KR_DEBUG_CAP_OVER | KR_DEBUG_CAP_OUT | KR_DEBUG_CAP_STACK;
  out->clock_hz = cpu == KR_DEBUG_SUB68K ? SCD_CLOCK : system_clock;
  out->address_bits = cpu == KR_DEBUG_Z80 ? 16 : 24;
  out->flags_kind = cpu == KR_DEBUG_Z80 ? KR_DEBUG_FLAGS_Z80 : KR_DEBUG_FLAGS_M68K;
  if (kr_debug_is_sh2(cpu))
  {
    out->address_bits = 32;
    out->flags_kind = KR_DEBUG_FLAGS_SH2;
    name = cpu == KR_DEBUG_SH2_MASTER ? "SH-2 master" : "SH-2 slave";
  }
  else if (cpu == KR_DEBUG_SSP1601)
  {
    out->address_bits = 16;
    out->address_shift = 1;
    out->clock_hz = (uint32_t)(((uint64_t)system_clock * SVP_cycles) / MCYCLES_PER_LINE);
    out->flags_kind = KR_DEBUG_FLAGS_SSP1601;
    out->capabilities = KR_DEBUG_CAP_EXEC | KR_DEBUG_CAP_READ | KR_DEBUG_CAP_WRITE |
                       KR_DEBUG_CAP_STEP | KR_DEBUG_CAP_OVER | KR_DEBUG_CAP_OUT |
                       KR_DEBUG_CAP_STACK_ASCENDING;
    name = "SVP SSP1601";
  }
  else if (cpu == KR_DEBUG_SUB68K) name = "Sub 68000";
  else if (cpu == KR_DEBUG_Z80) name = "Z80";
  else name = "Main 68000";
  snprintf(out->name, sizeof(out->name), "%s", name);
  return 1;
}

RETRO_API size_t retro_kegarefusion_debug_targets_v2(unsigned version, kr_debug_target *targets,
                                                      size_t capacity)
{
  if (version != KR_DEBUG_VERSION || !targets)
    return 0;

  size_t count = 0;
  for (unsigned i = 0; i < KR_DEBUG_MAX_CPUS && count < capacity; ++i)
  {
    kr_debug_target target = {0};
    if (kr_debug_target_info(1u << i, &target))
      targets[count++] = target;
  }
  return count;
}

#ifdef KEGAREFUSION_32X
static void kr_debug_32x_callback(void *opaque, unsigned cpu, unsigned event,
                                   uint32_t address, uint32_t value, unsigned size)
{
  (void)opaque;
  kr_debug_notify(cpu, event, address, value, size);
}
#endif

RETRO_API bool retro_kegarefusion_debug_connect_v2(kr_debug_callback callback, void *opaque,
                                                  const unsigned *masks, size_t count)
{
  if (!callback)
  {
    kr_debug_detach();
    return true;
  }
  if (!kr_debug_cpu_set() || count > KR_DEBUG_MAX_CPUS || (count && !masks))
    return false;

  const unsigned events = KR_DEBUG_EXEC | KR_DEBUG_READ | KR_DEBUG_WRITE;
  for (size_t i = 0; i < count; ++i)
  {
    if (!masks[i]) continue;
    kr_debug_target target;
    if (!kr_debug_target_info(1u << i, &target) ||
        (masks[i] & ~(events & target.capabilities)))
      return false;
  }

  kr_debug_callback_user = opaque;
  kr_debug_callback_fn = callback;
  for (unsigned i = 0; i < KR_DEBUG_MAX_CPUS; ++i)
    kr_debug_event_masks[i] = (kr_debug_event_masks[i] & KR_DEBUG_COV_BITS) |
                             (i < count ? masks[i] : 0);
#ifdef KEGAREFUSION_32X
  if (kegarefusion_32x_attached)
    kr_debug_32x_connect(kr_debug_32x_callback, NULL, kr_debug_sh2_master_mask, kr_debug_sh2_slave_mask);
#endif
  return true;
}

RETRO_API bool retro_kegarefusion_debug_connect(kr_debug_callback callback, void *opaque,
                                               unsigned m68k_mask, unsigned z80_mask)
{
  const unsigned masks[] = { m68k_mask, z80_mask };
  return retro_kegarefusion_debug_connect_v2(callback, opaque, masks, sizeof(masks) / sizeof(*masks));
}

void kr_debug_notify(unsigned cpu, unsigned event, uint32_t address,
                     uint32_t value, unsigned size)
{
  if (kr_debug_callback_fn)
  {
    const unsigned previous_cpu = kr_debug_callback_cpu;
    const unsigned previous_event = kr_debug_callback_event;
    kr_debug_callback_cpu = cpu;
    kr_debug_callback_event = event;
    const uint32_t mask = cpu == KR_DEBUG_SSP1601 ? kr_debug_ssp_event_address_mask(event)
                                                 : kr_debug_address_mask(cpu);
    kr_debug_callback_fn(kr_debug_callback_user, cpu, event, address & mask, value, size);
    kr_debug_callback_cpu = previous_cpu;
    kr_debug_callback_event = previous_event;
  }
}

static void kr_debug_register(kr_debug_cpu *out, const char *name,
                              unsigned digits, unsigned value)
{
  kr_debug_reg *entry = &out->regs[out->count++];
  snprintf(entry->name, sizeof(entry->name), "%s", name);
  entry->value = value;
  entry->digits = digits;
}

static void kr_debug_state_m68k(kr_debug_cpu *out, const m68ki_cpu_core *core)
{
  static const char *const names[] = {
    "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
    "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"
  };

  out->pc = core->pc & kr_debug_address_mask(out->cpu);
  out->sp = core->dar[15] & kr_debug_address_mask(out->cpu);
  out->cycles = core->cycles;
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    kr_debug_register(out, names[i], 8, core->dar[i]);
  kr_debug_register(out, "PC", 6, out->pc);
  kr_debug_register(out, "SR", 4, core == &m68k ? kr_debug_m68k_sr() : kr_debug_sub68k_sr());
  kr_debug_register(out, "USP", 8, core->s_flag ? core->sp[0] : core->dar[15]);
  kr_debug_register(out, "SSP", 8, core->s_flag ? core->dar[15] : core->sp[4]);
  kr_debug_register(out, "IR", 4, core->ir);
  kr_debug_register(out, "STOP", 2, core->stopped & 7u);
  if (system_hw == SYSTEM_MCD)
    kr_debug_register(out, "POLL", 8, core->stopped & ~7u);
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
  if (system_hw == SYSTEM_MD || system_hw == SYSTEM_MCD)
    kr_debug_register(out, "BUS", 2, zstate);
}

RETRO_API bool retro_kegarefusion_debug_state(unsigned cpu, kr_debug_cpu *out, size_t capacity)
{
  if (!out || capacity < sizeof(*out) || !kr_debug_cpu_available(cpu))
    return false;

  memset(out, 0, sizeof(*out));
  out->cpu = cpu;
#ifdef KEGAREFUSION_32X
  if (kr_debug_is_sh2(cpu))
    return kr_debug_32x_state(cpu, out);
#endif
  if (cpu == KR_DEBUG_M68K)
    kr_debug_state_m68k(out, &m68k);
  else if (cpu == KR_DEBUG_SUB68K)
    kr_debug_state_m68k(out, &s68k);
  else if (cpu == KR_DEBUG_SSP1601)
    kr_debug_ssp_state(out);
  else
    kr_debug_state_z80(out);
  return true;
}


static unsigned kr_debug_m68k_register_id(const char *name)
{
  if ((name[0] == 'D' || name[0] == 'A') &&
      name[1] >= '0' && name[1] <= '7' && !name[2])
    return (name[0] == 'D' ? M68K_REG_D0 : M68K_REG_A0) + (name[1] - '0');
  if (!strcmp(name, "PC")) return M68K_REG_PC;
  if (!strcmp(name, "SP")) return M68K_REG_SP;
  if (!strcmp(name, "SR")) return M68K_REG_SR;
  if (!strcmp(name, "USP")) return M68K_REG_USP;
  if (!strcmp(name, "SSP")) return M68K_REG_ISP;
  return UINT32_MAX;
}

static PAIR *kr_debug_z80_pair(const char *name)
{
  static const struct {
    const char *name;
    PAIR *pair;
  } words[] = {
    {"PC", &Z80.pc}, {"SP", &Z80.sp},
    {"AF", &Z80.af}, {"BC", &Z80.bc}, {"DE", &Z80.de}, {"HL", &Z80.hl},
    {"IX", &Z80.ix}, {"IY", &Z80.iy},
    {"AF'", &Z80.af2}, {"BC'", &Z80.bc2}, {"DE'", &Z80.de2}, {"HL'", &Z80.hl2}
  };
  for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
    if (!strcmp(name, words[i].name))
      return words[i].pair;
  return NULL;
}

enum kr_debug_z80_byte_id {
  KR_DEBUG_Z80_I, KR_DEBUG_Z80_R, KR_DEBUG_Z80_IM, KR_DEBUG_Z80_IFF1, KR_DEBUG_Z80_IFF2,
  KR_DEBUG_Z80_UNKNOWN
};

static unsigned kr_debug_z80_byte(const char *name)
{
  static const char *const names[] = {"I", "R", "IM", "IFF1", "IFF2"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    if (!strcmp(name, names[i]))
      return i;
  return KR_DEBUG_Z80_UNKNOWN;
}

RETRO_API bool retro_kegarefusion_debug_register_writable_v2(unsigned cpu, const char *name)
{
  if (!name || !kr_debug_cpu_available(cpu))
    return false;
  switch (cpu)
  {
    case KR_DEBUG_M68K:
    case KR_DEBUG_SUB68K:
      return kr_debug_m68k_register_id(name) != UINT32_MAX;
    case KR_DEBUG_Z80:
      return kr_debug_z80_pair(name) || kr_debug_z80_byte(name) != KR_DEBUG_Z80_UNKNOWN;
    case KR_DEBUG_SSP1601:
      return kr_debug_ssp_writable(name);
#ifdef KEGAREFUSION_32X
    case KR_DEBUG_SH2_MASTER:
    case KR_DEBUG_SH2_SLAVE:
      return kr_debug_32x_writable(cpu, name);
#endif
    default:
      return false;
  }
}

static bool kr_debug_write_z80(const char *name, uint32_t value, int before_instruction)
{
  PAIR *pair = kr_debug_z80_pair(name);
  if (pair)
  {
    if (value > 0xffff)
      return false;
    if (pair == &Z80.pc || pair == &Z80.sp)
      pair->d = value;
    else
      pair->w.l = value;
    return true;
  }

  switch (kr_debug_z80_byte(name))
  {
    case KR_DEBUG_Z80_I:
      if (value > 0xff) return false;
      Z80.i = value;
      return true;
    case KR_DEBUG_Z80_R:
      if (value > 0xff) return false;
      Z80.r = value & 0x7f;
      Z80.r2 = value & 0x80;
      return true;
    case KR_DEBUG_Z80_IM:
      if (value > 2) return false;
      Z80.im = value;
      return true;
    case KR_DEBUG_Z80_IFF1:
      if (value > 1) return false;
      Z80.iff1 = value;
      kr_debug_z80_irq_recheck = before_instruction;
      return true;
    case KR_DEBUG_Z80_IFF2:
      if (value > 1) return false;
      Z80.iff2 = value;
      return true;
    default:
      return false;
  }
}

RETRO_API bool retro_kegarefusion_debug_write_register_v2(unsigned cpu, const char *name, uint32_t value)
{
  if (!name || !kr_debug_cpu_available(cpu) ||
      (kr_debug_callback_cpu &&
       (kr_debug_callback_cpu != cpu || kr_debug_callback_event != KR_DEBUG_EXEC)))
    return false;

  const int before_instruction = kr_debug_callback_cpu != 0;
#ifdef KEGAREFUSION_32X
  if (kr_debug_is_sh2(cpu))
    return kr_debug_32x_write(cpu, name, value);
#endif
  if (cpu == KR_DEBUG_Z80)
    return kr_debug_write_z80(name, value, before_instruction);
  if (cpu == KR_DEBUG_SSP1601)
    return kr_debug_ssp_write(name, value);

  if (cpu == KR_DEBUG_M68K || cpu == KR_DEBUG_SUB68K)
  {
    const unsigned reg = kr_debug_m68k_register_id(name);
    if (reg == UINT32_MAX ||
        (reg == M68K_REG_PC && ((value & 1) || value > kr_debug_address_mask(cpu))) ||
        (reg == M68K_REG_SR && value > 0xffff))
      return false;

    if (cpu == KR_DEBUG_M68K)
      kr_debug_write_m68k(reg, value, before_instruction);
    else
      kr_debug_write_sub68k(reg, value, before_instruction);
    return true;
  }

  return false;
}

static int kr_debug_peek_m68k(uint32_t address, uint8_t *byte)
{
  const cpu_memory_map *map = &m68k.memory_map[address >> 16];
#ifdef KEGAREFUSION_32X
  if (kegarefusion_32x_attached && kr_debug_32x_peek_md(KR_DEBUG_M68K, address, byte))
    return 1;
#endif
  if (system_hw == SYSTEM_MCD)
    return kr_debug_peek_cd(KR_DEBUG_M68K, address, byte);
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
#ifdef KEGAREFUSION_32X
  if (kegarefusion_32x_attached && kr_debug_32x_peek_md(KR_DEBUG_Z80, address, byte))
    return 1;
#endif
  if (system_hw == SYSTEM_MCD)
    return kr_debug_peek_cd(KR_DEBUG_Z80, address, byte);
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

  if (cpu == KR_DEBUG_SSP1601)
  {
    for (size_t i = 0; i < capacity; ++i)
      bytes[i] = kr_debug_ssp_peek((address << 1) + i);
    memset(validity, 1, capacity);
    return capacity;
  }

  const int md_bus = system_hw == SYSTEM_MD || system_hw == SYSTEM_MCD;
  int (*peek)(uint32_t, uint8_t *) = cpu == KR_DEBUG_M68K ? kr_debug_peek_m68k :
       md_bus ? kr_debug_peek_md_z80 : kr_debug_peek_sms;
  for (size_t i = 0; i < capacity; ++i)
  {
    bytes[i] = 0;
    const uint32_t offset = (address + i) & mask;
#ifdef KEGAREFUSION_32X
    if (kr_debug_is_sh2(cpu))
    {
      validity[i] = (uint8_t)kr_debug_32x_peek(cpu, offset, &bytes[i]);
      continue;
    }
#endif
    validity[i] = (uint8_t)(cpu == KR_DEBUG_SUB68K ? kr_debug_peek_cd(cpu, offset, &bytes[i]) :
                                                        peek(offset, &bytes[i]));
  }
  return capacity;
}

static unsigned kr_debug_code(unsigned cpu, uint32_t address, uint8_t *bytes, unsigned capacity)
{
  if (cpu == KR_DEBUG_SSP1601)
  {
    for (unsigned i = 0; i < capacity; ++i)
      bytes[i] = kr_debug_ssp_peek((address << 1) + i);
    return capacity;
  }

  const uint32_t mask = kr_debug_address_mask(cpu);
  unsigned count;
  for (count = 0; count < capacity; ++count)
  {
    const uint32_t offset = (address + count) & mask;
    if (cpu != KR_DEBUG_Z80)
    {
      const m68ki_cpu_core *core = cpu == KR_DEBUG_SUB68K ? &s68k : &m68k;
      const uint8_t *base = core->memory_map[offset >> 16].base;
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
#ifdef KEGAREFUSION_32X
  if (kr_debug_is_sh2(cpu))
    return kr_debug_32x_disassemble(cpu, address, out);
#endif
  const unsigned alignment = cpu == KR_DEBUG_Z80 ? 1 : 2;
  if (cpu != KR_DEBUG_SSP1601 && address % alignment)
    return true;
  const unsigned available = kr_debug_code(cpu, address, out->bytes, sizeof(out->bytes));
  if (available < alignment)
    return true;
  unsigned length;
  if (cpu == KR_DEBUG_SSP1601) length = kr_debug_disassemble_ssp(out, available);
  else if (cpu == KR_DEBUG_Z80) length = kr_debug_disassemble_z80(out, available);
  else length = kr_debug_disassemble_m68k(out, available);
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
