/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "lifetime.h"

#include <stddef.h>

static bool valid(enum os_live_resource resource) {
  return resource >= 0 && resource < OS_LIVE_RESOURCE_COUNT;
}

bool os_lifetime_acquire(struct os_lifetime_counters *counters,
                         enum os_live_resource resource) {
  if (counters == NULL || !valid(resource) ||
      counters->live[resource] == SIZE_MAX)
    return false;
  ++counters->live[resource];
  return true;
}

bool os_lifetime_release(struct os_lifetime_counters *counters,
                         enum os_live_resource resource) {
  if (counters == NULL || !valid(resource) || counters->live[resource] == 0)
    return false;
  --counters->live[resource];
  return true;
}

size_t os_lifetime_total(const struct os_lifetime_counters *counters) {
  if (counters == NULL)
    return 0;
  size_t total = 0;
  for (size_t i = 0; i < OS_LIVE_RESOURCE_COUNT; ++i)
    total += counters->live[i];
  return total;
}
