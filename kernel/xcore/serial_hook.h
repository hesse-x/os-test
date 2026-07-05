/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_SERIAL_HOOK_H
#define KERNEL_XCORE_SERIAL_HOOK_H

// Minimal serial declarations for Xcore layer.
// Full definition lives in kernel/driver/serial.h (driver layer).
// Xcore must NOT include kernel/driver/ headers.

#include <stdarg.h>

void serial_init(void);

#ifdef NSERIAL

#define serial_printf(...) ((void)0)
#define serial_vprintf(fmt, ap) ((void)0)

#else

void serial_printf(const char *fmt, ...);
void serial_vprintf(const char *fmt, va_list ap);

#endif

#endif /* KERNEL_XCORE_SERIAL_HOOK_H */
