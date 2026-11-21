/* Included after lifecycle, inspection and debugger target definitions. */
#include "hooks.h"

unsigned kr_cov_exec_mask, kr_cov_read_mask, kr_cov_epoch;
kr_cov_prefetch_tag kr_cov_prefetch_tags[2];
static kr_cov_callback kr_cov_sink;
static void *kr_cov_user;

typedef struct kr_cov_storage {
  kr_cov_source info;
  const uint8_t *bytes;
} kr_cov_storage;

/* This table is rebuilt only when binding. Known backing buffers keep their
 * addresses while a console is loaded, including during bank and mode changes. */
static kr_cov_storage kr_cov_bound[KR_COV_MAX_SOURCES];
static unsigned kr_cov_bound_count;

static unsigned kr_cov_layout(kr_cov_storage *storage)
{
  const unsigned cpus = kr_debug_cpu_set();
  const unsigned main = cpus & KR_DEBUG_M68K;
  const unsigned z80 = cpus & KR_DEBUG_Z80;
  const unsigned sub = cpus & KR_DEBUG_SUB68K;
  const unsigned sh2 = cpus & (KR_DEBUG_SH2_MASTER | KR_DEBUG_SH2_SLAVE);
  const unsigned ssp = cpus & KR_DEBUG_SSP1601;
  const unsigned md = (system_hw == SYSTEM_MD || system_hw == SYSTEM_MCD);
  unsigned count = 0;
  if (!cpus) return 0;

#define SOURCE(id, name, ptr, size, exec, read, flags, domain) do { \
    const unsigned storage_size = (size); \
    if (storage_size && ((exec) | (read))) { \
      kr_cov_storage *entry = &storage[count++]; \
      entry->info = (kr_cov_source){ id, storage_size, exec, read, flags, domain, 0, name }; \
      entry->bytes = (const uint8_t *)(ptr); \
    } \
  } while (0)
  const unsigned cart_size = romtype != SYSTEM_MCD && cart.romsize <= sizeof(cart.rom) ?
                             cart.romsize : 0;
  SOURCE(KR_COV_CART_ROM, "Cartridge ROM", cart.rom, cart_size,
         main | sh2 | (md ? 0 : z80), main | z80 | sh2,
         KR_COV_SOURCE_ROM | KR_COV_SOURCE_PARTIAL, KR_MEM_ROM);
  if (md)
  {
    SOURCE(KR_COV_MAIN_RAM, "Main RAM", work_ram, sizeof(work_ram), main, main | z80,
           KR_COV_SOURCE_RAM, KR_MEM_RAM);
    SOURCE(KR_COV_Z80_RAM, "Z80 RAM", zram, sizeof(zram), z80, z80,
           KR_COV_SOURCE_RAM | KR_COV_SOURCE_PARTIAL, KR_MEM_ZRAM);
    if (system_bios & SYSTEM_MD)
      SOURCE(KR_COV_MD_BOOT, "Mega Drive boot ROM", boot_rom, sizeof(boot_rom), main, main | z80,
             KR_COV_SOURCE_ROM, 0);
  }
  else
  {
    const unsigned ram_size = system_hw == SYSTEM_SG ? 0x400u :
        system_hw == SYSTEM_SGII ? 0x800u :
        system_hw == SYSTEM_SGII_RAM_EXT ? 0x4000u : 0x2000u;
    SOURCE(KR_COV_Z80_RAM, "Z80 RAM", work_ram, ram_size, z80, z80,
           KR_COV_SOURCE_RAM, ram_size <= 0x2000u ? KR_MEM_ZRAM : 0);
    SOURCE(KR_COV_SMS_BOOT, "8-bit boot ROM", cart.rom + 0x400000, kr_cov_sms_bios_size(), z80, z80,
           KR_COV_SOURCE_ROM, 0);
    const uint8_t *flash = sms_cart_irata_flash();
    if (flash)
      SOURCE(KR_COV_SMS_FLASH, "Cartridge flash", flash, SMS_IRATA_FLASH_SIZE, z80, z80,
             KR_COV_SOURCE_RAM | KR_COV_SOURCE_PARTIAL, KR_MEM_SAVE);
  }
  if (sram.on)
    SOURCE(KR_COV_SAVE_RAM, "Cartridge save RAM", sram.sram, sizeof(sram.sram), main | (md ? 0 : z80),
           main | z80, KR_COV_SOURCE_RAM | KR_COV_SOURCE_PARTIAL,
           md || !sms_cart_irata_flash() ? KR_MEM_SAVE : 0);
  if (sub)
  {
    SOURCE(KR_COV_CD_BOOT, "CD boot ROM", scd.bootrom, sizeof(scd.bootrom), main, main | z80,
           KR_COV_SOURCE_ROM, romtype == SYSTEM_MCD ? KR_MEM_ROM : 0);
    SOURCE(KR_COV_CD_PRG, "CD Program RAM", scd.prg_ram, sizeof(scd.prg_ram), main | sub, main | sub | z80,
           KR_COV_SOURCE_RAM, KR_MEM_CD_PRG);
    SOURCE(KR_COV_CD_WORD, "CD Word RAM", scd.word_ram_2M, sizeof(scd.word_ram_2M),
           main | sub, main | sub | z80, KR_COV_SOURCE_RAM,
           scd.regs[1].w & 4 ? 0 : KR_MEM_CD_WORD);
    SOURCE(KR_COV_CD_BRAM, "CD Backup RAM", scd.bram, sizeof(scd.bram), 0, sub,
           KR_COV_SOURCE_RAM, KR_MEM_CD_BRAM);
  }
  if (ssp)
  {
    SOURCE(KR_COV_SVP_IRAM, "SVP IRAM", svp->iram_rom, 0x800, ssp, 0,
           KR_COV_SOURCE_RAM, KR_MEM_SVP_IRAM);
    SOURCE(KR_COV_SVP_ROM, "SVP program ROM", svp->iram_rom + 0x800, sizeof(svp->iram_rom) - 0x800,
           ssp, 0, KR_COV_SOURCE_ROM, 0);
    SOURCE(KR_COV_SVP_DRAM, "SVP DRAM", svp->dram, sizeof(svp->dram), main, main | z80,
           KR_COV_SOURCE_RAM | KR_COV_SOURCE_PARTIAL, KR_MEM_SVP_DRAM);
  }

#ifdef KEGAREFUSION_32X
  if (sh2)
  {
    unsigned n = 0;
    const uint8_t *ptr;
#define X32_SOURCE(id, name, exec, read, flags, domain) do { \
      ptr = kr_cov_32x_storage(id, &n); \
      if (ptr) SOURCE(id, name, ptr, n, exec, read, flags, domain); \
    } while (0)
    X32_SOURCE(KR_COV_32X_BOOT, "32X 68000 boot window", main, main | z80, KR_COV_SOURCE_ROM | KR_COV_SOURCE_PARTIAL, 0);
    X32_SOURCE(KR_COV_SH2_MASTER_BOOT, "SH-2 master boot ROM", KR_DEBUG_SH2_MASTER, KR_DEBUG_SH2_MASTER,
               KR_COV_SOURCE_ROM, 0);
    X32_SOURCE(KR_COV_SH2_SLAVE_BOOT, "SH-2 slave boot ROM", KR_DEBUG_SH2_SLAVE, KR_DEBUG_SH2_SLAVE,
               KR_COV_SOURCE_ROM, 0);
    X32_SOURCE(KR_COV_32X_SDRAM, "32X SDRAM", sh2, sh2, KR_COV_SOURCE_RAM, KR_MEM_32X_SDRAM);
    X32_SOURCE(KR_COV_SH2_MASTER_DATA, "SH-2 master data array", KR_DEBUG_SH2_MASTER, KR_DEBUG_SH2_MASTER,
               KR_COV_SOURCE_RAM, 0);
    X32_SOURCE(KR_COV_SH2_SLAVE_DATA, "SH-2 slave data array", KR_DEBUG_SH2_SLAVE, KR_DEBUG_SH2_SLAVE,
               KR_COV_SOURCE_RAM, 0);
    X32_SOURCE(KR_COV_32X_FB0, "32X framebuffer 0", main | sh2, main | z80 | sh2,
               KR_COV_SOURCE_RAM, KR_MEM_32X_FB0);
    X32_SOURCE(KR_COV_32X_FB1, "32X framebuffer 1", main | sh2, main | z80 | sh2,
               KR_COV_SOURCE_RAM, KR_MEM_32X_FB1);
#undef X32_SOURCE
  }
#endif
#undef SOURCE
  return count;
}

RETRO_API size_t retro_kegarefusion_coverage_sources_v1(unsigned version, kr_cov_source *out,
                                                        size_t capacity)
{
  if (version != KR_COV_VERSION || !out || !capacity) return 0;
  kr_cov_storage sources[KR_COV_MAX_SOURCES];
  const unsigned count = kr_cov_layout(sources);
  const size_t used = capacity < count ? capacity : count;
  for (size_t i = 0; i < used; ++i) out[i] = sources[i].info;
  return used;
}

static void kr_cov_update_cpu_masks(unsigned exec_cpus, unsigned read_cpus)
{
  /* The native SH-2 and SSP engines retain their own observer flags. */
  for (unsigned i = 0; i < 3; ++i)
  {
    const unsigned cpu = 1u << i;
    const unsigned flags = (exec_cpus & cpu ? KR_DEBUG_COV_EXEC : 0) |
                          (read_cpus & cpu ? KR_DEBUG_COV_READ : 0);
    kr_debug_event_masks[i] = (kr_debug_event_masks[i] & ~KR_DEBUG_COV_BITS) | flags;
  }
}

static void kr_cov_detach(void)
{
  kr_cov_exec_mask = kr_cov_read_mask = 0;
  kr_cov_update_cpu_masks(0, 0);
  ++kr_cov_epoch;
#ifdef KEGAREFUSION_32X
  kr_cov_32x_bind(0, 0);
#endif
  kr_cov_bound_count = 0;
  kr_cov_sink = NULL;
  kr_cov_user = NULL;
}

RETRO_API bool retro_kegarefusion_coverage_bind_v1(unsigned version, uint32_t exec_cpus,
                                                   uint32_t read_cpus, kr_cov_callback sink,
                                                   void *user)
{
  if (version != KR_COV_VERSION) return false;
  if (!sink)
  {
    kr_cov_detach();
    return true;
  }
  const unsigned targets = kr_debug_cpu_set();
  if (!(exec_cpus | read_cpus) || ((exec_cpus | read_cpus) & ~targets) ||
      (read_cpus & KR_DEBUG_SSP1601)) return false;

  /* Rebinding the same CPU selection (e.g. a host refresh) need not lose
   * the provenance of an already fetched word. */
  if (exec_cpus != kr_cov_exec_mask) ++kr_cov_epoch;
  kr_cov_bound_count = kr_cov_layout(kr_cov_bound);
  kr_cov_exec_mask = exec_cpus;
  kr_cov_read_mask = read_cpus;
  kr_cov_update_cpu_masks(exec_cpus, read_cpus);
  kr_cov_user = user;
  kr_cov_sink = sink;
#ifdef KEGAREFUSION_32X
  kr_cov_32x_bind(exec_cpus, read_cpus);
#endif
  return true;
}

static bool kr_cov_in_storage(const void *ptr, const void *storage, unsigned size, uint32_t *off)
{
  const uintptr_t loc = (uintptr_t)ptr, base = (uintptr_t)storage;
  if (loc < base || loc - base >= size) return false;
  *off = (uint32_t)(loc - base);
  return true;
}

kr_cov_location kr_cov_locate(uint32_t address, const uint8_t *bytes)
{
  if (!bytes) return (kr_cov_location){0, address};
  uint32_t offset;
  if (kr_firecore_active && kr_cov_firecore_location(address, bytes, &offset))
    return (kr_cov_location){KR_COV_CART_ROM, offset};
  if (system_hw == SYSTEM_MCD)
  {
    /* 1M banks are the alternating 16-bit words of the physical 2M view. */
    for (unsigned bank = 0; bank < 2; ++bank)
      if (kr_cov_in_storage(bytes, scd.word_ram[bank], sizeof(scd.word_ram[bank]), &offset))
        return (kr_cov_location){KR_COV_CD_WORD, ((offset & ~1u) << 1) | (bank << 1) | (offset & 1u)};
  }
  /* Secondary backing storage (notably SVP inside cart.rom) takes priority. */
  for (unsigned i = kr_cov_bound_count; i; --i)
  {
    const kr_cov_storage *entry = &kr_cov_bound[i - 1];
    if (kr_cov_in_storage(bytes, entry->bytes, entry->info.size, &offset))
      return (kr_cov_location){entry->info.id, offset};
  }
  return (kr_cov_location){0, address};
}

void kr_cov_emit(unsigned cpu, unsigned kind, kr_cov_location location, unsigned width)
{
  if (!kr_cov_sink || !width) return;
  const kr_cov_event event = {cpu, kind, location.source, location.offset, width};
  kr_cov_sink(kr_cov_user, &event);
}

void kr_cov_observe(unsigned cpu, unsigned kind, uint32_t address, const uint8_t *bytes, unsigned width)
{
  if (!width) return;
  kr_cov_location first = kr_cov_locate(address, bytes);
  unsigned run = 1;
  for (unsigned i = 1; i < width; ++i)
  {
    const kr_cov_location next = kr_cov_locate(address + i, bytes ? bytes + i : NULL);
    if (next.source == first.source && next.offset == first.offset + run)
    {
      ++run;
      continue;
    }
    kr_cov_emit(cpu, kind, first, run);
    first = next;
    run = 1;
  }
  kr_cov_emit(cpu, kind, first, run);
}

kr_cov_access kr_cov_68k_data(unsigned cpu, uint32_t address, unsigned width)
{
  address &= 0xffffffu;
  const cpu_memory_map *map = (cpu == KR_DEBUG_SUB68K ? s68k.memory_map : m68k.memory_map) +
                              (address >> 16);
  unsigned (*read)(unsigned) = width == 1 ? map->read8 : map->read16;
  if (!read)
    return (kr_cov_access){map->base ? map->base + (address & 0xffff) : NULL, width};
  if (read == sram_read_byte || read == sram_read_word)
    return (kr_cov_access){sram.sram + (address & (width == 1 ? 0xffff : 0xfffe)), width};
#ifdef KEGAREFUSION_32X
  if (cpu == KR_DEBUG_M68K && kegarefusion_32x_attached)
  {
    const kr_cov_access resolved = kr_cov_32x_md_data(cpu, address, width);
    if (resolved.bytes || !resolved.width) return resolved;
  }
#endif
  return system_hw == SYSTEM_MCD ? kr_cov_cd_data(cpu, address, width) :
                                 (kr_cov_access){NULL, width};
}

kr_cov_access kr_cov_z80_data(uint32_t address)
{
  if (system_hw != SYSTEM_MD && system_hw != SYSTEM_MCD)
    return kr_cov_sms_data(address);
  if (address < 0x4000)
    return (kr_cov_access){zram + (address & 0x1fff), 1};
  if (address < 0x8000)
    return (kr_cov_access){NULL, 1};

  address = (zbank | (address & 0x7fff)) & 0xffffff;
  unsigned page = address >> 16;
  unsigned (*read)(unsigned) = zbank_memory_map[page].read;
  if (!read && m68k.memory_map[page].base)
    return (kr_cov_access){m68k.memory_map[page].base + (address & 0xffff), 1};
  if (read == sram_read_byte)
    return (kr_cov_access){sram.sram + (address & 0xffff), 1};
#ifdef KEGAREFUSION_32X
  if (kegarefusion_32x_attached)
  {
    const kr_cov_access resolved = kr_cov_32x_md_data(KR_DEBUG_Z80, address, 1);
    if (resolved.bytes || !resolved.width) return resolved;
  }
#endif
  return system_hw == SYSTEM_MCD ? kr_cov_cd_data(KR_DEBUG_Z80, address, 1) :
                                 (kr_cov_access){NULL, 1};
}
