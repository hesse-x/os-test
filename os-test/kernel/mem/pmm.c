#include "os-test/kernel/mem/pmm.h"
#include "os-test/drivers/screen.h"
#include "os-test/kernel/mem/memlayout.h"
#include "os-test/kernel/mem/mmu.h"
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

/* temporary kernel stack */
uint8_t stack0[1024];

/* gdt_init - initialize the default GDT and TSS */
static void gdt_init(void) {
  // Setup a TSS so that we can get the right stack when we trap from
  // user to the kernel. But not safe here, it's only a temporary value,
  // it will be set to KSTACKTOP in lab2.
  ts.ts_esp0 = (uint32_t)&stack0 + sizeof(stack0);
  ts.ts_ss0 = KERNEL_DS;

  // initialize the TSS filed of the gdt
  gdt[SEG_TSS] = SEG16(STS_T32A, (uint32_t)&ts, sizeof(ts), DPL_KERNEL);
  gdt[SEG_TSS].sd_s = 0;

  // reload all segment registers
  lgdt(&gdt_pd);

  // load the TSS
  ltr(GD_TSS);
}

struct pde page_directory[1024] __attribute__((aligned(4096)));
struct pte page_table[1024] __attribute__((aligned(4096)));

static void page_init() {
  // 初始化页表
  for (int i = 0; i < 1024; i++) {
    page_table[i].present = 1;
    page_table[i].read_write = 1;
    page_table[i].user_supervisor = 0;
    page_table[i].page_write_through = 0;
    page_table[i].page_cache_disable = 0;
    page_table[i].accessed = 0;
    page_table[i].dirty = 0;
    page_table[i].page_attribute_table = 0;
    page_table[i].global = 0;
    page_table[i].available = 0;
    page_table[i].page_frame_base = (i * 4096) >> 12; // 物理地址的高20位
  }

  // 设置页目录项
  page_directory[0].present = 1;
  page_directory[0].read_write = 1;
  page_directory[0].user_supervisor = 0;
  page_directory[0].page_write_through = 0;
  page_directory[0].page_cache_disable = 0;
  page_directory[0].accessed = 0;
  page_directory[0].reserved = 0;
  page_directory[0].page_size = 0;
  page_directory[0].global = 0;
  page_directory[0].available = 0;
  page_directory[0].page_table_base =
      ((uint32_t)page_table) >> 12; // 页表基地址的高20位

  // 加载页目录地址到CR3寄存器
  asm volatile("mov %0, %%cr3" : : "r"((uint32_t)page_directory));

  // 启用分页机制
  uint32_t cr0;
  asm volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 |= 0x80000000; // 设置CR0的PG位
  asm volatile("mov %0, %%cr0" : : "r"(cr0));
}

/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
  //  page_init();

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
