/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "arch/x64/intel_stolen_early.h"

#include <stdint.h>

#include "arch/x64/utils.h"

#define PCI_CONFIG_ADDRESS 0xcf8
#define PCI_CONFIG_DATA 0xcfc

static struct intel_stolen_early_info early_info;

static uint32_t early_pci_read32(uint8_t bus, uint8_t dev, uint8_t func,
                                 uint8_t offset) {
  uint32_t address = 0x80000000u | ((uint32_t)bus << 16) |
                     ((uint32_t)dev << 11) | ((uint32_t)func << 8) |
                     (offset & 0xfcu);
  outl(PCI_CONFIG_ADDRESS, address);
  return inl(PCI_CONFIG_DATA);
}

bool intel_gen9_stolen_size(uint8_t gms, uint64_t *size) {
  if (!size)
    return false;
  if (gms < 0xf0)
    *size = (uint64_t)gms * 32u * 1024u * 1024u;
  else if (gms <= 0xfe)
    *size = ((uint64_t)(gms - 0xf0) * 4u + 4u) * 1024u * 1024u;
  else
    return false;
  return *size != 0;
}

void intel_stolen_early_detect(void) {
  early_info = (struct intel_stolen_early_info){0};
  uint32_t id = early_pci_read32(0, 2, 0, 0x00);
  uint32_t revision = early_pci_read32(0, 2, 0, 0x08);
  if ((id & 0xffff) != 0x8086 || (id >> 16) != 0xa780 ||
      (revision & 0xff) != 0x04)
    return;

  early_info.matched = true;
  early_info.gmch_ctrl = early_pci_read32(0, 2, 0, 0x50) & 0xffff;
  early_info.gms = early_info.gmch_ctrl >> 8;
  early_info.ggms = (early_info.gmch_ctrl >> 6) & 0x3;
  uint64_t base = early_pci_read32(0, 2, 0, 0xc0) & 0xfff00000u;
  base |= (uint64_t)early_pci_read32(0, 2, 0, 0xc4) << 32;
  uint64_t size;
  if (!intel_gen9_stolen_size(early_info.gms, &size) || !base ||
      (base & ((1u << 20) - 1)) || base > UINT64_MAX - size)
    return;
  early_info.base = base;
  early_info.size = size;
  early_info.valid = true;
}

const struct intel_stolen_early_info *intel_stolen_early_get(void) {
  return &early_info;
}
