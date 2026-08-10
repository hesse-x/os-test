/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ARCH_X64_INTEL_STOLEN_EARLY_H
#define ARCH_X64_INTEL_STOLEN_EARLY_H

#include <stdbool.h>
#include <stdint.h>

struct intel_stolen_early_info {
  bool matched;
  bool valid;
  uint16_t gmch_ctrl;
  uint8_t gms;
  uint8_t ggms;
  uint64_t base;
  uint64_t size;
};

void intel_stolen_early_detect(void);
const struct intel_stolen_early_info *intel_stolen_early_get(void);
bool intel_gen9_stolen_size(uint8_t gms, uint64_t *size);

#endif
