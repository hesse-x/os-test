/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/fops.h"

// fops instances are defined in their respective submodules:
//   sysfs_fops                      — kernel/bsd/sysfs.c (S1)
//   evdev_consumer/owner/control    — kernel/bsd/evdev_broker.c
//   dev_kernel_fops / dev_ipc_fops  — defined on later migration
// This file is kept for future generic fops helpers.
