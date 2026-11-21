/* Physical writes between completed frames. No FIFO/address changes, guest
 * bus calls or updates to lines that were already rendered. */
int kr_inspect_vdp_write(unsigned domain, uint32_t offset,
                         const uint8 *source, size_t length, int mode5)
{
  size_t i;
  if (domain == KR_MEM_VRAM)
  {
    int sat_changed = 0;
    for (i = 0; i < length; ++i)
    {
      const unsigned index = offset + i;
      int name;
      if (mode5)
      {
        if ((index & sat_base_mask) == satb &&
            READ_BYTE(sat, index & sat_addr_mask) != source[i])
        {
          WRITE_BYTE(sat, index & sat_addr_mask, source[i]);
          sat_changed = 1;
        }
        if (READ_BYTE(vram, index) == source[i])
          continue;
        WRITE_BYTE(vram, index, source[i]);
      }
      else
      {
        if (vram[index] == source[i])
          continue;
        vram[index] = source[i];
      }
      MARK_BG_DIRTY(index);
    }
    if (sat_changed)
      ++kega_vdp_sat_revision;
    return 1;
  }

  if (domain == KR_MEM_VSRAM)
  {
    for (i = 0; i < length; ++i)
      WRITE_BYTE(vsram, offset + i, source[i]);
    return 1;
  }

  const int firecore = KR_FIRECORE_VDP_ENABLED;
  const int gg = system_hw == SYSTEM_GG;
  const int packed8 = !mode5 && !gg;
  uint8 *storage = firecore ? kr_firecore_cram : cram;
  const unsigned first = packed8 ? offset : offset >> 1;
  const unsigned last = packed8 ? offset + length - 1 : (offset + length - 1) >> 1;
  uint16 colors[KR_INSPECT_MAX_WRITE];

  for (i = first; i <= last; ++i)
    memcpy(&colors[i - first], storage + i * 2, sizeof(colors[0]));
  for (i = 0; i < length; ++i)
  {
    const unsigned address = offset + i;
    if (packed8)
      colors[address - first] = source[i];
    else
    {
      const unsigned shift = (gg ? (address & 1u) : ((address & 1u) ^ 1u)) * 8;
      uint16 *color = &colors[(address >> 1) - first];
      *color = (*color & ~(0xffu << shift)) | ((unsigned)source[i] << shift);
    }
  }

  /* Stage all palette words before writing any. The narrow MD/GG color
   * formats cannot represent bits outside the physical palette width. */
  if (!firecore && (mode5 || gg))
  {
    const unsigned mask = gg ? 0xfffu : 0x1ffu;
    for (i = first; i <= last; ++i)
      if (colors[i - first] & ~mask)
        return 0;
  }

  for (i = first; i <= last; ++i)
  {
    const uint16 data = colors[i - first];
    memcpy(storage + i * 2, &data, sizeof(data));
    if (firecore)
    {
      if (i & 0x7e) color_update_firecore(i, data);
      if ((i >> 1) == border) color_update_firecore(i & 1, data);
    }
    else if (mode5)
    {
      if (i & 15) color_update_m5(i, data);
      if (i == border) color_update_m5(0, data);
    }
    else
    {
      color_update_m4(i, data);
      if (i == (0x10u | (border & 15u))) color_update_m4(0x40, data);
    }
  }
  return 1;
}
