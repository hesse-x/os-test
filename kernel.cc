// Higher-half内核，-fPIE编译
// kernel_main: 虚拟地址运行，init_mem + 串口 + 显存填白
#include <stddef.h>
#include <stdint.h>

#include "macro.h"
#include "kernel.h"
#include "mem.h"
#include "serial.h"

static void fill_white() {
  char *s = (char *)fb_mapped_vaddr;
  for (int32_t i = 0; i < fb_size; i++) {
    s[i] = 0xFF;
  }
}

extern "C" {

void kernel_main(int32_t magic_num, uintptr_t addr) {
  init_mem(addr);

  serial_init();

  if (magic_num == MULTIBOOT2_BOOTLOADER_MAGIC) {
    serial_puts("OK\n");
  } else {
    serial_puts("FAIL\n");
  }

  if (fb_mapped_vaddr != NULL && fb_size != 0) {
    fill_white();
  }
}
} // extern C
