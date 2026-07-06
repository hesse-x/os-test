/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_H
#define KERNEL_H

#include <stddef.h>

#include "kernel/xcore/mem/alloc.h"

void kernel_main(boot_info *bi);
void kernel_init_finish(void);

// POSIX hostname (group 1). Accessed by sys_gethostname/sys_sethostname.
#define HOSTNAME_MAX 256
extern char hostname[HOSTNAME_MAX];
void hostname_set(const char *name, size_t len);
size_t hostname_get(char *dst, size_t maxlen);

// Layered init functions (defined in their own .c files)
void xcore_init(boot_info *bi);
void driver_init(void);
void bsd_init(void);
#endif // KERNEL_H
