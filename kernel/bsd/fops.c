/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/fops.h"

/* fops 实例由各子模块定义:
 *   sysfs_fops        — kernel/bsd/sysfs.c (S1)
 *   evdev_consumer_fops / evdev_owner_fops / evdev_control_fops
 *                    — kernel/bsd/evdev_broker.c
 *   dev_kernel_fops / dev_ipc_fops — 后续迁移时定义
 * 本文件保留用于未来通用 fops 辅助函数。
 */
