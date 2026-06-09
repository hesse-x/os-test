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

extern "C" {

void kernel_init_finish() {
  // 禁止 bump 分配器
  bump_disable();
}

// Read ELF from disk via ATA PIO
#define ELF_SECTORS 32
#define ELF_BUFSIZE (ELF_SECTORS * 512)

static bool load_elf_from_disk(uint8_t *buf, uint32_t buf_size, uint32_t lba) {
  ata_read_lba(lba, ELF_SECTORS, buf);

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
  // LBA layout: 1=disk_driver(32 sectors), 33=kbd_driver(32 sectors), 65=shell(32 sectors)
  static uint8_t elf_buf[ELF_BUFSIZE];

  if (load_elf_from_disk(elf_buf, ELF_BUFSIZE, 1)) {
    process_create_elf(elf_buf, ELF_BUFSIZE, 3);  // IOPL=3 for driver
  } else {
    serial_puts("kernel_main: disk_driver.elf not found\n");
  }

  if (load_elf_from_disk(elf_buf, ELF_BUFSIZE, 33)) {
    process_create_elf(elf_buf, ELF_BUFSIZE, 3);  // IOPL=3 for driver
  } else {
    serial_puts("kernel_main: kbd_driver.elf not found\n");
  }

  if (load_elf_from_disk(elf_buf, ELF_BUFSIZE, 65)) {
    process_create_elf(elf_buf, ELF_BUFSIZE, 0);  // IOPL=0 for shell
  } else {
    serial_puts("kernel_main: shell.elf not found\n");
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
