#include "kernel/serial.h"
#include "kernel/trap.h"
#include "arch/x64/utils.h"

// ===================== TX lock =====================
spinlock_t serial_tx_lock;

// ===================== RX ring buffer =====================

uint8_t serial_rx_buf[SERIAL_RX_BUF_SIZE];
uint32_t serial_rx_head = 0;  // ISR write position
uint32_t serial_rx_tail = 0;  // sys_read read position
spinlock_t serial_rx_lock;
int32_t serial_read_waiter = -1;
int serial_fd_count = 0;
bool serial_irq_registered = false;

void serial_init(void) {
    outb(COM1_IER, 0x00);    // Disable all interrupts
    outb(COM1_LCR, 0x80);    // Enable DLAB
    outb(COM1,     0x01);    // Divisor low byte: 115200 baud
    outb(COM1_IER, 0x00);    // Divisor high byte
    outb(COM1_LCR, 0x03);    // 8N1, disable DLAB
    outb(COM1_FCR, 0xC7);    // Enable FIFO, clear, 14-byte threshold
    outb(COM1_MCR, 0x03);    // DTR + RTS
    // IER RX enable deferred to sys_open_dev(DEV_SERIAL)
}

#ifndef NSERIAL

void serial_putc(char c) {
    uint64_t flags;
    spin_lock_irqsave(&serial_tx_lock, &flags);
    while (!(inb(COM1_LSR) & LSR_THRE))
        ;
    outb(COM1, c);
    spin_unlock_irqrestore(&serial_tx_lock, flags);
}

// ISR: drain all available bytes from UART FIFO into kernel ring buffer
void serial_irq_handler(trapframe_t *tf) {
    uint64_t flags;
    spin_lock_irqsave(&serial_rx_lock, &flags);
    // Drain all available bytes from FIFO (Linux: read LSR in loop)
    while (inb(COM1_LSR) & LSR_DR) {
        uint8_t c = inb(COM1);
        uint32_t next = (serial_rx_head + 1) % SERIAL_RX_BUF_SIZE;
        if (next != serial_rx_tail) {  // drop if full
            serial_rx_buf[serial_rx_head] = c;
            serial_rx_head = next;
        }
    }
    // Wake blocked reader (like Linux tty_flip_buffer_push → wait queue wake)
    if (serial_read_waiter >= 0) {
        int32_t waiter = serial_read_waiter;
        serial_read_waiter = -1;
        spin_unlock_irqrestore(&serial_rx_lock, flags);
        wake_process(waiter);
        return;
    }
    spin_unlock_irqrestore(&serial_rx_lock, flags);
}

void serial_puts(const char *s) {
  while (*s) {
    serial_putc(*s++);
  }
}

void serial_put_hex(uint64_t val) {
  const char hex[] = "0123456789ABCDEF";
  serial_puts("0x");
  for (int i = 60; i >= 0; i -= 4) {
    serial_putc(hex[(val >> i) & 0xF]);
  }
}

void serial_printf(const char *fmt, ...) {
  char buf[256];
  int pos = 0;
  va_list ap;
  va_start(ap, fmt);

  while (*fmt && pos < 255) {
    if (*fmt != '%') {
      buf[pos++] = *fmt++;
      continue;
    }
    fmt++;  // skip '%'

    // Handle length modifier 'l'
    int is_long = 0;
    if (*fmt == 'l') { is_long = 1; fmt++; }

    switch (*fmt) {
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s) s = "(null)";
      while (*s && pos < 255) buf[pos++] = *s++;
      break;
    }
    case 'd': {
      long val = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
      char tmp[24];
      int i = 0, neg = 0;
      if (val < 0) { neg = 1; val = -val; }
      if (val == 0) { tmp[i++] = '0'; }
      while (val > 0 && i < 23) { tmp[i++] = '0' + val % 10; val /= 10; }
      if (neg) tmp[i++] = '-';
      while (i > 0 && pos < 255) buf[pos++] = tmp[--i];
      break;
    }
    case 'u': {
      unsigned long val = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
      char tmp[24];
      int i = 0;
      if (val == 0) { tmp[i++] = '0'; }
      while (val > 0 && i < 23) { tmp[i++] = '0' + val % 10; val /= 10; }
      while (i > 0 && pos < 255) buf[pos++] = tmp[--i];
      break;
    }
    case 'x':
    case 'X': {
      unsigned long val = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
      char tmp[24];
      int i = 0;
      if (val == 0) { tmp[i++] = '0'; }
      while (val > 0 && i < 23) {
        tmp[i++] = "0123456789abcdef"[val & 0xF];
        val >>= 4;
      }
      while (i > 0 && pos < 255) buf[pos++] = tmp[--i];
      break;
    }
    case 'p': {
      uint64_t val = (uint64_t)va_arg(ap, void *);
      buf[pos++] = '0'; buf[pos++] = 'x';
      int started = 0;
      for (int j = 60; j >= 0; j -= 4) {
        int nibble = (val >> j) & 0xF;
        if (nibble || started || j == 0) {
          started = 1;
          if (pos < 255) buf[pos++] = "0123456789abcdef"[nibble];
        }
      }
      break;
    }
    case 'c': {
      char c = (char)va_arg(ap, int);
      if (pos < 255) buf[pos++] = c;
      break;
    }
    case '%':
      buf[pos++] = '%';
      break;
    default:
      buf[pos++] = '%';
      if (pos < 255) buf[pos++] = *fmt;
      break;
    }
    fmt++;
  }

  va_end(ap);
  buf[pos] = '\0';
  serial_puts(buf);
}

#endif
