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

#define SERIAL_PRINTF(...) ((void)0)
#define SERIAL_VPRINTF(fmt, ap) ((void)0)

#else

void serial_printf(const char *fmt, ...);
void serial_vprintf(const char *fmt, va_list ap);

#define SERIAL_PRINTF(...) serial_printf(__VA_ARGS__)
#define SERIAL_VPRINTF(fmt, ap) serial_vprintf(fmt, ap)

#endif

#endif /* KERNEL_XCORE_SERIAL_HOOK_H */
