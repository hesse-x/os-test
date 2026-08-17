/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "scene_adapter.h"

bool os_scene_adapter_validate(const struct os_scene_adapter *adapter) {
  return adapter != NULL && adapter->size == sizeof(*adapter) &&
         adapter->version == OS_SCENE_ADAPTER_VERSION &&
         adapter->snapshot != NULL;
}
