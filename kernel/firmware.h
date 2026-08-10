/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_FIRMWARE_H
#define KERNEL_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

#define FIRMWARE_MAX_SIZE (16U * 1024U * 1024U)
#define FIRMWARE_NAME_MAX 192U

struct device;

struct firmware {
  size_t size;
  const uint8_t *data;
};

int request_firmware(const struct firmware **out, const char *name,
                     const struct device *dev);
void release_firmware(const struct firmware *fw);

/* Starts the TEST-only malformed-input and lifecycle checks. */
void firmware_selftest_start(void);

#endif
