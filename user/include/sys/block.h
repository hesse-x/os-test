#ifndef _SYS_BLOCK_H
#define _SYS_BLOCK_H

#include <sys/types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLOCK_DIR_READ  0
#define BLOCK_DIR_WRITE 1

int block_read(uint32_t lba, void *buf, uint32_t count);
int block_write(uint32_t lba, const void *data, uint32_t count);
int block_async(uint32_t lba, void *buf, uint32_t count, uint8_t dir);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_BLOCK_H */
