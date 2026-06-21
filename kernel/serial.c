#include "kernel/serial.h"
#include "arch/x64/utils.h"

#define COM1 0x3F8

void serial_init() {}

#ifndef NDEBUG

void serial_putc(char c) { outb(COM1, c); }

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
