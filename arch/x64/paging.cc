#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"

// ===================== GDT (物理地址阶段) =====================
// start.S 调用 gdt_init() 时仍在物理地址运行，
// 必须用物理地址的 GDT。smp_init_cpu 在虚拟地址阶段调用。

static gdt_entry_t gdt[8];
static gdt_ptr_t gdt_reg;

static void set_gdt_gate(int n, uint32_t base, uint32_t limit, uint8_t access,
                         uint8_t gran) {
  gdt[n].limit_low = L16(limit);
  gdt[n].base_low = L16(base);
  gdt[n].base_middle = (base >> 16) & 0xFF;
  gdt[n].access = access;
  gdt[n].granularity = ((gran & 0x0F) << 4) | ((limit >> 16) & 0x0F);
  gdt[n].base_high = (base >> 24) & 0xFF;
}

static void set_tss_gate(int n, uint64_t base, uint32_t limit) {
  gdt[n].limit_low = L16(limit);
  gdt[n].base_low = L16(base);
  gdt[n].base_middle = (base >> 16) & 0xFF;
  gdt[n].access = 0x89;
  gdt[n].granularity = 0x00;
  gdt[n].base_high = (base >> 24) & 0xFF;
  uint32_t *hi = (uint32_t *)&gdt[n + 1];
  hi[0] = (uint32_t)(base >> 32);
  hi[1] = 0;
}

extern "C" void reload_cs(void);

// 临时 TSS，仅供物理地址阶段使用
static tss_t boot_tss;

extern "C" const uint8_t stack_bottom[8192]
    __attribute__((aligned(16))) = {0};

void gdt_init() {
  // 物理地址阶段：使用静态 GDT（RIP-relative 自动给出物理地址）
  set_gdt_gate(0, 0, 0, 0, 0);
  set_gdt_gate(1, 0, 0, 0x9A, 0x02);    // kernel code64 (L=1)
  set_gdt_gate(2, 0, 0, 0x92, 0x00);    // kernel data
  set_gdt_gate(3, 0, 0, 0xFA, 0x00);    // user code32 compat (DPL=3, STAR[63:48] base)
  set_gdt_gate(4, 0, 0, 0xF2, 0x00);    // user data (DPL=3)
  set_gdt_gate(5, 0, 0, 0xFA, 0x02);    // user code64 (DPL=3, L=1)
  set_tss_gate(6, (uint64_t)&boot_tss, sizeof(tss_t) - 1);

  gdt_reg.base = (uint64_t)&gdt;
  gdt_reg.limit = sizeof(gdt) - 1;
  lgdt(&gdt_reg);
  __asm__ volatile(
      "movw $0x10, %%ax\n"
      "movw %%ax, %%ds\n"
      "movw %%ax, %%es\n"
      "movw %%ax, %%fs\n"
      "movw %%ax, %%gs\n"
      "movw %%ax, %%ss\n" ::: "ax");
  reload_cs();

  boot_tss.rsp0 = (uint64_t)&stack_bottom + 8192;
  boot_tss.iomap_base = 104;
  // Initialize IOPM to deny all ports
  for (int i = 0; i < IOPM_SIZE; i++)
      boot_tss.iopm[i] = 0xFF;
  ltr(TSS_SEL);
}

// ===================== 页表 =====================
// stub.S 在物理地址阶段设置这些页表，此处定义 BSS
__attribute__((aligned(4096))) uint64_t pml4[512];
__attribute__((aligned(4096))) uint64_t pdpt_ident[512];
__attribute__((aligned(4096))) uint64_t pdpt_hh[512];
__attribute__((aligned(4096))) uint64_t page_dir[512];

// ===================== enable_paging =====================
// 物理地址阶段，由 start.S _start 调用
// 注意：此时 higher-half 不可访问，必须通过物理地址指针操作页表
// CR3 加载后 identity map + higher-half 生效，才能访问 VMA
// 返回: rax = &gdtr(栈/物理地址), rdx = &far_ptr(栈/物理地址)
extern "C" __attribute__((noinline)) void enable_paging(boot_info *bi_phys) {
  (void)bi_phys;

  // === 构建页表 (2MB huge pages) ===
  // 物理地址运行时 RIP-relative 直接给出物理地址
  uint64_t *pml4_p   = pml4;
  uint64_t *pdpt_i_p = pdpt_ident;
  uint64_t *pdpt_h_p = pdpt_hh;
  uint64_t *pd_p     = page_dir;

  uint64_t pml4_phys   = (uint64_t)pml4_p;
  uint64_t pdpt_i_phys = (uint64_t)pdpt_i_p;
  uint64_t pdpt_h_phys = (uint64_t)pdpt_h_p;
  uint64_t pd_phys     = (uint64_t)pd_p;

  // 清零（手动循环，不用 __builtin_memset）
  for (int i = 0; i < 512; i++) pml4_p[i] = 0;
  for (int i = 0; i < 512; i++) pdpt_i_p[i] = 0;
  for (int i = 0; i < 512; i++) pdpt_h_p[i] = 0;
  for (int i = 0; i < 512; i++) pd_p[i] = 0;

  pml4_p[0]     = pdpt_i_phys | PTE_PRESENT | PTE_RW;
  pml4_p[511]   = pdpt_h_phys | PTE_PRESENT | PTE_RW;
  pdpt_i_p[0]   = pd_phys | PTE_PRESENT | PTE_RW;
  pdpt_h_p[510] = pd_phys | PTE_PRESENT | PTE_RW;

  for (int i = 0; i < 512; i++) {
    pd_p[i] = ((uint64_t)i << 21) | PTE_PRESENT | PTE_RW | PTE_PS;
  }

  // 加载 CR3 — 此后 identity map + higher-half 均生效
  load_cr3(pml4_phys);
}

// ===================== 全局变量定义 =====================
boot_info g_boot_info;
uintptr_t device_vma_base = 0;

// ===================== Bump 分配器 =====================
static uintptr_t bump_next_phys;
static bool bump_disabled = false;

void bump_init_phys(uintptr_t start) {
  bump_next_phys = ALIGN_UP(start, PAGE_SIZE);
}

void *bump_alloc(size_t size) {
  if (bump_disabled) {
    halt();
  }
  uintptr_t phys = bump_next_phys;
  bump_next_phys += ALIGN_UP(size, PAGE_SIZE);
  return (void *)(phys + VMA_BASE);
}

void bump_disable() { bump_disabled = true; }

// ===================== extend_mapping =====================
// 扩展 higher-half 映射：为超出初始 1GB 的物理 RAM 分配 PDPT+PD
// 0xFFFFFFFF80000000 的页表索引:
//   PML4 index = 511
//   PDPT index = 510
//   所以 higher-half 从 PDPT_hh[510] 开始
//   后续 1GB 块使用 PDPT_hh[511], PDPT_hh[512-overflow]...
//   注意: PDPT_hh[511] 已被 PML4 自映射占用时需要换 PDPT
void extend_mapping(uint64_t max_phys_addr) {
  // 计算需要多少个 1GB 块
  size_t max_1gb_block = (size_t)(max_phys_addr / 0x40000000);

  // stub 已设置 PDPT_hh[510] → PD (第一个1GB)
  // PDPT_hh[511] 将用于第二个1GB
  // n >= 2 的 1GB 块需要新的 PDPT 页，链接到 PML4[510]
  // 因为 PML4[511] + PDPT_hh 仅覆盖索引 510-511（2GB 虚拟地址空间）

  // PML4[510] 对应虚拟地址 0xFFFFFFFF00000000 起
  // 用于扩展 higher-half 映射（物理 2GB 以上）
  static uint64_t *pdpt_extra = nullptr;

  for (size_t n = 1; n <= max_1gb_block; n++) {
    // 分配 PD (4KB)
    uint64_t *pd = (uint64_t *)bump_alloc(4096);
    uintptr_t pd_phys = PHY_ADDR((uintptr_t)pd);

    // 填充 PD: 512个 2MB huge pages 映射物理 n*1GB 到 (n+1)*1GB
    uint64_t phys_base = (uint64_t)n * 0x40000000;
    for (int i = 0; i < 512; i++) {
      pd[i] = (phys_base + (uint64_t)i * PAGE_SIZE_2M) | PTE_PRESENT | PTE_RW | PTE_PS;
    }

    // identity map: PDPT_ident[n] = PD
    pdpt_ident[n] = pd_phys | PTE_PRESENT | PTE_RW;

    // higher-half map:
    //   n=1: PDPT_hh[511] (第二个1GB, 虚拟地址 0xFFFFFFFFC0000000)
    //   n>=2: 分配额外 PDPT, 链接到 PML4[510]
    if (n == 1) {
      pdpt_hh[511] = pd_phys | PTE_PRESENT | PTE_RW;
    } else {
      if (!pdpt_extra) {
        // 分配扩展 PDPT 页
        pdpt_extra = (uint64_t *)bump_alloc(4096);
        uintptr_t pdpt_phys = PHY_ADDR((uintptr_t)pdpt_extra);
        for (int i = 0; i < 512; i++)
          pdpt_extra[i] = 0;
        // PML4[510] 映射虚拟地址 0xFFFFFFFF00000000 起
        pml4[510] = pdpt_phys | PTE_PRESENT | PTE_RW;
      }
      // PDPT 索引: 第 n 个 1GB 块(n>=2)映射到 pdpt_extra[n-2]
      pdpt_extra[n - 2] = pd_phys | PTE_PRESENT | PTE_RW;
    }
  }

  // 设备映射区
  device_vma_base =
      ALIGN_UP(VMA_BASE + (uintptr_t)max_phys_addr, 0x40000000);
}

// ===================== flush_tlb =====================
void flush_tlb() {
  load_cr3(PHY_ADDR((uintptr_t)pml4));
}

// ===================== bump allocator query =====================
extern "C" uintptr_t bump_end_phys() {
  return bump_next_phys;
}

// ===================== NX bit enable =====================
// Set CR4.NXDE (bit 5) and EFER.NXE (bit 11) to enable no-execute pages.
extern "C" void enable_nx() {
  // Enable CR4.NXDE
  uint64_t cr4 = read_cr4();
  cr4 |= (1ULL << 5);
  write_cr4(cr4);

  // Enable EFER.NXE
  uint64_t efer = rdmsr(MSR_EFER);
  efer |= EFER_NXE;
  wrmsr(MSR_EFER, efer);
}
