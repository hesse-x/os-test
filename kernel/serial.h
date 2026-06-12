#pragma once

#include <stdint.h>

extern "C" {
void serial_init();
}

#ifdef NDEBUG

#define serial_putc(c)    ((void)0)
#define serial_puts(s)    ((void)0)
#define serial_put_hex(v) ((void)0)

#else

extern "C" {
void serial_putc(char c);
void serial_puts(const char *s);
void serial_put_hex(uint64_t val);
}

#endif
