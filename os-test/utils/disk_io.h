#ifndef UTILS_DISK_H_
#define UTILS_DISK_H_
#include <stdint.h>

#define SECTSIZE 512

/* read_sect - read a single sector at @secno into @dst */
void read_sect(void *dst, uint32_t secno);
void read_seg(void *dst, uint32_t count, uint32_t offset);
#endif // UTILS_DISK_H_
