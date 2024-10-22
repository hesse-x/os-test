#include "os-test/utils/disk_io.h"
#include "os-test/utils/x86.h"
#include <stdint.h>

static void waitdisk(void) {
  while ((inb(0x1F7) & 0xC0) != 0x40)
    /* do nothing */;
}

void read_sect(void *dst, uint32_t secno) {
  // wait for disk to be ready
  // waitdisk();

  outb(0x1F2, 1); // count = 1
  outb(0x1F3, secno & 0xFF);
  outb(0x1F4, (secno >> 8) & 0xFF);
  outb(0x1F5, (secno >> 16) & 0xFF);
  outb(0x1F6, ((secno >> 24) & 0xF) | 0xE0);
  outb(0x1F7, 0x20); // cmd 0x20 - read sectors

  // wait for disk to be ready
  waitdisk();

  // read a sector
  insl(0x1F0, dst, SECTSIZE / 4);
}

/* *
 * readseg - read @count bytes at @offset from kernel into virtual address @va,
 * might copy more than asked.
 * */
void read_seg(void *dst, uint32_t count, uint32_t offset) {
  char *buffer = (char *)dst;
  char *end = buffer + count;
  // round down to sector boundary
  buffer -= offset % SECTSIZE;
  // translate from bytes to sectors; kernel starts at sector 1
  uint32_t secno = (offset / SECTSIZE);
  // If this is too slow, we could read lots of sectors at a time.
  // We'd write more to memory than asked, but it doesn't matter --
  // we load in increasing order.
  for (; buffer < end; buffer += SECTSIZE, ++secno) {
    read_sect(buffer, secno);
  }
}
