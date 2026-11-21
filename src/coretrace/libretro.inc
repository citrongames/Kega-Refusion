/* Included after the native debugger and its pure opcode-map helpers. */
#include "hooks.h"

unsigned kr_trace_cpu_mask, kr_trace_filters;
static kr_trace_callback kr_trace_sink;
static void *kr_trace_user;

#ifdef KEGAREFUSION_32X
void kr_trace_32x_bind(unsigned instructions, unsigned irq);
#endif

static void kr_trace_detach(void)
{
  kr_trace_cpu_mask = kr_trace_filters = 0;
#ifdef KEGAREFUSION_32X
  kr_trace_32x_bind(0, 0);
#endif
  kr_trace_sink = NULL;
  kr_trace_user = NULL;
}

RETRO_API uint32_t retro_kegarefusion_trace_capabilities_v1(void)
{
  if (!kr_inspect_content) return 0;

  unsigned filters = KR_TRACE_VIDEO;
  if (kr_debug_cpu_set()) filters |= KR_TRACE_INSTRUCTION | KR_TRACE_IRQ;
  if (system_hw == SYSTEM_MD || system_hw == SYSTEM_MCD) filters |= KR_TRACE_DMA;
  if (system_hw == SYSTEM_MCD) filters |= KR_TRACE_DISC;
  if (io_link_yield || gg_link_yield) filters |= KR_TRACE_LINK;
  if (kr_firecore_active) filters |= KR_TRACE_MAPPER;
  return filters;
}

RETRO_API bool retro_kegarefusion_trace_bind_v1(unsigned version, uint32_t cpu_mask,
                                                uint32_t filters, kr_trace_callback sink,
                                                void *user)
{
  if (version != KR_TRACE_VERSION) return false;
  if (!sink)
  {
    kr_trace_detach();
    return true;
  }
  const unsigned supported = retro_kegarefusion_trace_capabilities_v1();
  if (!filters || (filters & ~supported) ||
      ((filters & KR_TRACE_INSTRUCTION) && (!cpu_mask || (cpu_mask & ~kr_debug_cpu_set()))))
    return false;

  kr_trace_user = user;
  kr_trace_sink = sink;
  kr_trace_cpu_mask = filters & KR_TRACE_INSTRUCTION ? cpu_mask : 0;
  kr_trace_filters = filters;
#ifdef KEGAREFUSION_32X
  kr_trace_32x_bind(kr_trace_cpu_mask, filters & KR_TRACE_IRQ);
#endif
  return true;
}

static void kr_trace_time(kr_trace_record *record, unsigned cpu, int64_t local_ticks)
{
  record->local_ticks = record->mclk = local_ticks;
  if (cpu == KR_DEBUG_SUB68K && scd.cycles_per_line)
    record->mclk = local_ticks * MCYCLES_PER_LINE / scd.cycles_per_line;
  else if (cpu == KR_DEBUG_SSP1601 && SVP_cycles > 0)
  {
    record->mclk = local_ticks * MCYCLES_PER_LINE / SVP_cycles;
    record->flags |= KR_TRACE_TIME_QUANTIZED;
  }
}

static void kr_trace_opcode(kr_trace_record *record, unsigned width)
{
  record->valid_bytes = record->width = (uint8_t)width;
  if (width == 2)
    record->bytes[0] = (uint8_t)(record->value >> 8);
  record->bytes[width - 1] = (uint8_t)record->value;
}

void kr_trace_exec(unsigned cpu, uint32_t pc, uint32_t opcode, unsigned width,
                   int64_t local_ticks)
{
  kr_trace_record record = {0};
  record.cpu = cpu;
  record.kind = KR_TRACE_EXEC;
  record.address = pc & kr_debug_address_mask(cpu);
  record.value = opcode;
  kr_trace_time(&record, cpu, local_ticks);

  const unsigned available = kr_debug_code(cpu, record.address, record.bytes, sizeof(record.bytes));
  kr_trace_opcode(&record, width);
  if (available > width)
  {
    record.flags |= KR_TRACE_PEEK_TAIL;
    record.valid_bytes = (uint8_t)available;
  }
  if (available < sizeof(record.bytes)) record.flags |= KR_TRACE_BYTES_TRUNCATED;
  if (kr_trace_sink) kr_trace_sink(kr_trace_user, &record);
}

void kr_trace_hardware(unsigned kind, unsigned device, unsigned cpu, int64_t local_ticks,
                       uint32_t address, uint32_t value, uint32_t aux, unsigned width)
{
  if (!kr_trace_sink) return;
  kr_trace_record record = {0};
  record.cpu = cpu;
  record.kind = kind;
  record.device = device;
  record.address = address;
  record.value = value;
  record.aux = aux;
  record.width = (uint8_t)width;
  kr_trace_time(&record, cpu, local_ticks);
  kr_trace_sink(kr_trace_user, &record);
}

void kr_trace_sh2(unsigned cpu, unsigned kind, int64_t mclk, int64_t local_ticks,
                  uint32_t address, uint32_t value, uint32_t aux)
{
  if (!kr_trace_sink) return;
  kr_trace_record record = {0};
  record.cpu = cpu;
  record.kind = kind;
  record.device = kind == KR_TRACE_EXEC ? KR_TRACE_CPU : KR_TRACE_32X;
  record.mclk = mclk;
  record.local_ticks = local_ticks;
  record.address = address;
  record.value = value;
  record.aux = aux;
  if (kind == KR_TRACE_EXEC) kr_trace_opcode(&record, 2);
  kr_trace_sink(kr_trace_user, &record);
}

RETRO_API bool retro_kegarefusion_trace_decode_v1(unsigned cpu, uint32_t pc,
                                                  const uint8_t *bytes, size_t available,
                                                  kr_debug_instruction *out, size_t capacity)
{
  if (!out || capacity < sizeof(*out) || !bytes || !available ||
      pc > kr_debug_address_mask(cpu) ||
      (cpu != KR_DEBUG_Z80 && cpu != KR_DEBUG_SSP1601 && (pc & 1u)))
    return false;
  memset(out, 0, sizeof(*out));
  out->address = pc;
  if (available > sizeof(out->bytes)) available = sizeof(out->bytes);
  memcpy(out->bytes, bytes, available);

  unsigned length;
  switch (cpu)
  {
    case KR_DEBUG_M68K:
    case KR_DEBUG_SUB68K:
      length = kr_debug_disassemble_m68k(out, (unsigned)available);
      break;
    case KR_DEBUG_Z80:
      length = kr_debug_disassemble_z80(out, (unsigned)available);
      break;
    case KR_DEBUG_SH2_MASTER:
    case KR_DEBUG_SH2_SLAVE:
      length = kr_debug_disassemble_sh2(out, (unsigned)available);
      break;
    case KR_DEBUG_SSP1601:
      length = kr_debug_disassemble_ssp(out, (unsigned)available);
      break;
    default:
      return false;
  }
  if (!length || length > available)
  {
    out->flags = 0;
    out->length = 0;
    out->text[0] = '\0';
    return false;
  }
  out->length = length;
  out->valid = 1;
  memset(out->bytes + length, 0, sizeof(out->bytes) - length);
  return true;
}
