/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x64/apic.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/driver/serial.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/xtask.h"
#include "utils/kvformat.h"

#include <xos/errno.h>
#include <xos/ioctl.h>  // TCGETS
#include <xos/socket.h> // POLLIN/POLLOUT

// ===================== TX lock =====================
spinlock serial_tx_lock = SPINLOCK_INIT;

// ===================== RX =====================
// Serial input has been removed entirely (see remove_serial_input.md):
// the OS has no poll wakeup path for device fds, and serial input was
// debug-only, off the Wayland/ELF/gcc acceptance axes. TX stays polled —
// serial_init() keeps IER disabled, so no RX ISR, ring buffer, or read
// path exists. /dev/serial remains open/write/poll-POLLOUT/ioctl only.

void serial_init(void) {
  outb(COM1_IER, 0x00); // Disable all interrupts (TX is polled, no RX)
  outb(COM1_LCR, 0x80); // Enable DLAB
  outb(COM1, 0x01);     // Divisor low byte: 115200 baud
  outb(COM1_IER, 0x00); // Divisor high byte
  outb(COM1_LCR, 0x03); // 8N1, disable DLAB
  outb(COM1_FCR, 0xC7); // Enable FIFO, clear, 14-byte threshold
  outb(COM1_MCR, 0x03); // DTR + RTS
}

static void serial_putc_raw(char c) {
  while (!(inb(COM1_LSR) & LSR_THRE))
    ;
  outb(COM1, c);
}

// Line-start flag: next non-newline char gets a timestamp prefix.
// Protected by serial_tx_lock; \n sets it true, \r is transparent.
static bool serial_line_start = true;

// Emit boot-time prefix [  sec.mmm] at line starts.
// No-op until TSC is calibrated (early boot output skips the prefix).
static void emit_timestamp_locked(void) {
  if (tsc_freq == 0)
    return;
  uint64_t ns = sched_clock();
  uint64_t ms_total = ns / 1000000ULL;
  uint64_t sec = ms_total / 1000;
  uint64_t ms = ms_total % 1000;
  char buf[16];
  int p = 0;
  buf[p++] = '[';
  // seconds, right-aligned width 5, space-padded
  char tmp[6];
  int tl = 0;
  uint64_t s = sec;
  if (s == 0)
    tmp[tl++] = '0';
  else
    while (s) {
      tmp[tl++] = '0' + (s % 10);
      s /= 10;
    }
  while (tl < 5)
    tmp[tl++] = ' ';
  while (tl > 0)
    buf[p++] = tmp[--tl];
  buf[p++] = '.';
  // milliseconds, zero-padded width 3
  char mtmp[4];
  int ml = 0;
  uint64_t m = ms;
  if (m == 0)
    mtmp[ml++] = '0';
  else
    while (m) {
      mtmp[ml++] = '0' + (m % 10);
      m /= 10;
    }
  while (ml < 3)
    mtmp[ml++] = '0';
  while (ml > 0)
    buf[p++] = mtmp[--ml];
  buf[p++] = ']';
  buf[p++] = ' ';
  for (int i = 0; i < p; i++)
    serial_putc_raw(buf[i]);
}

static void serial_putc(char c) {
  uint64_t flags;
  spin_lock_irqsave(&serial_tx_lock, &flags);
  if (serial_line_start && c != '\n' && c != '\r') {
    emit_timestamp_locked();
    serial_line_start = false;
  }
  serial_putc_raw(c);
  if (c == '\n')
    serial_line_start = true;
  spin_unlock_irqrestore(&serial_tx_lock, flags);
}

static void serial_puts(const char *s) {
  while (*s) {
    serial_putc(*s++);
  }
}

struct serial_buf_arg {
  char *buf;
  int pos;
  int cap;
};

static void serial_buf_putc(char c, void *arg) {
  struct serial_buf_arg *a = (struct serial_buf_arg *)arg;
  if (a->pos < a->cap)
    a->buf[a->pos++] = c;
}

void serial_printf(const char *fmt, ...) {
  char buf[256];
  struct serial_buf_arg a = {buf, 0, 255};
  va_list ap;
  va_start(ap, fmt);
  kvformat(serial_buf_putc, &a, fmt, ap);
  va_end(ap);
  buf[a.pos] = '\0';
  serial_puts(buf);
}

void serial_vprintf(const char *fmt, va_list ap) {
  char buf[256];
  struct serial_buf_arg a = {buf, 0, 255};
  kvformat(serial_buf_putc, &a, fmt, ap);
  buf[a.pos] = '\0';
  serial_puts(buf);
}

// ===================== Serial dev_ops (VFS callback dispatch)
// =====================

static int serial_dev_open(xtask *proc, int fd) {
  // Serial input is removed; open is a no-op (VFS still requires the callback).
  // IER stays disabled — TX is polled, no RX ISR to register.
  return 0;
}

static int serial_dev_close(xtask *proc, int fd) {
  // No per-fd state to clean up (no IRQ registration, no reference count).
  return 0;
}

static ssize_t serial_dev_write(xtask *proc, int fd, const void *buf,
                                size_t count) {
  if (!buf)
    return -EFAULT;
  const char *src = (const char __force *)buf;
  for (size_t i = 0; i < count; i++)
    serial_putc(src[i]);
  return (ssize_t)count;
}

static long serial_dev_ioctl(uint32_t cmd, void *arg) {
  if (cmd == TCGETS)
    return 0; // serial is a tty
  return -ENOTTY;
}

static __poll serial_dev_poll(xtask *proc, int events) {
  // Serial is output-only: POLLOUT is always ready, POLLIN never reported.
  __poll revents = 0;
  if (events & POLLOUT)
    revents |= POLLOUT;
  return revents;
}

static struct dev_ops serial_ops = {
    .driver_pid = 0,
    .is_block = false,
    .open = serial_dev_open,
    .close = serial_dev_close,
    .read = NULL, // serial input removed — sys_read returns -ENOSYS
    .write = serial_dev_write,
    .ioctl = serial_dev_ioctl,
    .poll = serial_dev_poll,
};

void serial_dev_register(void) {
  int rc = devtmpfs_create("serial", &serial_ops, NULL);
  if (rc != 0) {
    printk(LOG_ERROR, "serial_dev_register: failed (rc=%d)\n", rc);
  }
}

// ===================== Driver registry =====================
#include "kernel/driver/driver.h"

dev_driver serial_driver = {
    .name = "serial",
    .pci_class = 0, // No PCI device (ISA UART at 0x3F8)
    .pci_vendor = 0,
    .pci_device = 0,
    .init = NULL, // serial_init() called early from xcore_init
    .ops = &serial_ops,
};
