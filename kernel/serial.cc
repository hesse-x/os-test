#include "kernel/serial.h"
#include "arch/x86/utils.h"

#define COM1 0x3F8

void serial_init() {}

void serial_putc(char c) { outb(COM1, c); }

void serial_puts(const char *s) {
  while (*s) {
    serial_putc(*s++);
  }
}

void serial_put_hex(uint32_t val) {
  const char hex[] = "0123456789ABCDEF";
  serial_puts("0x");
  for (int i = 28; i >= 0; i -= 4) {
    serial_putc(hex[(val >> i) & 0xF]);
  }
}
