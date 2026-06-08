// 64位 higher-half内核，-mcmodel=kernel编译
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
#include "arch/x64/paging.h"
#include "kernel/proc.h"
#include "driver/ata.h"
#include "arch/x64/smp.h"

extern "C" {

void kernel_init_finish() {
  // 禁止 bump 分配器
  bump_disable();
}

// Read shell.elf from disk (LBA 1) via ATA PIO
#define SHELL_ELF_SECTORS 16
#define SHELL_ELF_BUFSIZE (SHELL_ELF_SECTORS * 512)

static bool load_shell_from_disk(uint8_t *buf, uint32_t buf_size) {
  ata_read_lba(1, SHELL_ELF_SECTORS, buf);

  // Validate ELF magic
  if (buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
    return false;
  }
  return true;
}

void kernel_main(boot_info *bi) {
  serial_init();

  if (bi->magic != BOOT_INFO_MAGIC) {
    serial_puts("kernel_main: bad boot_info magic!\n");
    halt();
  }

  init_mem(bi);
  isr_init();
  kernel_init_finish();

  proc_init();
  init_idle_proc();

  clear();

  smp_boot_aps();

  // Load shell.elf from disk and create one user process
  static uint8_t shell_buf[SHELL_ELF_BUFSIZE];
  if (load_shell_from_disk(shell_buf, SHELL_ELF_BUFSIZE)) {
    process_create_elf(shell_buf, SHELL_ELF_BUFSIZE);
  } else {
    serial_puts("kernel_main: shell.elf not found\n");
  }

  schedule();

  while (1) __asm__ volatile("hlt");
}
}
