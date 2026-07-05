/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_SERIAL_H
#define KERNEL_SERIAL_H

#include "arch/x64/trap.h" // trapframe_t
#include "kernel/driver/driver.h"
#include "kernel/xcore/spinlock.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <xos/types.h> // pid_t

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
#define IER_RX_ENABLE 0x01 // Enable RX data interrupt
#define IER_TX_ENABLE 0x02 // Enable TX holding register empty interrupt

// LSR bits
#define LSR_DR 0x01   // Data Ready (RX has data)
#define LSR_THRE 0x20 // TX Holding Register Empty

// RX ring buffer
#define SERIAL_RX_BUF_SIZE 256

void serial_init(void);

// RX state (for trap.c and proc.c)
extern uint8_t serial_rx_buf[];
extern uint32_t serial_rx_head;
extern uint32_t serial_rx_tail;
extern spinlock serial_rx_lock;
extern spinlock serial_tx_lock;
extern pid_t serial_read_waiter;
extern int serial_fd_count;
extern bool serial_irq_registered;

#ifdef NSERIAL

#define SERIAL_PRINTF(...) ((void)0)
#define SERIAL_VPRINTF(fmt, ap) ((void)0)

#else

void serial_printf(const char *fmt, ...);
void serial_vprintf(const char *fmt, va_list ap);

#define SERIAL_PRINTF(...) serial_printf(__VA_ARGS__)
#define SERIAL_VPRINTF(fmt, ap) serial_vprintf(fmt, ap)

// Register serial device in devtmpfs (called by vfs_init)
void serial_dev_register(void);

struct dev_driver;
extern struct dev_driver serial_driver;

#endif

#endif /* KERNEL_SERIAL_H */
