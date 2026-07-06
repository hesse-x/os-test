/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "arch/x64/paging.h"
#include "arch/x64/memlayout.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "utils/macro.h"
#include <stdbool.h>

// ===================== GDT (physical-address phase) =====================
// start.S calls gdt_init() while still running at physical addresses,
// so the GDT must be accessed via physical addresses.  smp_init_cpu is
// called later in the virtual-address phase and uses a separate GDT.

static gdt_entry gdt[8];
static gdt_ptr gdt_reg;

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

void reload_cs(void);

// Temporary TSS for the physical-address phase only
static struct tss_struct boot_tss;

const uint8_t stack_bottom[8192] __attribute__((aligned(16))) = {0};

__attribute__((no_sanitize("kernel-address"))) void gdt_init() {
  // Physical-address phase: use a static GDT (RIP-relative addressing
  // automatically produces physical addresses)
  set_gdt_gate(0, 0, 0, 0, 0);
  set_gdt_gate(1, 0, 0, 0x9A, 0x02); // kernel code64 (L=1)
  set_gdt_gate(2, 0, 0, 0x92, 0x00); // kernel data
  set_gdt_gate(3, 0, 0, 0xFA,
               0x00); // user code32 compat (DPL=3, STAR[63:48] base)
  set_gdt_gate(4, 0, 0, 0xF2, 0x00); // user data (DPL=3)
  set_gdt_gate(5, 0, 0, 0xFA, 0x02); // user code64 (DPL=3, L=1)
  set_tss_gate(6, (uint64_t)&boot_tss, sizeof(struct tss_struct) - 1);

  gdt_reg.base = (uint64_t)&gdt;
  gdt_reg.limit = sizeof(gdt) - 1;
  lgdt(&gdt_reg);
  __asm__ volatile("movw $0x10, %%ax\n"
                   "movw %%ax, %%ds\n"
                   "movw %%ax, %%es\n"
                   "movw %%ax, %%fs\n"
                   "movw %%ax, %%gs\n"
                   "movw %%ax, %%ss\n" ::
                       : "ax");
  reload_cs();

  boot_tss.rsp0 = (uint64_t)&stack_bottom + 8192;
  boot_tss.iomap_base = 104;
  // Initialize IOPM to deny all ports
  for (int i = 0; i < IOPM_SIZE; i++)
    boot_tss.iopm[i] = 0xFF;
  ltr(TSS_SEL);
}

// ===================== Page tables =====================
// Set up by stub.S in the physical-address phase; we define the BSS here.
__attribute__((aligned(4096))) uint64_t pml4[512];
__attribute__((aligned(4096))) uint64_t pdpt_ident[512];
__attribute__((aligned(4096))) uint64_t pdpt_hh[512];
__attribute__((aligned(4096))) uint64_t page_dir[512];

// ===================== enable_paging =====================
// Called from start.S _start in the physical-address phase.
// Note: the higher-half is not yet accessible; page tables must be
// manipulated through physical-address pointers.  Once CR3 is loaded,
// both the identity map and the higher-half become active, making VMA
// addresses accessible.
// Returns: rax = &gdtr (on stack, physical addr), rdx = &far_ptr (stack, phys)
__attribute__((noinline, no_sanitize("kernel-address"))) void
enable_paging(boot_info *bi_phys) {
  (void)bi_phys;

  // === Build page tables (2MB huge pages) ===
  // During physical-address execution, RIP-relative addressing directly
  // yields physical addresses.
  uint64_t *pml4_p = pml4;
  uint64_t *pdpt_i_p = pdpt_ident;
  uint64_t *pdpt_h_p = pdpt_hh;
  uint64_t *pd_p = page_dir;

  uint64_t pml4_phys = (uint64_t)pml4_p;
  uint64_t pdpt_i_phys = (uint64_t)pdpt_i_p;
  uint64_t pdpt_h_phys = (uint64_t)pdpt_h_p;
  uint64_t pd_phys = (uint64_t)pd_p;

  // Zero out page tables (manual loop, avoid __builtin_memset)
  for (int i = 0; i < 512; i++)
    pml4_p[i] = 0;
  for (int i = 0; i < 512; i++)
    pdpt_i_p[i] = 0;
  for (int i = 0; i < 512; i++)
    pdpt_h_p[i] = 0;
  for (int i = 0; i < 512; i++)
    pd_p[i] = 0;

  pml4_p[0] = pdpt_i_phys | PTE_PRESENT | PTE_RW;
  pml4_p[511] = pdpt_h_phys | PTE_PRESENT | PTE_RW;
  pdpt_i_p[0] = pd_phys | PTE_PRESENT | PTE_RW;
  pdpt_h_p[510] = pd_phys | PTE_PRESENT | PTE_RW;

  for (int i = 0; i < 512; i++) {
    pd_p[i] = ((uint64_t)i << 21) | PTE_PRESENT | PTE_RW | PTE_PS;
  }

  // Load CR3 — both the identity map and the higher-half become active
  load_cr3(pml4_phys);

  // Program PAT MSR after paging is enabled
  pat_init();
}

// ===================== Global variable definitions =====================
boot_info g_boot_info;
uintptr_t device_vma_base = 0;

// ===================== Bump allocator =====================
static uintptr_t bump_next_phys;
static bool bump_disabled = false;

__attribute__((no_sanitize("kernel-address"))) void
bump_init_phys(uintptr_t start) {
  bump_next_phys = ALIGN_UP(start, PAGE_SIZE);
}

__attribute__((no_sanitize("kernel-address"))) void *bump_alloc(size_t size) {
  if (bump_disabled) {
    halt();
  }
  uintptr_t phys = bump_next_phys;
  bump_next_phys += ALIGN_UP(size, PAGE_SIZE);
  return (void *)(phys + VMA_BASE);
}

__attribute__((no_sanitize("kernel-address"))) void bump_disable() {
  bump_disabled = true;
}

// ===================== extend_mapping =====================
// Extend the higher-half mapping: allocate PDPT+PD entries for physical
// RAM beyond the initial 1 GB.
// Page table index for 0xFFFFFFFF80000000:
//   PML4 index = 511
//   PDPT index = 510
//   Thus the higher-half starts at PDPT_hh[510]
//   Subsequent 1 GB blocks use PDPT_hh[511], PDPT_hh[512-overflow], ...
//   Note: if PDPT_hh[511] is already occupied by PML4 self-map,
//   a new PDPT page must be allocated.
__attribute__((no_sanitize("kernel-address"))) void
extend_mapping(uint64_t max_phys_addr) {
  // Calculate how many 1 GB blocks are needed
  size_t max_1gb_block = (size_t)(max_phys_addr / 0x40000000);

  // stub already set PDPT_hh[510] → PD (first 1 GB)
  // PDPT_hh[511] covers the second 1 GB
  // n >= 2 blocks require a fresh PDPT page linked to PML4[510]
  // because PML4[511] + PDPT_hh only covers indices 510-511 (2 GB VA space)

  // PML4[510] maps virtual addresses starting at 0xFFFFFFFF00000000
  // Used to extend the higher-half mapping (physical memory above 2 GB)
  static uint64_t *pdpt_extra = NULL;

  for (size_t n = 1; n <= max_1gb_block; n++) {
    // Allocate PD (4 KB)
    uint64_t *pd = (uint64_t *)bump_alloc(4096);
    uintptr_t pd_phys = (__force uintptr_t)PHY_ADDR((uintptr_t)pd);

    // Fill PD: 512 x 2MB huge pages mapping physical n*1GB to (n+1)*1GB
    uint64_t phys_base = (uint64_t)n * 0x40000000;
    for (int i = 0; i < 512; i++) {
      pd[i] = (phys_base + (uint64_t)i * PAGE_SIZE_2M) | PTE_PRESENT | PTE_RW |
              PTE_PS;
    }

    // identity map: PDPT_ident[n] = PD
    pdpt_ident[n] = pd_phys | PTE_PRESENT | PTE_RW;

    // higher-half map:
    //   n=1: PDPT_hh[511] (second 1 GB, virtual 0xFFFFFFFFC0000000)
    //   n>=2: allocate extra PDPT, link to PML4[510]
    if (n == 1) {
      pdpt_hh[511] = pd_phys | PTE_PRESENT | PTE_RW;
    } else {
      if (!pdpt_extra) {
        // Allocate an extended PDPT page
        pdpt_extra = (uint64_t *)bump_alloc(4096);
        uintptr_t pdpt_phys =
            (__force uintptr_t)PHY_ADDR((uintptr_t)pdpt_extra);
        for (int i = 0; i < 512; i++)
          pdpt_extra[i] = 0;
        // PML4[510] maps starting at virtual address 0xFFFFFFFF00000000
        pml4[510] = pdpt_phys | PTE_PRESENT | PTE_RW;
      }
      // PDPT index: the n-th 1 GB block (n>=2) maps to pdpt_extra[n-2]
      pdpt_extra[n - 2] = pd_phys | PTE_PRESENT | PTE_RW;
    }
  }

  // Device mapping region
  device_vma_base = ALIGN_UP(VMA_BASE + (uintptr_t)max_phys_addr, 0x40000000);
}

// ===================== flush_tlb =====================
void flush_tlb() { load_cr3((__force uint64_t)PHY_ADDR((uintptr_t)pml4)); }

// ===================== bump allocator query =====================
__attribute__((no_sanitize("kernel-address"))) uintptr_t bump_end_phys() {
  return bump_next_phys;
}

// ===================== NX bit enable =====================
// Set CR4.NXDE (bit 5) and EFER.NXE (bit 11) to enable no-execute pages.
__attribute__((no_sanitize("kernel-address"))) void enable_nx() {
  // Enable CR4.NXDE
  uint64_t cr4 = read_cr4();
  cr4 |= (1ULL << 5);
  write_cr4(cr4);

  // Enable EFER.NXE
  uint64_t efer = rdmsr(MSR_EFER);
  efer |= EFER_NXE;
  wrmsr(MSR_EFER, efer);
}

// ===================== SSE/FPU enable =====================
// Set CR4.OSFXSR (bit 9) + CR4.OSXMMEXCPT (bit 10).
//   OSFXSR: fxsave/fxrstor save/restore SSE regs; without this flag,
//           user-mode SSE instructions trigger #UD.
//   OSXMMEXCPT: SSE #XM (vector 19) exceptions use the standard IDT path.
// Must be called on every CPU (BSP + AP) during bringup.
__attribute__((no_sanitize("kernel-address"))) void enable_sse() {
  uint64_t cr4 = read_cr4();
  cr4 |= (1ULL << 9) | (1ULL << 10);
  write_cr4(cr4);
}

// ===================== per-CPU capability logging =====================
// Print CR0/CR4/EFER summaries with key bits decoded
// (TS/PE/MP/EM, OSFXSR/OSXMMEXCPT/NXDE, NXE/SCE).
// Called once at the end of BSP (irq_init) and AP (cpu_bringup_common)
// bringup.  Makes "missing required bit" bugs (e.g. AP missing
// CR4.OSFXSR causing #UD) immediately visible.
void log_cpu_caps(const char *tag) {
  uint64_t cr0 = read_cr0();
  uint64_t cr4 = read_cr4();
  uint64_t efer = rdmsr(MSR_EFER);
  printk(LOG_INFO,
         "cpu[%s] caps: CR0=0x%016lX TS=%d PE=%d MP=%d EM=%d | "
         "CR4=0x%016lX OSFXSR=%d OSXMMEXCPT=%d NXDE=%d | "
         "EFER=0x%016lX NXE=%d SCE=%d LME=%d\n",
         tag, cr0, (int)((cr0 >> 3) & 1), (int)((cr0 >> 0) & 1),
         (int)((cr0 >> 1) & 1), (int)((cr0 >> 2) & 1), cr4,
         (int)((cr4 >> 9) & 1), (int)((cr4 >> 10) & 1), (int)((cr4 >> 5) & 1),
         efer, (int)((efer >> 11) & 1), (int)((efer >> 0) & 1),
         (int)((efer >> 8) & 1));
}

// ===================== PAT MSR programming =====================
#define MSR_IA32_PAT 0x277
#define PAT_MSR_LO 0x0001040600010406ULL
#define PAT_MSR_HI 0x0001040600010406ULL

void pat_init(void) {
  // Check CPUID.01H:EDX[16] for PAT support
  uint32_t eax, ebx, ecx, edx;
  __asm__ volatile("cpuid"
                   : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "a"(1));
  if (!(edx & (1 << 16))) {
    // PAT not supported, skip programming
    return;
  }
  wrmsr(MSR_IA32_PAT, (PAT_MSR_HI << 32) | PAT_MSR_LO);
}
