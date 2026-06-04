// Higher-half内核，-fPIE编译
// kernel_main: 虚拟地址运行，init_mem + 串口 + framebuffer 输出
#include <stddef.h>
#include <stdint.h>

#include "macro.h"
#include "kernel.h"
#include "mem.h"
#include "serial.h"
#include "fb.h"
#include "isr.h"
#include "kbd.h"

static void kbd_echo(char c) {
  fb_putc(c, 0xFFFFFF);
}

extern "C" {

void kernel_main(int32_t magic_num, uintptr_t addr) {
  init_mem(addr);

  serial_init();
  isr_init();

  if (magic_num == MULTIBOOT2_BOOTLOADER_MAGIC) {
    serial_puts("OK\n");
  } else {
    serial_puts("FAIL\n");
  }

  clear();
  prints("Hello, framebuffer!", 0xFFFFFF);

  kbd_register_handler(kbd_echo);

  while (1)
    __asm__ volatile("hlt");
}
} // extern C
