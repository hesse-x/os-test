#ifndef LIBC_MEM_H_
#define LIBC_MEM_H_

#include <stdint.h>

void memcpy(uint8_t *source, uint8_t *dest, int nbytes);
void memset(uint8_t *dest, uint8_t val, uint32_t len);

#endif // LIBC_MEM_H_
