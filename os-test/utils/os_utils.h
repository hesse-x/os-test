#ifndef UTILS_GLOBAL_CONSTEXPR_H_
#define UTILS_GLOBAL_CONSTEXPR_H_
#include <stdint.h>

#define UNUSED(x) (void)(x)

#define L16(address) (uint16_t)((address)&0xFFFF)
#define H16(address) (uint16_t)(((address) >> 16) & 0xFFFF)
#define ALIGN(x, align) (((x) + (align) - 1) & ~((align) - 1))

#endif // UTILS_GLOBAL_CONSTEXPR_H_
