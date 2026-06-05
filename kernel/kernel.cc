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
  serial_puts("kernel_main: serial_init ok\n");

  if (bi->magic != BOOT_INFO_MAGIC) {
    serial_puts("kernel_main: bad boot_info magic!\n");
    halt();
  }

  serial_puts("kernel_main: boot_info ok, kernel_phys=");
  serial_put_hex(bi->kernel_phys);
  serial_puts("\n");

  init_mem(bi);
  serial_puts("kernel_main: init_mem ok\n");
  isr_init();
  serial_puts("kernel_main: isr_init ok\n");
  kernel_init_finish();
  serial_puts("kernel_main: bump disabled\n");

  proc_init();
  serial_puts("kernel_main: proc_init ok\n");
  init_idle_proc();
  serial_puts("kernel_main: init_idle_proc ok\n");

  serial_puts("free_page_nums: ");
  serial_put_hex(bfc_alloc.free_page_nums());
  serial_puts("\nfree_list: ");
  serial_put_hex((uint64_t)BFCAllocator::free_list);
  serial_puts("\nframes: ");
  serial_put_hex((uint64_t)BFCAllocator::frames);
  serial_puts("\ntotal_page_frames: ");
  serial_put_hex(total_page_frames);
  serial_puts("\n");

  // Load shell.elf from disk and create user process
  static uint8_t shell_buf[SHELL_ELF_BUFSIZE];
  proc_t *p = nullptr;
  if (load_shell_from_disk(shell_buf, SHELL_ELF_BUFSIZE)) {
    serial_puts("kernel_main: shell.elf loaded from disk\n");
    p = process_create_elf(shell_buf, SHELL_ELF_BUFSIZE);
  } else {
    serial_puts("kernel_main: shell.elf not found, using init_code\n");
    p = process_create(0x400000);
  }
  serial_puts("kernel_main: process_create ");
  serial_put_hex(p ? p->pid : -1);
  serial_puts("\n");

  clear();
  serial_puts("kernel_main: before schedule\n");
  schedule();  // idle → first user process
  serial_puts("kernel_main: after schedule\n");

  // idle loop: hlt waits for next interrupt
  while (1) __asm__ volatile("hlt");
}
}
