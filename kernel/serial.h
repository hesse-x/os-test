#pragma once

#include <stdint.h>

extern "C" {
void serial_init();
void serial_putc(char c);
void serial_puts(const char *s);
void serial_put_hex(uint32_t val);
}
