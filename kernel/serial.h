#pragma once

#include <stdint.h>
#include <stdarg.h>

void serial_init();

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
