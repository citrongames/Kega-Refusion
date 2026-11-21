#ifndef KR_CORE_INSPECTION_VALUES_H
#define KR_CORE_INSPECTION_VALUES_H

#include "../DebugInspectionAbi.h"
#include <stdio.h>

/* Only called while creating a bounded inspection snapshot. */
static inline void kr_inspect_put(kr_inspect_info *info, unsigned part,
                                  const char *device, const char *name,
                                  unsigned digits, uint64_t value)
{
  kr_inspect_value *entry;
  if (info->value_count >= KR_INSPECT_MAX_VALUES)
    return;
  entry = &info->values[info->value_count++];
  entry->part = part;
  entry->digits = digits;
  entry->value = value;
  snprintf(entry->device, sizeof(entry->device), "%s", device);
  snprintf(entry->name, sizeof(entry->name), "%s", name);
}

void kr_inspect_vdp(kr_inspect_info *info);
void kr_inspect_psg(kr_inspect_info *info);
void kr_inspect_fm(kr_inspect_info *info);
void kr_inspect_ym2612(kr_inspect_info *info);
void kr_inspect_ym2413(kr_inspect_info *info);
void kr_inspect_yx5200(kr_inspect_info *info);
void kr_inspect_md_cart(kr_inspect_info *info, unsigned parts);
void kr_inspect_sms_cart(kr_inspect_info *info);

#endif
