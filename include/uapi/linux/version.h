/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _LINUX_VERSION_H
#define _LINUX_VERSION_H

#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6, 8, 0)

#endif
