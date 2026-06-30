// 64位 higher-half内核，-mcmodel=kernel编译
// kernel_main: 虚拟地址运行，xcore_init + driver_init + bsd_init + idle + ELF加载
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "common/macro.h"
#include "common/boot.h"
#include "kernel/kernel.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/log.h"
#include "kernel/driver/serial.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/sched.h"
#include "kernel/bsd/proc.h"
#include "kernel/driver/ahci.h"
#include "common/elf.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"

// VFS data structure init (must run before driver_init so devtmpfs_create works)
void inode_init(void);
void page_cache_init(void);
void devtmpfs_init(void);

// Read ELF from disk via AHCI DMA — two-phase: read header first, then allocate
// exact-size pages and read the rest. Returns BFC-allocated buffer (caller must free_page)
// or NULL on failure.
#define ELF_SLOT_SECTORS 200   // max slot size per ELF on disk (100KB)

static uint8_t *load_elf_from_disk(uint32_t lba, uint64_t *out_size, Page **out_page, size_t *out_npages) {
  // Phase 1: read first sector to parse ELF header
  uint8_t hdr_buf[512];
  printk(LOG_DEBUG, "load_elf: LBA=%x", lba);
  if (ahci_read_lba(lba, 1, hdr_buf) != 0) {
    printk(LOG_ERROR, "load_elf: READ_FAILED\n");
    return NULL;
  }

  if (hdr_buf[0] != 0x7F || hdr_buf[1] != 'E' || hdr_buf[2] != 'L' || hdr_buf[3] != 'F') {
    printk(LOG_ERROR, "load_elf: BAD_MAGIC got %x\n", ((uint32_t *)hdr_buf)[0]);
    return NULL;
  }

  Elf64_Ehdr *ehdr = (Elf64_Ehdr *)hdr_buf;
  // Validate program headers are within the first sector
  if (ehdr->e_phentsize == 0 ||
      ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > 512) {
    printk(LOG_ERROR, "load_elf_from_disk: program headers exceed first sector\n");
    return NULL;
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
    printk(LOG_ERROR, "load_elf_from_disk: ELF too large\n");
    return NULL;
  }
  if (total_sectors < 1) total_sectors = 1;

  uint64_t file_size = total_sectors * 512;

  printk(LOG_DEBUG, "load_elf_from_disk: sectors=%x size=%lx\n", total_sectors, (unsigned long)file_size);

  // Phase 2: allocate pages and read
  size_t npages = (file_size + 4095) / 4096;
  Page *page = bfc_alloc_page(npages);
  if (!page) {
    printk(LOG_ERROR, "load_elf_from_disk: alloc_page failed\n");
    return NULL;
  }

  uint8_t *buf = (__force uint8_t *)phys_to_virt((__force phys_addr_t)page_to_phys(page));

  // Copy header sector already read
  __builtin_memcpy(buf, hdr_buf, 512);

  // Read remaining sectors
  if (total_sectors > 1) {
    if (ahci_read_lba(lba + 1, total_sectors - 1, buf + 512) != 0) {
      bfc_free_page(page, npages);
      return NULL;
    }
  }

  *out_size = file_size;
  *out_page = page;
  *out_npages = npages;
  return buf;
}

void kernel_main(boot_info *bi) {
  // Layered initialization
  xcore_init(bi);

  // VFS data structures must be initialized before driver_init
  // so that devtmpfs_create() calls during driver_init survive
  inode_init();
  page_cache_init();
  devtmpfs_init();

  driver_init();
  bsd_init();

  printk(LOG_INFO, "kernel_main: all subsystems initialized\n");

  // Create BSP idle process
  xtask_t *bsp_idle = create_idle_process(0);
  if (!bsp_idle) {
    printk(LOG_ERROR, "kernel_main: create BSP idle failed\n");
    halt();
  }
  printk(LOG_INFO, "kernel_main: BSP idle created\n");

  // Load user processes from disk
  // LBA layout: 1-100=unused(gap), 101=init(100s), 201+=FAT32
  int try_ports[] = { 0, 1, 2, 3, 4, 5 };

  bool init_loaded = false;

  for (int pi = 0; pi < 6; pi++) {
    int port = try_ports[pi];
    if (port < 0) continue;

    // Switch to this port; skip if no device detected
    if (ahci_set_active_port(port) != 0) {
      printk(LOG_INFO, "kernel_main: skip port %x (no device)\n", port);
      continue;
    }

    if (!init_loaded) {
      printk(LOG_INFO, "kernel_main: loading init...\n");
      uint64_t sz; Page *pg; size_t np;
      uint8_t *elf = load_elf_from_disk(101, &sz, &pg, &np);
      if (elf) {
        xtask_t *init_proc = process_create_elf(elf, sz);
        bfc_free_page(pg, np);
        if (init_proc) { init_loaded = true; init_pid = init_proc->pid; printk(LOG_INFO, "kernel_main: init created\n"); }
      }
    }

    if (init_loaded) break;
  }

  if (!init_loaded) printk(LOG_ERROR, "kernel_main: init.elf FAILED on all ports\n");

  printk(LOG_INFO, "kernel_main: all tasks loaded, entering idle\n");

  sti();

  // Set current_task to BSP idle, switch to idle kernel stack, enter idle_entry
  current_task = bsp_idle;
  bsp_idle->state = RUNNING;
  per_cpu_tss[0].rsp0 = bsp_idle->k_stack_top;
  cpu_locals[0].tss_rsp0 = bsp_idle->k_stack_top;

  sti();

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
