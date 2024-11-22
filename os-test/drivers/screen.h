#ifndef DRIVERS_SCREEN_H_
#define DRIVERS_SCREEN_H_

#define WHITE_ON_BLACK 0x0f
#define RED_ON_WHITE 0xf4
#include "stdint.h"

/* Public kernel API */
void init_screen();
void clear_screen();
void kprint(const char *message);
void kprint_int(int32_t value);
void kprint_int64(int64_t value);
void kprint_hex(int32_t value);
void kprint_hex64(int64_t value);
void put_char(const char c, const char attr);

#endif // DRIVERS_SCREEN_H_
