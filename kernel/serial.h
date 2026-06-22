#pragma once

#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include "kernel/spinlock.h"
#include "arch/x64/trap.h"  // trapframe_t

// ===================== 16550 UART register offsets from COM1 base =====================
#define COM1      0x3F8
#define COM1_IER  0x3F9   // Interrupt Enable Register
#define COM1_IIR  0x3FA   // Interrupt Identification Register
#define COM1_FCR  0x3FA   // FIFO Control Register (same port as IIR, write-only)
#define COM1_LCR  0x3FB   // Line Control Register
#define COM1_MCR  0x3FC   // Modem Control Register
#define COM1_LSR  0x3FD   // Line Status Register

// IER bits
#define IER_RX_ENABLE   0x01   // Enable RX data interrupt
#define IER_TX_ENABLE   0x02   // Enable TX holding register empty interrupt

// LSR bits
#define LSR_DR   0x01   // Data Ready (RX has data)
#define LSR_THRE 0x20   // TX Holding Register Empty

// RX ring buffer
#define SERIAL_RX_BUF_SIZE 256

void serial_init(void);

// RX state (for trap.c and proc.c)
extern uint8_t serial_rx_buf[];
extern uint32_t serial_rx_head;
extern uint32_t serial_rx_tail;
extern spinlock_t serial_rx_lock;
extern int32_t serial_read_waiter;   // pid_t is int32_t, avoid proc.h include
extern int serial_fd_count;
extern bool serial_irq_registered;

// ISR
void serial_irq_handler(trapframe_t *tf);

#ifdef NSERIAL

#define serial_putc(c)    ((void)0)
#define serial_puts(s)    ((void)0)
#define serial_put_hex(v) ((void)0)
#define serial_printf(...) ((void)0)

#else

void serial_putc(char c);
void serial_puts(const char *s);
void serial_put_hex(uint64_t val);
void serial_printf(const char *fmt, ...);

#endif
