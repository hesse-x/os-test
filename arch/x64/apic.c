/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "arch/x64/apic.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/acpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include <stddef.h>

void __iomem *lapic_vaddr = NULL;
void __iomem *ioapic_vaddr = NULL;
uint32_t lapic_timer_ticks_calibrated = 0;

// TSC calibration results
uint64_t tsc_freq = 0;        // TSC ticks per second
uint64_t tsc_per_ms = 0;      // TSC ticks per millisecond
static uint64_t tsc_base = 0; // TSC value at boot baseline

// Monotonic nanosecond clock since boot (TSC-based).
// tsc_offset corrects this CPU's rdtsc onto the BSP timebase: under KVM the
// INIT IPI at AP bringup resets the AP's TSC (TCG doesn't), so per-CPU rdtsc
// values differ by an arbitrary offset (see tsc_sync_ap in smp.c).
uint64_t sched_clock() {
  uint64_t now = rdtsc64() + (uint64_t)get_cpu_local()->tsc_offset;
  uint64_t delta = now - tsc_base;
  // Split to avoid overflow: delta * 1e9 overflows uint64_t after ~7s at 2.5GHz
  uint64_t sec = delta / tsc_freq;
  uint64_t rem = delta % tsc_freq;
  return sec * 1000000000ULL + rem * 1000000000ULL / tsc_freq;
}

// ===================== I/O APIC =====================
void ioapic_set_irq(uint32_t gsi, uint8_t vector, uint32_t apic_id, bool masked,
                    bool level_triggered, bool active_low) {
  uint32_t reg = IOAPIC_REDIR_OFFSET(gsi);
  uint64_t entry =
      IOAPIC_DELIVERY_FIXED | IOAPIC_DEST_MODE_PHYS |
      (level_triggered ? IOAPIC_TRIGGER_LEVEL : IOAPIC_TRIGGER_EDGE) |
      (active_low ? IOAPIC_POLARITY_LOW : IOAPIC_POLARITY_HIGH) |
      (uint64_t)vector;
  if (masked)
    entry |= IOAPIC_INTR_MASKED;
  entry |= (uint64_t)apic_id << 56;

  ioapic_write(reg, (uint32_t)entry);
  ioapic_write(reg + 1, (uint32_t)(entry >> 32));
}

// ===================== Calibration =====================
// TSC frequency via CPUID leaf 0x15 (exact under KVM -cpu host).
// Returns 0 when unavailable (TCG qemu64 has no leaf 0x15).
static uint64_t calibrate_tsc_cpuid(void) {
  uint32_t eax, ebx, ecx, edx;
  __asm__ volatile("cpuid"
                   : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "a"(0));
  if (eax < 0x15)
    return 0;
  __asm__ volatile("cpuid"
                   : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "a"(0x15));
  uint32_t denom = eax, numer = ebx, crystal = ecx;
  printk(LOG_INFO, "calib: CPUID.0x15 denom=%u numer=%u crystal=%u Hz\n", denom,
         numer, crystal);
  if (!denom || !numer || !crystal)
    return 0;
  return (uint64_t)crystal * numer / denom;
}

// TSC frequency via PIT. Mode 0 counts down from the divisor; at terminal
// count the counter wraps to 0xFFFF and keeps decrementing — the wrapped
// range persists for ~55ms, so the poll cannot miss it even when every port
// access is a KVM VM exit (the old mode-4 poll for cur==0 missed the
// momentary zero and exited at a later wrap crossing, inflating tsc_freq
// ~13x under KVM). Elapsed PIT counts are recovered exactly from the wrapped
// value, so the measured window is precise regardless of poll latency.
static uint64_t calibrate_tsc_pit(void) {
  const uint16_t divisor = 11932; // 10.000ms at 1.193182MHz
  outb(0x43, 0x30);               // channel 0, lobyte/hibyte, mode 0
  outb(0x40, divisor & 0xFF);
  outb(0x40, (divisor >> 8) & 0xFF);
  uint64_t t0 = rdtsc64();
  for (;;) {
    outb(0x43, 0x00); // latch — freezes the count at this instant
    uint64_t t1 = rdtsc64();
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    uint16_t cur = (uint16_t)((hi << 8) | lo);
    if (cur > divisor) { // wrapped past terminal count
      uint32_t pit_counts = (uint32_t)divisor + (0xFFFF - cur) + 1;
      return (t1 - t0) * 1193182ULL / pit_counts;
    }
  }
}

static uint64_t calibrate_tsc(void) {
  uint64_t freq = calibrate_tsc_cpuid();
  if (freq == 0) {
    freq = calibrate_tsc_pit();
    printk(LOG_INFO, "calib: TSC %lu Hz (PIT)\n", freq);
  } else {
    printk(LOG_INFO, "calib: TSC %lu Hz (CPUID.0x15)\n", freq);
  }
  return freq;
}

// LAPIC timer ticks per 10ms, measured against a TSC busy-wait window.
// No port I/O, so KVM VM-exit latency can't distort the window; sharing the
// TSC timebase with sched_clock keeps the timer and the clock consistent.
static uint32_t calibrate_lapic_timer() {
  lapic_write(LAPIC_TIMER_DCR, 0x0B); // divide by 1
  lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);
  uint64_t start = rdtsc64();
  while (rdtsc64() - start < tsc_freq / 100) // 10ms
    __asm__ volatile("pause");
  return 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);
}

// ===================== APIC MMIO mapping =====================
// Map APIC MMIO region into the higher-half virtual address space.
// Uses the device_vma_base area and pdpt_hh, same pattern as fb.cc.
static void map_apic_mmio(uint64_t lapic_phys, uint64_t ioapic_phys) {
  // Both LAPIC (0xFEE00000) and IOAPIC (0xFEC00000) fit in the
  // 0xFEC00000-0xFEE00000+4K range. Align to 2MB boundaries.
  uint64_t region_start = lapic_phys < ioapic_phys ? lapic_phys : ioapic_phys;
  region_start &= ~0x1FFFFFULL; // 2MB align down
  uint64_t region_end =
      (lapic_phys > ioapic_phys ? lapic_phys : ioapic_phys) + 0x1000;
  region_end = (region_end + 0x1FFFFF) & ~0x1FFFFFULL; // 2MB align up
  size_t num_2mb = (region_end - region_start) / 0x200000;

  // Find a free PDPT_hh slot (search all 512 entries)
  int pdpt_idx = -1;
  for (int i = 511; i >= 0; i--) {
    if (pdpt_hh[i] == 0) {
      pdpt_idx = i;
      break;
    }
  }
  if (pdpt_idx < 0) {
    printk(LOG_ERROR, "apic: no free PDPT_hh slot\n");
    halt();
  }

  // Allocate and fill a PD for the APIC MMIO region.
  // Use bfc_alloc_page_data (the tracked page allocator), NOT bump_alloc:
  // init_mem already built bfc_free_list before apic_init() runs, so a
  // bump_alloc'd PD page would stay PAGE_FREE in frames[] and could be
  // re-handed out by bfc_alloc_page later (e.g. kasan_init allocates shadow
  // PT pages) — that page then gets zeroed, silently deleting the APIC MMIO
  // PTEs and causing a #PF on the next lapic_read (seen in smp_boot_aps).
  uint64_t *pd = (uint64_t *)bfc_alloc_page_data(1);
  if (!pd) {
    printk(LOG_ERROR, "apic: failed to allocate MMIO PD page\n");
    halt();
  }
  uintptr_t pd_phys = (__force uintptr_t)PHY_ADDR((uintptr_t)pd);
  for (int i = 0; i < 512; i++)
    pd[i] = 0;

  // Fill PD with 2MB pages, UC for APIC MMIO
  for (size_t n = 0; n < num_2mb; n++) {
    pd[n] =
        (region_start + n * 0x200000) | PTE_PRESENT | PTE_RW | PTE_PS | PTE_UC;
  }

  pdpt_hh[pdpt_idx] = pd_phys | PTE_PRESENT | PTE_RW;

  // Compute virtual address for this PDPT slot.
  // pdpt_hh is the higher-half PDPT under PML4[511] (base = VMA_BASE), so
  // pdpt_hh[i] maps virtual VMA_BASE + i * 1GB (single continuous span; the
  // direct map occupies pdpt_hh[0..63], device MMIO claims top-down slots
  // like this one). The formula below is PML4[511] base | (i << 30).
  uint64_t vma =
      (0xFFFFULL << 48) | (511ULL << 39) | ((uint64_t)pdpt_idx << 30);
  // Account for device_vma_base tracking (must be within this PDPT slot's
  // range) The PD starts at the beginning of the PDPT slot's 1GB region
  uint64_t apic_vma = vma; // PD entry 0 = start of this 1GB region
  device_vma_base = apic_vma + num_2mb * 0x200000;

  flush_tlb();

  lapic_vaddr =
      (void __iomem __force *)(apic_vma + (lapic_phys - region_start));
  ioapic_vaddr =
      (void __iomem __force *)(apic_vma + (ioapic_phys - region_start));
}

// Send reschedule IPI to target CPU (Fixed delivery, physical destination,
// vector 0xec)
__attribute__((no_sanitize("kernel-address"))) void
lapic_send_reschedule(int target_cpu) {
  uint32_t apic_id = cpu_locals[target_cpu].apic_id;
  while (lapic_read(LAPIC_ICR_LOW) & 0x1000) // wait delivery idle
    __asm__ volatile("pause");
  lapic_write(LAPIC_ICR_HIGH, (uint64_t)apic_id << 24);
  // Fixed IPI, physical destination, vector = RESCHEDULE_VECTOR (0xec)
  lapic_write(LAPIC_ICR_LOW, 0x00004000 | RESCHEDULE_VECTOR);
}

// ===================== APIC init =====================
void apic_init() {
  uint64_t lapic_phys = g_madt.lapic_base;
  uint64_t ioapic_phys = g_madt.ioapic_base;
  if (lapic_phys == 0)
    lapic_phys = 0xFEE00000;
  if (ioapic_phys == 0)
    ioapic_phys = 0xFEC00000;

  // 1. Map APIC MMIO
  map_apic_mmio(lapic_phys, ioapic_phys);
  cpu_locals[0].lapic_base = lapic_vaddr;

  // 2. Enable LAPIC globally via IA32_APIC_BASE MSR
  uint64_t msr = rdmsr(MSR_IA32_APIC_BASE);
  msr |= APIC_BASE_ENABLE;
  wrmsr(MSR_IA32_APIC_BASE, msr);

  // 3. Software-enable LAPIC via Spurious Interrupt Vector Register
  lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFF);

  // 3b. Clear Task Priority Register — TPR determines the minimum priority
  // class (vector >> 4) the LAPIC will deliver. If left at a non-zero value
  // (e.g. set by UEFI firmware during boot services), all MSI/MSI-X vectors
  // in the 64–95 range (priority classes 4–5) would be silently dropped,
  // while the LAPIC timer at vector 120 (class 7) would still fire — exactly
  // the symptom observed before this fix.
  lapic_write(LAPIC_TPR, 0);

  // 4. Remap PIC (don't fully disable yet — keyboard may still route through
  // PIC)
  outb(0x20, 0x11);
  outb(0xA0, 0x11);
  outb(0x21, 0x20);
  outb(0xA1, 0x28);
  outb(0x21, 0x04);
  outb(0xA1, 0x02);
  outb(0x21, 0x01);
  outb(0xA1, 0x01);
  // Mask all PIC IRQs (interrupts now routed through I/O APIC)
  outb(0x21, 0xFF);
  outb(0xA1, 0xFF);

  // 5. Mask unused LAPIC LVT entries
  lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);

  // 6. Configure I/O APIC: mask all, then unmask timer + keyboard
  uint32_t bsp_apic_id = lapic_read(LAPIC_ID) >> 24;

  uint32_t ver = ioapic_read(IOAPIC_VER);
  int max_redir = ((ver >> 16) & 0xFF) + 1;
  printk(LOG_INFO, "apic: IOAPIC max_redir=%d\n", max_redir);

  for (int i = 0; i < max_redir; i++) {
    ioapic_set_irq(i, 32 + i, bsp_apic_id, true, false, false);
  }
  // PIT timer: ISO override maps IRQ0 → GSI 2, but BSP uses LAPIC timer
  // for scheduling so the PIT I/O APIC interrupt is unneeded. Mask GSI 2
  // to prevent spurious vec 34 interrupts.
  {
    const acpi_iso_override *iso = acpi_find_iso(0);
    uint32_t gsi = iso ? iso->gsi : 0;
    bool level = iso ? iso->level_triggered : false;
    bool low = iso ? iso->active_low : false;
    ioapic_set_irq(gsi, 32 + gsi, bsp_apic_id, true, level, low);
    printk(LOG_INFO, "apic: PIT IRQ0 gsi=%d masked (LAPIC timer used)\n", gsi);
  }
  // Unmask keyboard (ISA IRQ 1 → GSI per ISO)
  {
    const acpi_iso_override *iso = acpi_find_iso(1);
    uint32_t gsi = iso ? iso->gsi : 1;
    bool level = iso ? iso->level_triggered : false;
    bool low = iso ? iso->active_low : false;
    ioapic_set_irq(gsi, 32 + gsi, bsp_apic_id, false, level, low);
    printk(LOG_INFO, "apic: kbd IRQ1 gsi=%d level=%d low=%d\n", gsi, level,
           low);
  }

  // 7. Calibrate TSC (CPUID.0x15 when available, else PIT), then calibrate
  // the LAPIC timer against a TSC-measured window.
  tsc_freq = calibrate_tsc();
  tsc_per_ms = tsc_freq / 1000;
  lapic_timer_ticks_calibrated = calibrate_lapic_timer();

  // Record TSC baseline for sched_clock()
  tsc_base = rdtsc64();

  // Start LAPIC periodic timer
  lapic_write(LAPIC_TIMER_DCR, 0x0B); // divide by 1
  lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_LVT_TIMER_PERIODIC);
  lapic_write(LAPIC_TIMER_ICR, lapic_timer_ticks_calibrated);
  printk(LOG_INFO, "apic: BSP timer started vec=0x%x ticks=%u\n",
         LAPIC_TIMER_VECTOR, lapic_timer_ticks_calibrated);
}
