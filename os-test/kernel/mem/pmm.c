#include "os-test/kernel/mem/pmm.h"
#include "os-test/drivers/screen.h"
#include "os-test/kernel/mem/memlayout.h"
#include "os-test/kernel/mem/mmu.h"
#include "os-test/utils/os_utils.h"
#include "os-test/utils/x86.h"
#include <stdint.h>

/* *
 * Task State Segment:
 *
 * The TSS may reside anywhere in memory. A special segment register called
 * the Task Register (TR) holds a segment selector that points a valid TSS
 * segment descriptor which resides in the GDT. Therefore, to use a TSS
 * the following must be done in function gdt_init:
 *   - create a TSS descriptor entry in GDT
 *   - add enough information to the TSS in memory as needed
 *   - load the TR register with a segment selector for that segment
 *
 * There are several fileds in TSS for specifying the new stack pointer when a
 * privilege level change happens. But only the fields SS0 and ESP0 are useful
 * in our os kernel.
 *
 * The field SS0 contains the stack segment selector for CPL = 0, and the ESP0
 * contains the new ESP value for CPL = 0. When an interrupt happens in
 * protected mode, the x86 CPU will look in the TSS for SS0 and ESP0 and load
 * their value into SS and ESP respectively.
 * */
static struct taskstate ts = {0};

/* *
 * Global Descriptor Table:
 *
 * The kernel and user segments are identical (except for the DPL). To load
 * the %ss register, the CPL must equal the DPL. Thus, we must duplicate the
 * segments for the user and the kernel. Defined as follows:
 *   - 0x0 :  unused (always faults -- for trapping NULL far pointers)
 *   - 0x8 :  kernel code segment
 *   - 0x10:  kernel data segment
 *   - 0x18:  user code segment
 *   - 0x20:  user data segment
 *   - 0x28:  defined for tss, initialized in gdt_init
 * */
static struct segdesc gdt[] = {
    SEG_NULL,
    [SEG_KTEXT] = SEG(STA_X | STA_R, 0x0, 0xFFFFFFFF, DPL_KERNEL),
    [SEG_KDATA] = SEG(STA_W, 0x0, 0xFFFFFFFF, DPL_KERNEL),
    [SEG_UTEXT] = SEG(STA_X | STA_R, 0x0, 0xFFFFFFFF, DPL_USER),
    [SEG_UDATA] = SEG(STA_W, 0x0, 0xFFFFFFFF, DPL_USER),
    [SEG_TSS] = SEG_NULL,
};

static struct pseudodesc gdt_pd = {sizeof(gdt) - 1, (uint32_t)gdt};

/* *
 * lgdt - load the global descriptor table register and reset the
 * data/code segement registers for kernel.
 * */
static inline void lgdt(struct pseudodesc *pd) {
  asm volatile("lgdt (%0)" ::"r"(pd));
  asm volatile("movw %%ax, %%gs" ::"a"(USER_DS));
  asm volatile("movw %%ax, %%fs" ::"a"(USER_DS));
  asm volatile("movw %%ax, %%es" ::"a"(KERNEL_DS));
  asm volatile("movw %%ax, %%ds" ::"a"(KERNEL_DS));
  asm volatile("movw %%ax, %%ss" ::"a"(KERNEL_DS));
  // reload cs
  asm volatile("ljmp %0, $1f\n 1:\n" ::"i"(KERNEL_CS));
}

extern char bootstack[], bootstacktop[];
/* gdt_init - initialize the default GDT and TSS */
static void gdt_init(void) {
  ts.ts_esp0 = (uintptr_t)bootstacktop;
  ts.ts_ss0 = KERNEL_DS;

  // initialize the TSS filed of the gdt
  gdt[SEG_TSS] = SEG16(STS_T32A, (uint32_t)&ts, sizeof(ts), DPL_KERNEL);
  gdt[SEG_TSS].sd_s = 0;

  // reload all segment registers
  lgdt(&gdt_pd);

  // load the TSS
  ltr(GD_TSS);
}

static void fill_pde(struct pde_t *pde, unsigned int offset, void *addr,
                     size_t len) {
  uintptr_t phys_addr = (uintptr_t)addr;
  size_t num_pages = len / PAGE_SIZE;
  unsigned int pde_index = offset / PTE_MAX_ENTRIES;
  unsigned int pte_index = offset % PTE_MAX_ENTRIES;
  struct pde_t *current_pde = &pde[pde_index];
  struct pte_t *page_table = (struct pte_t *)(current_pde->base_addr << 12);
  for (size_t i = 0; i < num_pages; i++) {
    page_table[pte_index].present = 1;
    page_table[pte_index].writable = 1;
    page_table[pte_index].base_addr = phys_addr >> 12;
    phys_addr += PAGE_SIZE;
    pte_index++;
    if (pte_index >= PTE_MAX_ENTRIES) {
      pte_index = 0; // 重置 PTE 索引
      pde_index++;   // 切换到下一个 PDE
      page_table = (struct pte_t *)(pde[pde_index].base_addr << 12);
    }
  }
}

// This function is call in kernel_entry.S.
void pde_init(struct pde_t *kernel_pde) {
  uint32_t pde_idx = GET_PDE_INDEX(KERNEL_BASE_ADDR);
  kernel_pde[pde_idx].present = 1;
  kernel_pde[pde_idx].writable = 1;
  kernel_pde[pde_idx].base_addr = (uintptr_t)kernel_pde >> 12;
  // We run this func on phy_addr, so we need map vaddr to paddr and call.
  void (*fn)(struct pde_t *, unsigned int, void *, size_t) =
      (void (*)(struct pde_t *, unsigned int, void *, size_t))(
          (uintptr_t)fill_pde & 0xFFFFFF);
  fn(kernel_pde + pde_idx, 0, 0, 3 << 20 /* 3M */);
  // Before call kernel_init, we run on phy_addr,
  // so we also need to map 0-4M vaddr to 0-4M paddr.
  pde_idx = 0;
  kernel_pde[pde_idx].present = 1;
  kernel_pde[pde_idx].writable = 1;
  kernel_pde[pde_idx].base_addr = (uintptr_t)kernel_pde >> 12;
}

/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
  struct e820map *memmap = (struct e820map *)(0x90000);

  kprint("e820map:\n");
  kprint("num: ");
  kprint_int(memmap->nr_map);
  kprint("\n");
  int i;
  for (i = 0; i < memmap->nr_map; i++) {
    uint64_t begin = memmap->map[i].addr, end = begin + memmap->map[i].size;
    kprint("  memory size: ");
    kprint_int64(memmap->map[i].size);
    kprint(", begin: ");
    kprint_hex64(begin);
    kprint(", end:");
    kprint_hex64(end);
    kprint(", type:");
    kprint_int(memmap->map[i].type);
    kprint("\n");
  }
  gdt_init();
}
