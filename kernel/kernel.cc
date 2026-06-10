// 64位 higher-half内核，-mcmodel=kernel编译
// kernel_main: 虚拟地址运行，init_mem + 串口 + framebuffer 输出
#include <stddef.h>
#include <stdint.h>

#include "common/macro.h"
#include "kernel/kernel.h"
#include "kernel/mem/alloc.h"
#include "kernel/serial.h"
#include "kernel/fb.h"
#include "kernel/trap.h"
#include "arch/x64/paging.h"
#include "kernel/proc.h"
#include "kernel/ata.h"
#include "arch/x64/smp.h"
#include "common/elf.h"

extern "C" {

void kernel_init_finish() {
  // 禁止 bump 分配器
  bump_disable();
}

// Read ELF from disk via ATA PIO — dynamic size
// First read 1 sector to get ELF header, then read the remaining sectors
// based on the actual file size parsed from program headers.
#define ELF_MAX_SECTORS 50   // max slot size per ELF on disk (25KB)
#define ELF_MAX_BUFSIZE (ELF_MAX_SECTORS * 512)

static bool load_elf_from_disk(uint8_t *buf, uint32_t buf_size, uint32_t lba) {
  // Step 1: Read first sector (ELF header + possibly program headers)
  ata_read_lba(lba, 1, buf);

  // Validate ELF magic
  if (buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
    return false;
  }

  // Step 2: Parse ELF header to determine total file size
  Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
  uint64_t file_end = 0;
  for (int i = 0; i < ehdr->e_phnum; i++) {
    Elf64_Phdr *phdr = (Elf64_Phdr *)(buf + ehdr->e_phoff + i * ehdr->e_phentsize);
    if (phdr->p_type == PT_LOAD) {
      uint64_t seg_end = phdr->p_offset + phdr->p_filesz;
      if (seg_end > file_end) file_end = seg_end;
    }
  }

  // Calculate total sectors needed (round up)
  uint32_t total_sectors = (uint32_t)((file_end + 511) / 512);
  if (total_sectors > ELF_MAX_SECTORS) {
    serial_puts("load_elf_from_disk: ELF too large\n");
    return false;
  }
  if (total_sectors < 1) total_sectors = 1;

  // Step 3: Read remaining sectors (we already have sector 0)
  if (total_sectors > 1) {
    ata_read_lba(lba + 1, total_sectors - 1, buf + 512);
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

  // Initialize shared pages (after BFC allocator is ready)
  shm_init();

  clear();

  smp_boot_aps();

  // Create BSP idle process
  proc_t *bsp_idle = create_idle_process(0);
  if (!bsp_idle) {
    serial_puts("kernel_main: create BSP idle failed\n");
    halt();
  }

  // Load user processes from disk
  // LBA layout: 1=disk_driver(50s), 51=kbd_driver(50s), 101=shell(50s), 151=fs_driver(50s)
  static uint8_t elf_buf[ELF_MAX_BUFSIZE];

  if (load_elf_from_disk(elf_buf, ELF_MAX_BUFSIZE, 1)) {
    process_create_elf(elf_buf, ELF_MAX_BUFSIZE, 3);  // IOPL=3 for driver
  } else {
    serial_puts("kernel_main: disk_driver.elf not found\n");
  }

  if (load_elf_from_disk(elf_buf, ELF_MAX_BUFSIZE, 51)) {
    process_create_elf(elf_buf, ELF_MAX_BUFSIZE, 3);  // IOPL=3 for driver
  } else {
    serial_puts("kernel_main: kbd_driver.elf not found\n");
  }

  if (load_elf_from_disk(elf_buf, ELF_MAX_BUFSIZE, 101)) {
    process_create_elf(elf_buf, ELF_MAX_BUFSIZE, 0);  // IOPL=0 for shell
  } else {
    serial_puts("kernel_main: shell.elf not found\n");
  }

  if (load_elf_from_disk(elf_buf, ELF_MAX_BUFSIZE, 151)) {
    process_create_elf(elf_buf, ELF_MAX_BUFSIZE, 0);  // IOPL=0 for fs_driver
  } else {
    serial_puts("kernel_main: fs_driver.elf not found\n");
  }

  // BKL removed — fine-grained locks protect each subsystem
  sti();

  // Set current_proc to BSP idle, switch to idle kernel stack, enter idle_entry
  current_proc = bsp_idle;
  bsp_idle->state = RUNNING;
  per_cpu_tss[0].rsp0 = bsp_idle->k_stack_top;
  cpu_locals[0].tss_rsp0 = bsp_idle->k_stack_top;

  uint64_t idle_rsp = bsp_idle->k_rsp;
  __asm__ volatile(
      "movq %0, %%rsp\n"
      "popq %%rbx\n"
      "popq %%rbp\n"
      "popq %%r12\n"
      "popq %%r13\n"
      "popq %%r14\n"
      "popq %%r15\n"
      "retq\n"
      :: "r"(idle_rsp)
      : "memory");
  // never reaches here
}
}
