#include "arch/x86/paging.h"
#include "arch/x86/lib.h"

// ===================== GDT =====================
static gdt_entry_t gdt[6];
static gdt_ptr_t gdt_reg;
tss_t tss;

static void set_gdt_gate(int n, uint32_t base, uint32_t limit, uint8_t access,
                         uint8_t gran) {
  gdt[n].limit_low = L16(limit);
  gdt[n].base_low = L16(base);
  gdt[n].base_middle = (base >> 16) & 0xFF;
  gdt[n].access = access;
  gdt[n].granularity = ((gran & 0x0F) << 4) | ((limit >> 16) & 0x0F);
  gdt[n].base_high = (base >> 24) & 0xFF;
}

static void set_gdt() {
  gdt_reg.base = (uint32_t)&gdt;
  gdt_reg.limit = sizeof(gdt) - 1;
  __asm__ volatile("lgdt (%0)" : : "r"(&gdt_reg));
  __asm__ volatile(
      "movw $0x10, %%ax\n"
      "movw %%ax, %%ds\n"
      "movw %%ax, %%es\n"
      "movw %%ax, %%fs\n"
      "movw %%ax, %%gs\n"
      "movw %%ax, %%ss\n"
      "ljmp $0x08, $1f\n"
      "1:\n" :::"eax");
}

extern "C" const uint8_t stack_bottom[8192];

void gdt_init() {
  set_gdt_gate(0, 0, 0, 0, 0);                   // null segment
  set_gdt_gate(1, 0, 0xFFFFFFFF, 0x9A, 0x0C);    // code: ER, ring0, 4K granularity, 32-bit
  set_gdt_gate(2, 0, 0xFFFFFFFF, 0x92, 0x0C);    // data: RW, ring0, 4K granularity, 32-bit
  set_gdt_gate(3, 0, 0xFFFFFFFF, 0xFA, 0x0C);    // user code: ER, ring3, 4K granularity, 32-bit
  set_gdt_gate(4, 0, 0xFFFFFFFF, 0xF2, 0x0C);    // user data: RW, ring3, 4K granularity, 32-bit
  set_gdt_gate(5, (uint32_t)&tss, sizeof(tss_t) - 1, 0x89, 0x0); // TSS, Available, byte granularity
  set_gdt();

  // Initialize TSS
  tss.ss0 = 0x10;
  tss.esp0 = (uint32_t)&stack_bottom + 8192;
  tss.iomap_base = sizeof(tss_t);
  __asm__ volatile("ltr %w0" :: "r"(TSS_SEL));
}

// ===================== 页表 =====================
__attribute__((aligned(4096))) uint32_t page_directory[1024];
__attribute__((aligned(4096))) uint32_t page_table[1024];

// ===================== enable_page =====================
// 在物理地址运行，由 start.S 调用
extern "C" void enable_page() {
  // GOTOFF 自动给出物理地址（因 enable_page 在物理地址运行）

  // 清零 PD 和 PT
  for (int i = 0; i < 1024; i++) {
    page_directory[i] = 0;
    page_table[i] = 0;
  }

  // 填充 PT：物理 0x0-0x3FFFFF → 4KB页，present + writable
  for (int i = 0; i < 1024; i++) {
    page_table[i] = (i * 4096) | 0x03;
  }

  // PD[0] = PT 物理地址 | flags（identity map: virt 0-4MB → phys 0-4MB）
  page_directory[0] = ((uintptr_t)page_table) | 0x03;

  // PD[768] = PT 物理地址 | flags（higher-half: virt 0xC0000000-0xC0400000 → phys 0-4MB）
  page_directory[768] = ((uintptr_t)page_table) | 0x03;

  // 启用分页
  __asm__ volatile(
      "movl %0, %%cr3\n"
      "movl %%cr0, %%eax\n"
      "orl $0x80000000, %%eax\n"
      "movl %%eax, %%cr0\n"
      :
      : "r"((uintptr_t)page_directory)
      : "eax", "memory");
}

// ===================== 全局变量定义 =====================
uintptr_t device_vma_base = 0;

// ===================== Bump 分配器 =====================
static uintptr_t bump_next_phys;
static bool bump_disabled = false;

void bump_init_phys(uintptr_t start) {
  bump_next_phys = ALIGN_UP(start, PAGE_SIZE);
}

void *bump_alloc(size_t size) {
  if (bump_disabled) {
    // Can't format a nice message without printf; just halt
    __asm__ volatile("cli; hlt");
  }
  uintptr_t phys = bump_next_phys;
  bump_next_phys += ALIGN_UP(size, PAGE_SIZE);
  return (void *)(phys + VMA_BASE);
}

void bump_disable() { bump_disabled = true; }

// ===================== extend_mapping =====================
// 扩展 higher-half 映射：为超出初始 4MB 的物理 RAM 块分配 PT
void extend_mapping(uint64_t max_phys_addr) {
  size_t max_4mb_block = (size_t)(max_phys_addr / 0x400000);
  for (size_t n = 1; n <= max_4mb_block; n++) {
    uint32_t *pt = (uint32_t *)bump_alloc(4096);
    uintptr_t pt_phys = PHY_ADDR((uintptr_t)pt);

    uint32_t phys_base = (uint32_t)(n * 0x400000);
    for (int i = 0; i < 1024; i++) {
      pt[i] = (phys_base + i * 4096) | 0x03;
    }

    page_directory[768 + n] = pt_phys | 0x03;
  }

  // 设备映射区
  device_vma_base =
      ALIGN_UP(VMA_BASE + (uintptr_t)max_phys_addr, 0x400000);
}

// ===================== flush_tlb =====================
void flush_tlb() {
  __asm__ volatile("movl %0, %%cr3\n" ::"r"(PHY_ADDR((uintptr_t)page_directory))
                   : "memory");
}

// ===================== bump allocator query =====================
// 供 kernel/mem/alloc.cc 查询 bump 分配的物理结束地址
extern "C" uintptr_t bump_end_phys() {
  return bump_next_phys;
}
