/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "arch/x64/paging.h"
#include "arch/x64/memlayout.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/efi.h"
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
//
// At entry CR3 still holds the UEFI firmware page table, which identity-maps
// ALL physical RAM. We exploit that to read boot_info (and the EFI memory map
// it points at) BEFORE installing our own page table: every physical address is
// directly readable/writable as an identity virtual address.
//
// We compute the highest physical address that must remain reachable after we
// switch CR3 (every EFI memory descriptor, regardless of type — covers RAM,
// ACPI tables, loader data holding boot_info/mmap/init.elf), then build a
// single direct map covering [0, max_map_addr) with 2MB huge pages, in both
// identity and higher-half form. Only after that do we load_cr3, so the
// higher-half boot_info copy in _entry64 (which may live at ~2GB on large-RAM
// machines) and the mmap walk in init_mem never hit an unmapped page.
//
// Page-table pages come from a tiny early bump allocator carved out of physical
// RAM starting at the kernel image end. Its final offset is published via
// early_bump_end so init_mem's bump allocator can resume past it without
// colliding with the page tables we allocate here.
__attribute__((noinline, no_sanitize("kernel-address"))) void
enable_paging(boot_info *bi_phys) {
  // === Early bump allocator (physical-address phase) ===
  // We execute at physical addresses before load_cr3, so RIP-relative
  // addressing of a kernel-image symbol yields its PHYSICAL address directly —
  // (uintptr_t)kernel_end is already physical, NOT the higher-half VMA, so we
  // must NOT subtract VMA_BASE here. Page-align and grow upward.
  //
  // NOTE: no nested functions / trampolines here — they need a writable+-
  // executable trampoline on the stack and a static-chain calling convention,
  // neither valid in this bare physical phase. Inline the PT-page allocation.
  uintptr_t early_bump = ALIGN_UP((uintptr_t)kernel_end, PAGE_SIZE);

  // === Read the EFI memory map (still under UEFI's identity map) ===
  // bi_phys is an identity virtual address == its physical address.
  // Only count real RAM descriptors: OVMF/q35 also report large
  // EfiReservedMemoryType and EfiMemoryMappedIO ranges far above physical
  // RAM (observed: a reserved desc ending at 1 TB), which would otherwise
  // balloon max_map_addr, make n_1gb exceed the identity PDPT (512) and the
  // higher-half span (2 GB), and cause out-of-bounds pdpt_ident[]/
  // pdpt_extra[] writes that corrupt the page tables and triple-fault on
  // load_cr3. MMIO and reserved ranges are never part of the direct map.
  boot_info *bi = bi_phys;
  size_t desc_count = bi->mmap_size / bi->mmap_desc_size;
  uint64_t max_map_addr = 0;
  for (size_t i = 0; i < desc_count; i++) {
    efi_memory_descriptor *desc =
        (efi_memory_descriptor *)(bi->mmap_addr + i * bi->mmap_desc_size);
    switch (desc->type) {
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:
    case EfiConventionalMemory:
    case EfiACPIReclaimMemory:
    case EfiACPIMemoryNVS:
    case EfiPalCode: {
      uint64_t end = desc->physical_start + desc->number_of_pages * 4096;
      if (end > max_map_addr)
        max_map_addr = end;
      break;
    }
    default:
      break; // EfiReserved / EfiUnusable / EfiMemoryMappedIO / IO port space
    }
  }
  if (max_map_addr == 0)
    max_map_addr = 0x40000000; // fallback: 1 GB

  // Number of 1 GB blocks to map (rounded up).
  size_t n_1gb = (size_t)((max_map_addr + 0x40000000 - 1) / 0x40000000);

  // === Allocate page-table pages from the early bump ===
  // Reuse the statically reserved top-level pages; allocate one PD per 1 GB
  // block plus, for the higher-half, one extra PDPT once we exceed 2 GB.
  uint64_t *pml4_p = pml4;
  uint64_t *pdpt_i_p = pdpt_ident;
  uint64_t *pdpt_h_p = pdpt_hh;
  uint64_t *pdpt_extra = NULL;

  for (int i = 0; i < 512; i++) {
    pml4_p[i] = 0;
    pdpt_i_p[i] = 0;
    pdpt_h_p[i] = 0;
  }

  uint64_t pml4_phys = (uint64_t)pml4_p;
  uint64_t pdpt_i_phys = (uint64_t)pdpt_i_p;
  uint64_t pdpt_h_phys = (uint64_t)pdpt_h_p;

  pml4_p[0] = pdpt_i_phys | PTE_PRESENT | PTE_RW;   // identity at VA 0
  pml4_p[511] = pdpt_h_phys | PTE_PRESENT | PTE_RW; // higher-half at VMA_BASE

  // Fill PD for the n-th 1 GB block (n = 0..n_1gb-1): 512 x 2MB huge pages.
  // pd_points: one PD page per block, identity-mapped so writable right now.
  for (size_t n = 0; n < n_1gb; n++) {
    // Carve one 4 KB PD page from the early bump. The pointer is an identity
    // virtual address (== physical address) under UEFI's still-active map.
    uint64_t *pd = (uint64_t *)early_bump;
    early_bump += 4096;
    uint64_t pd_phys = (uint64_t)pd;
    uint64_t phys_base = (uint64_t)n * 0x40000000;
    for (int i = 0; i < 512; i++) {
      pd[i] = (phys_base + (uint64_t)i * PAGE_SIZE_2M) | PTE_PRESENT |
              PTE_RW | PTE_PS;
    }

    // identity: PDPT_ident[n] -> PD
    pdpt_i_p[n] = pd_phys | PTE_PRESENT | PTE_RW;

    // higher-half: block 0 -> PDPT_hh[510], block 1 -> PDPT_hh[511],
    // block 2+ -> PML4[510] -> pdpt_extra[n-2] (PML4[511]'s PDPT only has
    // slots 510/511 free, i.e. 2 GB of higher-half VA).
    if (n == 0) {
      pdpt_h_p[510] = pd_phys | PTE_PRESENT | PTE_RW;
    } else if (n == 1) {
      pdpt_h_p[511] = pd_phys | PTE_PRESENT | PTE_RW;
    } else {
      if (!pdpt_extra) {
        pdpt_extra = (uint64_t *)early_bump;
        early_bump += 4096;
        uint64_t pdpt_extra_phys = (uint64_t)pdpt_extra;
        for (int i = 0; i < 512; i++)
          pdpt_extra[i] = 0;
        pml4_p[510] = pdpt_extra_phys | PTE_PRESENT | PTE_RW;
      }
      pdpt_extra[n - 2] = pd_phys | PTE_PRESENT | PTE_RW;
    }
  }

  // The statically reserved page_dir[] is no longer used for the initial 1 GB
  // (it is superseded by the dynamically allocated PD for block 0). Leave it
  // zeroed; it is harmless BSS.

  // Publish the early bump frontier for init_mem to resume from.
  early_bump_end = early_bump;

  // Load CR3 — UEFI's identity map is replaced by our direct map, which
  // covers all RAM up to max_map_addr in both identity and higher-half form.
  load_cr3(pml4_phys);

  // Program PAT MSR after paging is enabled
  pat_init();
}

// ===================== Global variable definitions =====================
boot_info g_boot_info;
uintptr_t device_vma_base = 0;
// Frontier of the early bump allocator used inside enable_paging (physical
// address). init_mem resumes its own bump allocator here so frames[] and
// friends never overlap the page-table pages allocated during early paging.
uintptr_t early_bump_end = 0;

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
