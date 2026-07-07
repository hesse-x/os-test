/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_SERIAL_H
#define KERNEL_SERIAL_H

#include "kernel/driver/driver.h"
#include "kernel/xcore/spinlock.h"
#include <stdarg.h>

// ===================== 16550 UART register offsets from COM1 base
// =====================
#define COM1 0x3F8
#define COM1_IER 0x3F9 // Interrupt Enable Register
#define COM1_IIR 0x3FA // Interrupt Identification Register
#define COM1_FCR 0x3FA // FIFO Control Register (same port as IIR, write-only)
#define COM1_LCR 0x3FB // Line Control Register
#define COM1_MCR 0x3FC // Modem Control Register
#define COM1_LSR 0x3FD // Line Status Register

// IER bits
#define IER_TX_ENABLE 0x02 // Enable TX holding register empty interrupt

// LSR bits
#define LSR_THRE 0x20 // TX Holding Register Empty

void serial_init(void);

extern spinlock serial_tx_lock;

void serial_printf(const char *fmt, ...);
void serial_vprintf(const char *fmt, va_list ap);

#define SERIAL_PRINTF(...) serial_printf(__VA_ARGS__)
#define SERIAL_VPRINTF(fmt, ap) serial_vprintf(fmt, ap)

// Register serial device in devtmpfs (called by vfs_init)
void serial_dev_register(void);

extern struct dev_driver serial_driver;

#endif /* KERNEL_SERIAL_H */
