// 64位 higher-half内核，-mcmodel=kernel编译
// kernel_main: 虚拟地址运行，init_mem + 串口 + framebuffer 输出
#include <stddef.h>
#include <stdint.h>

#include "common/macro.h"
#include "kernel/kernel.h"
#include "kernel/mem/alloc.h"
#include "kernel/mem/slab.h"
#include "kernel/serial.h"
#include "kernel/trap.h"
#include "arch/x64/paging.h"
#include "kernel/proc.h"
#include "kernel/ata.h"
#include "arch/x64/smp.h"
#include "kernel/elf_loader.h"
#include "common/dev.h"

extern "C" {

void kernel_init_finish() {
  // 禁止 bump 分配器
  bump_disable();
}

// Read ELF from disk via ATA PIO — two-phase: read header first, then allocate
// exact-size pages and read the rest. Returns BFC-allocated buffer (caller must free_page)
// or nullptr on failure.
#define ELF_SLOT_SECTORS 100   // max slot size per ELF on disk (50KB)

static uint8_t *load_elf_from_disk(uint32_t lba, uint64_t *out_size, Page **out_page, size_t *out_npages) {
  // Phase 1: read first sector to parse ELF header
  uint8_t hdr_buf[512];
  ata_read_lba(lba, 1, hdr_buf);

  if (hdr_buf[0] != 0x7F || hdr_buf[1] != 'E' || hdr_buf[2] != 'L' || hdr_buf[3] != 'F') {
    return nullptr;
  }

  Elf64_Ehdr *ehdr = (Elf64_Ehdr *)hdr_buf;
  // Validate program headers are within the first sector
  if (ehdr->e_phentsize == 0 ||
      ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > 512) {
    serial_puts("load_elf_from_disk: program headers exceed first sector\n");
    return nullptr;
  }
  uint64_t file_end = 0;
  for (int i = 0; i < ehdr->e_phnum; i++) {
    Elf64_Phdr *phdr = (Elf64_Phdr *)(hdr_buf + ehdr->e_phoff + i * ehdr->e_phentsize);
    if (phdr->p_type == PT_LOAD) {
      uint64_t seg_end = phdr->p_offset + phdr->p_filesz;
      if (seg_end > file_end) file_end = seg_end;
    }
  }

  uint32_t total_sectors = (uint32_t)((file_end + 511) / 512);
  if (total_sectors > ELF_SLOT_SECTORS) {
    serial_puts("load_elf_from_disk: ELF too large\n");
    return nullptr;
  }
  if (total_sectors < 1) total_sectors = 1;

  uint64_t file_size = total_sectors * 512;

  // Phase 2: allocate pages and read
  size_t npages = (file_size + 4095) / 4096;
  Page *page = bfc_alloc.alloc_page(npages);
  if (!page) {
    serial_puts("load_elf_from_disk: alloc_page failed\n");
    return nullptr;
  }

  uint8_t *buf = (uint8_t *)phys_to_virt(page_to_phys(page));

  // Copy header sector already read
  __builtin_memcpy(buf, hdr_buf, 512);

  // Read remaining sectors
  if (total_sectors > 1) {
    ata_read_lba(lba + 1, total_sectors - 1, buf + 512);
  }

  *out_size = file_size;
  *out_page = page;
  *out_npages = npages;
  return buf;
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
  slab_init();

  proc_init();

  smp_boot_aps();

  // Create BSP idle process
  proc_t *bsp_idle = create_idle_process(0);
  if (!bsp_idle) {
    serial_puts("kernel_main: create BSP idle failed\n");
    halt();
  }

  // Load user processes from disk
  // LBA layout: 1=disk_driver(100s), 101=fs_driver(100s), 201=init(100s)
  // kbd_driver, kms_driver, terminal, shell are spawned by init from FAT32

  {
    uint64_t sz; Page *pg; size_t np;
    uint8_t *elf = load_elf_from_disk(1, &sz, &pg, &np);
    if (elf) {
      proc_t *p = process_create_elf(elf, sz);
      bfc_alloc.free_page(pg, np);
      if (p) { /* disk_driver self-registers via device_register() */ }
    }
    else serial_puts("kernel_main: disk_driver.elf not found\n");
  }

  {
    uint64_t sz; Page *pg; size_t np;
    uint8_t *elf = load_elf_from_disk(101, &sz, &pg, &np);
    if (elf) {
      proc_t *p = process_create_elf(elf, sz);
      bfc_alloc.free_page(pg, np);
    }
    else serial_puts("kernel_main: fs_driver.elf not found\n");
  }

  {
    uint64_t sz; Page *pg; size_t np;
    uint8_t *elf = load_elf_from_disk(201, &sz, &pg, &np);
    if (elf) { proc_t *init_proc = process_create_elf(elf, sz); bfc_alloc.free_page(pg, np); if (init_proc) { init_pid = init_proc->pid; } }
    else serial_puts("kernel_main: init.elf not found\n");
  }

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
