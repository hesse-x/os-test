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
#include "driver/ata.h"

extern "C" {

void kernel_init_finish() {
  // 清除 identity map（PD[0] 设为 not present）
  page_directory[0] = 0;
  flush_tlb();

  // 禁止 bump 分配器
  bump_disable();
}

// Read shell.elf from disk (LBA 1) via ATA PIO
// Disk layout: LBA 0 = MBR, LBA 1+ = shell.elf
// We read enough sectors to cover a typical small ELF (up to 16 sectors = 8KB)
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

  // Load shell.elf from disk via ATA PIO
  Page *buf_pages = bfc_alloc.alloc_page(2);  // 8KB
  if (buf_pages) {
    uint32_t buf_phys = (uint32_t)(buf_pages - BFCAllocator::frames) * PAGE_SIZE;
    uint8_t *buf = (uint8_t *)(buf_phys + VMA_BASE);

    if (load_shell_from_disk(buf, SHELL_ELF_BUFSIZE)) {
      process_create_elf(buf, SHELL_ELF_BUFSIZE);
    } else {
      process_create(0x400000);
    }
  } else {
    process_create(0x400000);
  }

  schedule();  // idle → first user process

  // idle loop: hlt waits for next interrupt
  while (1) __asm__ volatile("hlt");
}
}
