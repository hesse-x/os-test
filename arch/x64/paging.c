/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>

#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/efi.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/log.h"

// Kernel image section boundaries (linker.ld). In the physical-address phase
// their addresses ARE physical (same convention as kernel_end), so we map
// [__text_start, __rodata_end) as read-only 4K pages (code + rodata +
// __ex_table) and [__data_start, kernel_end) as read-write 4K pages, instead
// of one writable 2MB huge page covering the whole image. Without this split a
// wild/DMA write landing in .text silently corrupts kernel instructions
// (observed: syscall_fast_entry 00 00 -> FF FF -> #UD on the next syscall).
extern uint8_t __text_start[];
extern uint8_t __rodata_end[];
extern uint8_t __data_start[];
#include "kernel/xcore/mem/alloc.h"
#include "utils/macro.h"

#include <xos/page.h>

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

// Early kernel stack used during the physical-address phase and the first
// virtual-address steps until per-CPU kernel stacks are set up. Must be
// WRITABLE: call/ret push return addresses here. Declared non-const + zero-
// initialized (no initializer) so it lands in .bss (RW) rather than .rodata
// (RO) — once .text/.rodata are mapped read-only a const stack here would
// #PF on the first push after load_cr3 (triple fault → reboot loop).
uint8_t stack_bottom[8192] __attribute__((aligned(16)));

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
// VA layout (all inside PML4[511] -> pdpt_hh, the canonical 512GB high-half
// window starting at VMA_BASE):
//   pdpt_hh[0..n_1gb-1]  direct map: phys n*1GB -> VMA_BASE + n*1GB
//                         (kernel image at pdpt_hh[0] head: phys 0x100000 ->
//                         VMA_BASE+0x100000 = KERNEL_VMA_BASE, no separate
//                         mapping needed)
//   pdpt_hh[n_1gb..511]  free; device MMIO (APIC/PCI BAR) claims top-down
//                         slots via map_apic_mmio() / device MMIO allocator
// phys_to_virt(phys) = phys + VMA_BASE holds for all [0, max_map_addr), a
// single continuous span with no 64-bit wraparound (VMA_BASE is the canonical
// high-half start, leaving the full 512GB window before any wrap). Capped at
// DIRECT_MAP_MAX_GB so the direct map can never grow past pdpt_hh[63] and
// collide with device MMIO; raise DIRECT_MAP_MAX_GB to extend.
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
  // balloon max_map_addr and make the direct map span absurdly. MMIO and
  // reserved ranges are never part of the direct map.
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

  // Cap the direct map so it never grows past pdpt_hh[DIRECT_MAP_MAX_GB-1] and
  // collides with device MMIO (which claims top-down slots). The current
  // physical-memory targets are far below this; raise DIRECT_MAP_MAX_GB to
  // support more.
  ASSERT(n_1gb <= DIRECT_MAP_MAX_GB);

  // === Allocate page-table pages from the early bump ===
  // Reuse the statically reserved top-level pages; allocate one PD per 1 GB
  // block. The direct map is a single continuous span in pdpt_hh[0..n_1gb-1].
  uint64_t *pml4_p = pml4;
  uint64_t *pdpt_i_p = pdpt_ident;
  uint64_t *pdpt_h_p = pdpt_hh;

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
  // Both identity (pdpt_ident[n]) and higher-half (pdpt_hh[n]) point at the
  // same PD, so phys n*1GB is reachable both as VA n*1GB and VMA_BASE+n*1GB.
  // Block 0's PD also covers the kernel image at phys 0x100000, so the image
  // is reachable at KERNEL_VMA_BASE with no separate mapping.
  for (size_t n = 0; n < n_1gb; n++) {
    // Carve one 4 KB PD page from the early bump. The pointer is an identity
    // virtual address (== physical address) under UEFI's still-active map.
    uint64_t *pd = (uint64_t *)early_bump;
    early_bump += 4096;
    uint64_t pd_phys = (uint64_t)pd;
    uint64_t phys_base = (uint64_t)n * 0x40000000;
    for (int i = 0; i < 512; i++) {
      // The 2MB block at phys 0 (block 0, PD[0]) contains the kernel image.
      // Split it into 4K pages so code/rodata can be mapped read-only (W^X);
      // every other 2MB block stays a writable huge page.
      uint64_t block_phys = phys_base + (uint64_t)i * PAGE_SIZE_2M;
      if (n == 0 && i == 0) {
        // Carve a 4K PT page for the 512 x 4K pages of this 2MB block.
        uint64_t *pt = (uint64_t *)early_bump;
        early_bump += 4096;
        uint64_t pt_phys = (uint64_t)pt;
        uintptr_t ro_start = (uintptr_t)__text_start; // == phys 0x100000
        uintptr_t ro_end = (uintptr_t)__rodata_end;   // end of rodata+ex_table
        for (int j = 0; j < 512; j++) {
          uint64_t page_phys = block_phys + (uint64_t)j * PAGE_SIZE;
          // Read-only for kernel code + rodata + __ex_table; writable
          // everywhere else in the block (data/bss below the image, plus the
          // low 1MB and the tail of the 2MB block past kernel_end). EFER.NXE
          // is not yet enabled here, so we set no NX bit — RO alone blocks the
          // silent instruction-corruption writes we are after.
          uint64_t flags = PTE_PRESENT;
          if (page_phys >= ro_start && page_phys < ro_end)
            flags &= ~PTE_RW; // code/rodata: read-only (no PTE_RW)
          else
            flags |= PTE_RW; // data/bss/rest: writable
          pt[j] = page_phys | flags;
        }
        pd[i] = pt_phys | PTE_PRESENT | PTE_RW; // PD entry -> 4K PT (no PTE_PS)
        continue;
      }
      pd[i] = block_phys | PTE_PRESENT | PTE_RW | PTE_PS;
    }

    // identity: PDPT_ident[n] -> PD
    pdpt_i_p[n] = pd_phys | PTE_PRESENT | PTE_RW;

    // higher-half: PDPT_hh[n] -> PD (single continuous span, no splicing)
    pdpt_h_p[n] = pd_phys | PTE_PRESENT | PTE_RW;
  }

  // The statically reserved page_dir[] is no longer used for the initial 1 GB
  // (it is superseded by the dynamically allocated PD for block 0). Leave it
  // zeroed; it is harmless BSS.

  // Publish the early bump frontier for init_mem to resume from.
  early_bump_end = early_bump;

  // Load CR3 — UEFI's identity map is replaced by our direct map, which
  // covers all RAM up to max_map_addr in both identity and higher-half form.
  load_cr3(pml4_phys);
  outb(0x3F8, 'P'); // diagnostic: paging installed + still executing (RO test)

  // Program PAT MSR after paging is enabled
  pat_init();
  outb(0x3F8, 'p');
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
// Highest bump frontier that init_mem already marked PAGE_USED in frames[]
// (its step 8). post-init_mem bump_alloc consumers (historically apic_init's
// MMIO PD page) advance bump_next_phys past this point, leaving those pages
// PAGE_FREE so bfc_alloc_page can re-hand them out. bump_disable() re-marks
// [init_accounted_end, bump_end_phys()) as USED so the tracked allocator never
// reuses a page still owned by the bump region.
static uintptr_t init_accounted_end = 0;

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
  // Account for any bump pages allocated after init_mem built the free list.
  // Without this, those pages stay PAGE_FREE and bfc_alloc_page can hand them
  // out again, then zero them — silently destroying whatever they backed (the
  // APIC MMIO PD page, which caused a #PF in smp_boot_aps once kasan_init
  // allocated shadow PTs over it). Forward-declared externs from alloc.c.
  uintptr_t end = bump_end_phys();
  if (bfc_frames && end > init_accounted_end) {
    size_t idx_start = PHY_TO_PAGE(ALIGN_UP(init_accounted_end, PAGE_SIZE));
    size_t idx_end = PHY_TO_PAGE(ALIGN_UP(end, PAGE_SIZE));
    for (size_t i = idx_start; i < idx_end && i < total_page_frames; i++) {
      bfc_frames[i].status = PAGE_USED;
      refcount_set(&bfc_frames[i].p_refcount, 1);
    }
  }
  bump_disabled = true;
}

// ===================== flush_tlb =====================
void flush_tlb() { load_cr3((__force uint64_t)PHY_ADDR((uintptr_t)pml4)); }

// ===================== bump allocator query =====================
__attribute__((no_sanitize("kernel-address"))) uintptr_t bump_end_phys() {
  return bump_next_phys;
}

__attribute__((no_sanitize("kernel-address"))) void
bump_set_accounted(uintptr_t end) {
  init_accounted_end = end;
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
