// Higher-half内核，-fPIE编译
// kernel_main: 虚拟地址运行，init_mem + 串口 + framebuffer 输出
#include <stddef.h>
#include <stdint.h>

#include "common/macro.h"
#include "kernel/kernel.h"
#include "kernel/mem/alloc.h"
#include "kernel/serial.h"
#include "driver/fb.h"
#include "kernel/trap.h"
#include "driver/kbd.h"
#include "arch/x86/multiboot2.h"
#include "arch/x86/paging.h"
#include "kernel/proc.h"

extern "C" {

void kernel_init_finish() {
  // 清除 identity map（PD[0] 设为 not present）
  page_directory[0] = 0;
  flush_tlb();

  // 禁止 bump 分配器
  bump_disable();
}

void kernel_main(int32_t magic_num, uintptr_t addr) {
  init_mem(addr);

  serial_init();
  isr_init();
  kernel_init_finish();

  if (magic_num == MULTIBOOT2_BOOTLOADER_MAGIC) {
    serial_puts("OK\n");
  } else {
    serial_puts("FAIL\n");
  }

  // Process scheduler initialization
  proc_init();
  init_idle_proc();
  process_create(0x400000);
  process_create(0x400000);
  schedule();  // idle → first user process

  // idle loop: hlt waits for next interrupt
  while (1) __asm__ volatile("hlt");
}
}