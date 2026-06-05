#pragma once

#include <stddef.h>

extern "C" {
void *memcpy(void *dst, const void *src, size_t n);
void serial_early_out(char c);
}
