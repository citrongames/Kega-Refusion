/* The caller owns the core on the emulation thread. These reads access
 * physical storage and latches without invoking any mapped bus handler. */
#include "values.h"
#ifdef KEGAREFUSION_32X
#include "../core/kega32x/bus.h"
#include "../core/kega32x/audio/pwm.h"
#endif

enum kr_memory_order { KR_BYTES, KR_WORDS_BE, KR_SMS_COLORS, KR_WORDS_LE };

typedef struct
{
  const uint8 *data;
  size_t size;
  uint32_t base;
  const char *name;
  enum kr_memory_order order;
} kr_memory_view;

static int kr_inspect_content;
static int kr_inspect_md(void)
{
  return (system_hw & SYSTEM_PBC) == SYSTEM_MD;
}

static int kr_inspect_sg(void)
{
  return system_hw == SYSTEM_SG || system_hw == SYSTEM_SGII ||
         system_hw == SYSTEM_SGII_RAM_EXT;
}

static int kr_inspect_mode5(void)
{
  return kr_inspect_md() && ((reg[1] & 4) || KR_FIRECORE_VDP_ENABLED);
}

static unsigned kr_inspect_vdp_register_count(void)
{
  if (kr_inspect_md()) return kr_firecore_active ? 32u : 24u;
  return kr_inspect_sg() ? 8u : 11u;
}

static unsigned kr_inspect_video_format(void)
{
  if (KR_FIRECORE_VDP_ENABLED && (reg[0x18] & 0x40))
    return KR_VIDEO_FIRECORE_BITMAP;
  if (kr_inspect_mode5())
    return KR_VIDEO_MD;
  if (kr_inspect_sg())
    return KR_VIDEO_SG;
  if ((reg[0] & 4) || system_hw == SYSTEM_GGMS)
    return KR_VIDEO_SMS;
  return kr_inspect_md() ? KR_VIDEO_NONE : KR_VIDEO_SG;
}

static kr_memory_view kr_inspect_domain_view(unsigned id)
{
  const int md = kr_inspect_md();
  switch (id)
  {
    case KR_MEM_ROM:
      /* A disc image's header may occupy the cart buffer. Expose its actual
       * boot ROM, while a cartridge with the CD add-on still has cart ROM. */
      if (romtype == SYSTEM_MCD)
        return (kr_memory_view){ scd.bootrom, sizeof(scd.bootrom),
            (uint32_t)scd.cartridge.boot << 16, "CD boot ROM", KR_WORDS_BE };
      if (cart.romsize <= sizeof(cart.rom))
        return (kr_memory_view){ cart.rom, cart.romsize, 0, "Cartridge ROM",
            ((romtype & SYSTEM_MD) && romtype != SYSTEM_PBC) ? KR_WORDS_BE : KR_BYTES };
      break;
    case KR_MEM_RAM:
      if (md)
        return (kr_memory_view){ work_ram, sizeof(work_ram), 0xff0000, "Main RAM", KR_WORDS_BE };
      break;
    case KR_MEM_ZRAM:
      if (system_hw == SYSTEM_PICO) break;
      if (md)
        return (kr_memory_view){ zram, sizeof(zram), 0, "Z80 RAM", KR_BYTES };
      return (kr_memory_view){ work_ram, system_hw == SYSTEM_SG ? 0x400u :
          system_hw == SYSTEM_SGII ? 0x800u : 0x2000u, 0xc000, "Z80 RAM", KR_BYTES };
    case KR_MEM_VRAM:
      if (kr_inspect_mode5())
        return (kr_memory_view){ vram, kr_firecore_vram ? 0x20000u : 0x10000u, 0, "VRAM", KR_WORDS_BE };
      return (kr_memory_view){ vram, 0x4000, 0, "VRAM", KR_BYTES };
    case KR_MEM_CRAM:
      if (KR_FIRECORE_VDP_ENABLED)
        return (kr_memory_view){ kr_firecore_cram, 0x800, 0, "Firecore CRAM", KR_WORDS_BE };
      if (kr_inspect_mode5())
        return (kr_memory_view){ cram, sizeof(cram), 0, "CRAM (9-bit words)", KR_WORDS_BE };
      if (system_hw == SYSTEM_GG)
        return (kr_memory_view){ cram, 0x40, 0, "CRAM", KR_WORDS_LE };
      if (!kr_inspect_sg())
        return (kr_memory_view){ cram, 0x20, 0, "CRAM", KR_SMS_COLORS };
      break;
    case KR_MEM_VSRAM:
      if (kr_inspect_mode5())
        return (kr_memory_view){ vsram, 0x50, 0, "VSRAM", KR_WORDS_BE };
      break;
    case KR_MEM_VDP_REGS:
      return (kr_memory_view){ reg, kr_inspect_vdp_register_count(), 0, "VDP registers", KR_BYTES };
    case KR_MEM_SAVE:
    {
      const uint8 *flash = md ? NULL : sms_cart_irata_flash();
      if (flash)
        return (kr_memory_view){ flash, SMS_IRATA_FLASH_SIZE, 0, "Cartridge flash", KR_BYTES };
      if (sram.on)
        return (kr_memory_view){ sram.sram, sizeof(sram.sram), 0, "Cartridge save storage", KR_BYTES };
      break;
    }
    case KR_MEM_CD_PRG:
      if (system_hw == SYSTEM_MCD)
        return (kr_memory_view){ scd.prg_ram, sizeof(scd.prg_ram), 0, "CD Program RAM", KR_WORDS_BE };
      break;
    case KR_MEM_CD_WORD:
      if (system_hw == SYSTEM_MCD)
      {
        if (scd.regs[1].w & 4)
          return (kr_memory_view){ (const uint8 *)&scd.word_ram, sizeof(scd.word_ram), 0,
              "CD Word RAM (1M banks 0 and 1)", KR_WORDS_BE };
        return (kr_memory_view){ scd.word_ram_2M, sizeof(scd.word_ram_2M), 0, "CD Word RAM (2M)", KR_WORDS_BE };
      }
      break;
    case KR_MEM_CD_PCM:
      if (system_hw == SYSTEM_MCD)
        return (kr_memory_view){ scd.pcm_hw.ram, sizeof(scd.pcm_hw.ram), 0, "CD PCM Wave RAM", KR_BYTES };
      break;
    case KR_MEM_CD_BRAM:
      if (system_hw == SYSTEM_MCD)
        return (kr_memory_view){ scd.bram, sizeof(scd.bram), 0, "CD Backup RAM", KR_BYTES };
      break;
#ifdef KEGAREFUSION_32X
    case KR_MEM_32X_SDRAM:
    case KR_MEM_32X_FB0:
    case KR_MEM_32X_FB1:
    case KR_MEM_32X_CRAM:
    {
      const Kega32xBus *bus = kegarefusion_32x_inspect_bus();
      if (!bus || !bus->memory) break;
      if (id == KR_MEM_32X_SDRAM)
        return (kr_memory_view){ (const uint8 *)bus->memory->sdram,
            sizeof(bus->memory->sdram), 0x06000000, "32X SDRAM", KR_WORDS_BE };
      if (id == KR_MEM_32X_CRAM)
        return (kr_memory_view){ (const uint8 *)bus->s.palette, sizeof(bus->s.palette),
            0, "32X CRAM", KR_WORDS_BE };
      return (kr_memory_view){ (const uint8 *)bus->memory->framebuffer[id - KR_MEM_32X_FB0],
          sizeof(bus->memory->framebuffer[0]), 0,
          id == KR_MEM_32X_FB0 ? "32X framebuffer 0" : "32X framebuffer 1", KR_WORDS_BE };
    }
#endif
    case KR_MEM_SVP_IRAM:
      if (md && svp)
        return (kr_memory_view){ svp->iram_rom, 0x800, 0, "SVP IRAM", KR_WORDS_BE };
      break;
    case KR_MEM_SVP_DRAM:
      if (md && svp)
        return (kr_memory_view){ svp->dram, sizeof(svp->dram), 0x300000, "SVP DRAM", KR_WORDS_BE };
      break;
  }
  return (kr_memory_view){0};
}

static void kr_inspect_machine(kr_inspect_info *info)
{
  const char *name;
  switch (system_hw)
  {
    case SYSTEM_MCD:
      info->hardware = KR_HW_MD | KR_HW_CD;
      if (kr_megald_active)
      {
        info->hardware |= KR_HW_MEGALD;
        name = "MegaLD";
      }
      else name = "Sega CD";
      break;
    case SYSTEM_PICO:
      info->hardware = KR_HW_PICO;
      name = "Pico";
      break;
    case SYSTEM_PBC:
      info->hardware = KR_HW_MD | KR_HW_SMS;
      name = "Power Base Converter";
      break;
    case SYSTEM_GG:
      info->hardware = KR_HW_GG;
      name = "Game Gear";
      break;
    case SYSTEM_GGMS:
      info->hardware = KR_HW_GG | KR_HW_SMS;
      name = "Game Gear (SMS mode)";
      break;
    case SYSTEM_SG:
      info->hardware = KR_HW_SG;
      name = "SG-1000";
      break;
    case SYSTEM_SGII:
    case SYSTEM_SGII_RAM_EXT:
      info->hardware = KR_HW_SG;
      name = "SG-1000 II";
      break;
    case SYSTEM_MARKIII:
      info->hardware = KR_HW_SMS;
      name = "Mark III";
      break;
    case SYSTEM_SMS:
    case SYSTEM_SMS2:
      info->hardware = KR_HW_SMS;
      name = "Master System";
      break;
    default:
      info->hardware = KR_HW_MD;
      name = "Mega Drive";
      break;
  }
  if (kr_firecore_active)
  {
    info->hardware |= KR_HW_FIRECORE;
    name = "Firecore";
  }
#ifdef KEGAREFUSION_32X
  if (kegarefusion_32x_inspect_bus())
  {
    info->hardware |= KR_HW_32X;
    name = system_hw == SYSTEM_MCD ? "Sega CD + 32X" : "32X";
  }
#endif
  if (kr_inspect_md() && svp)
    info->hardware |= KR_HW_SVP;

  snprintf(info->system, sizeof(info->system), "%s", name);
  info->video_format = kr_inspect_video_format();
  info->width = bitmap.viewport.w;
  info->height = bitmap.viewport.h;
  memcpy(info->vdp_regs, reg, kr_inspect_vdp_register_count());
}

static void kr_inspect_domains(kr_inspect_info *info, unsigned parts)
{
  for (unsigned id = KR_MEM_ROM; id <= KR_MEM_SVP_DRAM; ++id)
  {
    if (!(parts & KR_INSPECT_MEMORY))
    {
      const int video = (id >= KR_MEM_VRAM && id <= KR_MEM_VDP_REGS) ||
                        (id >= KR_MEM_32X_FB0 && id <= KR_MEM_32X_CRAM);
      if (!(parts & KR_INSPECT_VIDEO) || !video)
        continue;
    }
    const kr_memory_view view = kr_inspect_domain_view(id);
    if (!view.data || !view.size || info->domain_count >= KR_INSPECT_MAX_DOMAINS)
      continue;
    kr_inspect_domain *domain = &info->domains[info->domain_count++];
    domain->id = id;
    domain->size = (uint32_t)view.size;
    domain->address_base = view.base;
    domain->big_endian = view.order == KR_WORDS_BE;
    snprintf(domain->name, sizeof(domain->name), "%s", view.name);
  }
}


static void kr_inspect_cd(kr_inspect_info *info, unsigned parts)
{
  if (parts & KR_INSPECT_AUDIO)
  {
    const unsigned part = KR_INSPECT_AUDIO;
    const pcm_t *pcm = &scd.pcm_hw;
    kr_inspect_put(info, part, "CDDA", "Drive status", 2, cdd.status);
    if (cdd.loaded)
    {
      kr_inspect_put(info, part, "CDDA", "LBA (signed)", 8, (uint32)cdd.lba);
      kr_inspect_put(info, part, "CDDA", "Fader current", 4, cdd.fader[0]);
      kr_inspect_put(info, part, "CDDA", "Fader target", 4, cdd.fader[1]);
      if (cdd.index >= 0 && cdd.index < cdd.toc.last)
      {
        kr_inspect_put(info, part, "CDDA", "Disc track (one-based)", 0, cdd.index + 1);
        kr_inspect_put(info, part, "CDDA", "Audio track", 0, !cdd.toc.tracks[cdd.index].type);
      }
    }
    kr_inspect_put(info, part, "RF5C164 PCM", "Enabled", 0, !!pcm->enabled);
    kr_inspect_put(info, part, "RF5C164 PCM", "Active channel mask", 2, pcm->status);
    kr_inspect_put(info, part, "RF5C164 PCM", "Channel select", 0, pcm->index + 1);
    for (unsigned i = 0; i < 8; ++i)
    {
      char device[24];
      snprintf(device, sizeof(device), "RF5C164 PCM ch %u", i + 1);
      kr_inspect_put(info, part, device, "Sample address", 4, (pcm->chan[i].addr >> 11) & 0xffff);
      kr_inspect_put(info, part, device, "Loop address", 4, pcm->chan[i].ls.w);
      kr_inspect_put(info, part, device, "Envelope / pan", 4, (pcm->chan[i].env << 8) | pcm->chan[i].pan);
    }
  }

  if (parts & KR_INSPECT_HARDWARE)
  {
    const unsigned part = KR_INSPECT_HARDWARE;
    kr_inspect_put(info, part, "Sega CD", "Memory mode register", 4, scd.regs[1].w);
    kr_inspect_put(info, part, "Sega CD", "Word RAM mode (Mbit)", 0, (scd.regs[1].w & 4) ? 1 : 2);
    kr_inspect_put(info, part, "Sega CD", "PRG RAM bank", 0, (scd.regs[1].w >> 6) & 3);
    kr_inspect_put(info, part, "Sega CD", "DMNA request", 0, !!scd.dmna);
    kr_inspect_put(info, part, "Sega CD", "Pending interrupts", 2, scd.pending);
    kr_inspect_put(info, part, "Sega CD", "Disc loaded", 0, cdd.loaded != 0);
    kr_inspect_put(info, part, "Sega CD", "Drive status", 2, cdd.status);
    kr_inspect_put(info, part, "Sega CD", "CDC mode register", 4, scd.regs[2].w);
  }
}

#ifdef KEGAREFUSION_32X
static void kr_inspect_32x(kr_inspect_info *info, unsigned parts, const Kega32xBus *bus)
{
  if (parts & KR_INSPECT_VIDEO)
  {
    const unsigned part = KR_INSPECT_VIDEO;
    kr_inspect_put(info, part, "32X VDP", "Mode", 0, (bus->s.regs[0] & 1) ? bus->s.vdp[0] & 3 : 0);
    kr_inspect_put(info, part, "32X VDP", "Displayed framebuffer", 0, bus->s.vdp[5] & 1);
    kr_inspect_put(info, part, "32X VDP", "Pixel shift", 0, bus->s.vdp[1] & 1);
    kr_inspect_put(info, part, "32X VDP", "Priority inversion", 0, !!(bus->s.vdp[0] & 0x80));
    kr_inspect_put(info, part, "32X VDP", "Height", 0, (bus->s.vdp[0] & 0x40) ? 240 : 224);
    kr_inspect_put(info, part, "32X VDP", "Mode register", 4, bus->s.vdp[0]);
    kr_inspect_put(info, part, "32X VDP", "Framebuffer control", 4, bus->s.vdp[5]);
  }

  if (parts & KR_INSPECT_AUDIO)
  {
    const Kega32xPwmState *pwm = kegarefusion_32x_inspect_pwm();
    if (pwm)
    {
      const unsigned part = KR_INSPECT_AUDIO;
      kr_inspect_put(info, part, "32X PWM", "Control", 4, pwm->control);
      kr_inspect_put(info, part, "32X PWM", "Cycle", 4, pwm->cycle);
      kr_inspect_put(info, part, "32X PWM", "Left FIFO words", 0, pwm->count[0]);
      kr_inspect_put(info, part, "32X PWM", "Right FIFO words", 0, pwm->count[1]);
      kr_inspect_put(info, part, "32X PWM", "Left held output (signed)", 8, (uint32)pwm->current[0]);
      kr_inspect_put(info, part, "32X PWM", "Right held output (signed)", 8, (uint32)pwm->current[1]);
    }
  }

  if (parts & KR_INSPECT_HARDWARE)
  {
    const unsigned part = KR_INSPECT_HARDWARE;
    kr_inspect_put(info, part, "32X", "Adapter control", 4, bus->s.regs[0]);
    kr_inspect_put(info, part, "32X", "ROM bank", 0, bus->s.regs[2] & 3);
    kr_inspect_put(info, part, "32X", "Master SH-2 IRQ mask", 2, bus->s.irq_mask[0]);
    kr_inspect_put(info, part, "32X", "Slave SH-2 IRQ mask", 2, bus->s.irq_mask[1]);
  }
}
#endif

static void kr_inspect_peripherals(kr_inspect_info *info)
{
  const unsigned part = KR_INSPECT_HARDWARE;
  kr_inspect_put(info, part, "Console", "PAL", 0, vdp_pal);
  kr_inspect_put(info, part, "Console", "Region code", 2, region_code);
  kr_inspect_put(info, part, "Console", "Master clock (Hz)", 0, system_clock);
  kr_inspect_put(info, part, "Console", "Cartridge hardware flags", 2, cart.special);
  if (system_hw != SYSTEM_PICO)
    kr_inspect_put(info, part, "CPU", "Z80", 0, 1);
  if (kr_inspect_md())
  {
    kr_inspect_put(info, part, "CPU", "Main 68000", 0, 1);
    if (system_hw != SYSTEM_PICO)
    {
      kr_inspect_put(info, part, "Z80", "Bus and reset state", 2, zstate);
      kr_inspect_put(info, part, "Z80", "Bank address", 6, zbank);
    }
  }
  if (info->hardware & KR_HW_CD)
    kr_inspect_put(info, part, "CPU", "Sub 68000", 0, 1);
  if (info->hardware & KR_HW_32X)
  {
    kr_inspect_put(info, part, "CPU", "Master SH-2", 0, 1);
    kr_inspect_put(info, part, "CPU", "Slave SH-2", 0, 1);
  }
  if (info->hardware & KR_HW_SVP)
    kr_inspect_put(info, part, "CPU", "SVP SSP1601", 0, 1);
  if (system_hw == SYSTEM_PICO)
    kr_inspect_put(info, part, "Pico", "Page register", 2, pico_current);
  if (system_hw == SYSTEM_GG)
  {
    kr_inspect_put(info, part, "Game Gear serial", "Parallel data", 2, io_reg[1]);
    kr_inspect_put(info, part, "Game Gear serial", "Data direction / NMI", 2, io_reg[2]);
    kr_inspect_put(info, part, "Game Gear serial", "Transmit data", 2, io_reg[3]);
    kr_inspect_put(info, part, "Game Gear serial", "Receive data", 2, io_reg[4]);
    kr_inspect_put(info, part, "Game Gear serial", "Serial control", 2, io_reg[5]);
  }
}

RETRO_API bool retro_kegarefusion_inspect_info(unsigned version, unsigned parts,
                                              kr_inspect_info *info, size_t capacity)
{
  const unsigned valid = KR_INSPECT_MEMORY | KR_INSPECT_VIDEO | KR_INSPECT_AUDIO | KR_INSPECT_HARDWARE;
  if (!kr_inspect_content || !system_hw || version != KR_INSPECT_VERSION ||
      (parts & ~valid) || !info || capacity < sizeof(*info))
    return false;

  memset(info, 0, sizeof(*info));
  info->version = KR_INSPECT_VERSION;
  info->struct_size = sizeof(*info);
  kr_inspect_machine(info);
  kr_inspect_domains(info, parts);
  if (parts & KR_INSPECT_VIDEO) kr_inspect_vdp(info);
  if (parts & KR_INSPECT_AUDIO)
  {
    kr_inspect_psg(info);
    kr_inspect_fm(info);
    if (cart.special & HW_YX5200) kr_inspect_yx5200(info);
  }
  if (parts & KR_INSPECT_HARDWARE)
  {
    kr_inspect_peripherals(info);
    if (!kr_inspect_md()) kr_inspect_sms_cart(info);
  }
  if (kr_inspect_md()) kr_inspect_md_cart(info, parts);
  if (system_hw == SYSTEM_MCD)
  {
    kr_inspect_cd(info, parts);
    if (kr_megald_active) kr_inspect_megald(info, parts);
  }
#ifdef KEGAREFUSION_32X
  const Kega32xBus *bus = kegarefusion_32x_inspect_bus();
  if (bus) kr_inspect_32x(info, parts, bus);
#endif
  return true;
}

RETRO_API size_t retro_kegarefusion_inspect_read(unsigned domain, uint32_t offset,
                                                void *destination, size_t capacity)
{
  if (!kr_inspect_content || !system_hw || !destination || !capacity)
    return 0;
  const kr_memory_view view = kr_inspect_domain_view(domain);
  if (!view.data || offset >= view.size)
    return 0;
  const size_t available = view.size - offset;
  if (capacity > KR_INSPECT_MAX_READ) capacity = KR_INSPECT_MAX_READ;
  if (capacity > available) capacity = available;

  uint8 *out = (uint8 *)destination;
  size_t i;
  switch (view.order)
  {
    case KR_WORDS_BE:
#ifdef LSB_FIRST
      for (i = 0; i < capacity; ++i)
        out[i] = READ_BYTE(view.data, offset + i);
      break;
#endif
    case KR_BYTES:
      memcpy(out, view.data + offset, capacity);
      break;
    case KR_WORDS_LE:
#ifndef LSB_FIRST
      for (i = 0; i < capacity; ++i)
        out[i] = view.data[(offset + i) ^ 1u];
#else
      memcpy(out, view.data + offset, capacity);
#endif
      break;
    case KR_SMS_COLORS:
      /* GPX stores single-byte palette entries in host-endian words. */
      for (i = 0; i < capacity; ++i)
      {
        uint16 color;
        memcpy(&color, view.data + 2 * (offset + i), sizeof(color));
        out[i] = (uint8)color;
      }
      break;
  }
  return capacity;
}


int kr_inspect_vdp_write(unsigned domain, uint32_t offset,
                         const uint8 *source, size_t length, int mode5);

RETRO_API bool retro_kegarefusion_inspect_writable(unsigned domain)
{
  if (!kr_inspect_content || !system_hw)
    return false;
  switch (domain)
  {
    case KR_MEM_VRAM:
      /* The wide native VDP mode disconnects a data lane and requires a
       * separate mapping from the physical array to the internal SAT. */
      if (kr_inspect_mode5() && (reg[1] & 0x80) && !kr_firecore_active)
        return false;
      break;
    case KR_MEM_CRAM:
      if (kr_inspect_md() && !kr_inspect_mode5())
        return false;
      break;
    case KR_MEM_RAM:
    case KR_MEM_ZRAM:
    case KR_MEM_VSRAM:
    case KR_MEM_CD_PRG:
    case KR_MEM_CD_WORD:
    case KR_MEM_CD_PCM:
    case KR_MEM_CD_BRAM:
      break;
    default:
      return false;
  }
  const kr_memory_view view = kr_inspect_domain_view(domain);
  return view.data && view.size;
}

RETRO_API size_t retro_kegarefusion_inspect_write(unsigned domain, uint32_t offset,
                                                 const void *source, size_t length)
{
  if (!source || !length || length > KR_INSPECT_MAX_WRITE ||
      !retro_kegarefusion_inspect_writable(domain))
    return 0;
  const kr_memory_view view = kr_inspect_domain_view(domain);
  if (offset >= view.size || length > view.size - offset)
    return 0;

  const uint8 *bytes = (const uint8 *)source;
  if (domain == KR_MEM_VRAM || domain == KR_MEM_CRAM || domain == KR_MEM_VSRAM)
    return kr_inspect_vdp_write(domain, offset, bytes, length, kr_inspect_mode5()) ? length : 0;
  uint8 *destination = (uint8 *)view.data;
  if (view.order == KR_WORDS_BE)
  {
    for (size_t i = 0; i < length; ++i)
      WRITE_BYTE(destination, offset + i, bytes[i]);
  }
  else
    memcpy(destination + offset, bytes, length);
  return length;
}
