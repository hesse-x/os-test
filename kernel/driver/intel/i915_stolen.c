/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "arch/x64/intel_stolen_early.h"
#include "arch/x64/memlayout.h"
#include "kernel/driver/intel/i915_drv.h"
#include "kernel/driver/pci.h"
#include "kernel/xcore/mem/alloc.h"
#include <stddef.h>
#include <stdint.h>
#include <xos/errno.h>
#include <xos/page.h>

static int i915_validate_ownership(const struct page *frames,
                                   size_t frame_count, size_t first,
                                   size_t pages, size_t *conflict) {
  if (!frames || first >= frame_count || pages > frame_count - first)
    return -ERANGE;
  for (size_t i = 0; i < pages; i++) {
    if (frames[first + i].status == PAGE_RESERVED)
      continue;
    if (conflict)
      *conflict = first + i;
    return -EBUSY;
  }
  return 0;
}

int i915_stolen_probe(struct i915_device *i915,
                      struct i915_memory_layout *layout) {
  if (!i915 || !layout)
    return -EINVAL;
  pci_device *pdev = i915->pdev;
  uint16_t gmch = pci_read_config16(pdev->bus, pdev->dev, pdev->func, 0x50);
  uint8_t gms = gmch >> 8;
  uint8_t ggms = (gmch >> 6) & 0x3;
  uint64_t size;
  if (!intel_gen9_stolen_size(gms, &size) || ggms == 0)
    return -EINVAL;

  uint64_t base =
      pci_read_config(pdev->bus, pdev->dev, pdev->func, 0xc0) & 0xfff00000u;
  base |= (uint64_t)pci_read_config(pdev->bus, pdev->dev, pdev->func, 0xc4)
          << 32;
  if (!base || (base & ((1u << 20) - 1)) || base > UINT64_MAX - size)
    return -ERANGE;

  const struct intel_stolen_early_info *early = intel_stolen_early_get();
  if (!early->matched || !early->valid || early->gmch_ctrl != gmch ||
      early->base != base || early->size != size)
    return -EIO;

  size_t first = PHY_TO_PAGE(base);
  size_t pages = size / PAGE_SIZE;
  int rc = i915_validate_ownership(bfc_frames, total_page_frames, first, pages,
                                   NULL);
  if (rc)
    return rc;

  *layout = (struct i915_memory_layout){
      .gmch_ctrl = gmch,
      .gms = gms,
      .ggms = ggms,
      .stolen_base = base,
      .stolen_size = size,
      .ggtt_size = (uint64_t)(1u << ggms) * 1024u * 1024u,
      .aperture_size = pdev->bar[2].size,
  };
  return 0;
}

#ifdef TEST
int i915_stolen_test_ownership(const uint8_t *statuses, size_t count,
                               size_t first, size_t pages, size_t *conflict) {
  if (!statuses || count > 8)
    return -EINVAL;
  struct page frames[8] = {0};
  for (size_t i = 0; i < count; i++)
    frames[i].status = (page_status)statuses[i];
  return i915_validate_ownership(frames, count, first, pages, conflict);
}
#endif
