#ifndef UTILS_OS_UTILS_H_
#define UTILS_OS_UTILS_H_
#include <stdint.h>

#define UNUSED(x) (void)(x)

#define L16(address) (uint16_t)((address)&0xFFFF)
#define H16(address) (uint16_t)(((address) >> 16) & 0xFFFF)

#endif // UTILS_OS_UTILS_H_
