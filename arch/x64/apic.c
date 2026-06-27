#include "arch/x64/apic.h"
#include "arch/x64/utils.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "kernel/acpi.h"
#include "kernel/serial.h"

void __iomem *lapic_vaddr = NULL;
void __iomem *ioapic_vaddr = NULL;
uint32_t lapic_timer_ticks_calibrated = 0;

// TSC calibration results
uint64_t tsc_freq = 0;     // TSC ticks per second
uint64_t tsc_per_ms = 0;   // TSC ticks per millisecond
static uint64_t tsc_base = 0;  // TSC value at boot baseline

// Monotonic nanosecond clock since boot (TSC-based)
uint64_t sched_clock() {
  uint64_t now = rdtsc64();
  uint64_t delta = now - tsc_base;
  // Split to avoid overflow: delta * 1e9 overflows uint64_t after ~7s at 2.5GHz
  uint64_t sec = delta / tsc_freq;
  uint64_t rem = delta % tsc_freq;
  return sec * 1000000000ULL + rem * 1000000000ULL / tsc_freq;
}

// ===================== PIC disable =====================
void pic_disable() {
  outb(0x21, 0xFF);
  outb(0xA1, 0xFF);
}

// ===================== I/O APIC =====================
void ioapic_set_irq(uint32_t gsi, uint8_t vector, uint32_t apic_id,
                    bool masked) {
  uint32_t reg = IOAPIC_REDIR_OFFSET(gsi);
  uint64_t entry = IOAPIC_DELIVERY_FIXED | IOAPIC_DEST_MODE_PHYS |
                   IOAPIC_TRIGGER_EDGE | IOAPIC_POLARITY_HIGH |
                   (uint64_t)vector;
  if (masked) entry |= IOAPIC_INTR_MASKED;
  entry |= (uint64_t)apic_id << 56;

  ioapic_write(reg, (uint32_t)entry);
  ioapic_write(reg + 1, (uint32_t)(entry >> 32));
}

// ===================== PIT calibration =====================
// Use PIT to calibrate the LAPIC timer bus frequency.
// Strategy: set LAPIC timer to count down from max in one-shot mode,
// set PIT channel 0 to one-shot mode with a known period (~10ms),
// wait for PIT to expire, then read how many LAPIC ticks elapsed.
static uint32_t calibrate_lapic_timer() {
  // 10ms PIT period (divisor = 11932 for 100Hz = 10ms)
  uint16_t divisor = 11932;

  // Set LAPIC timer: one-shot, divide by 1, masked (no interrupt)
  lapic_write(LAPIC_TIMER_DCR, 0x0B); // divide by 1
  lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);

  // Set PIT channel 0 to mode 4 (software strobe): write divisor starts
  // countdown, counter stops at 0 (does not wrap/reload).
  outb(0x43, 0x38); // channel 0, lobyte/hibyte, mode 4, binary
  outb(0x40, divisor & 0xFF);
  outb(0x40, (divisor >> 8) & 0xFF);

  // Start LAPIC timer from max count (same time as PIT starts)
  lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

  // Poll until PIT counter reaches 0.
  // In mode 4 the counter stops at 0, so we just wait for cur == 0.
  // Latch before each read to get a consistent 16-bit snapshot.
  for (;;) {
    outb(0x43, 0x00); // latch PIT count
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    uint16_t cur = (uint16_t)((hi << 8) | lo);
    if (cur == 0) break;
  }

  // Read LAPIC current count
  uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);

  // elapsed = LAPIC ticks per 10ms. For 100Hz timer, use this directly.
  return elapsed;
}

// ===================== APIC MMIO mapping =====================
// Map APIC MMIO region into the higher-half virtual address space.
// Uses the device_vma_base area and pdpt_hh, same pattern as fb.cc.
static void map_apic_mmio(uint64_t lapic_phys, uint64_t ioapic_phys) {
  // Both LAPIC (0xFEE00000) and IOAPIC (0xFEC00000) fit in the
  // 0xFEC00000-0xFEE00000+4K range. Align to 2MB boundaries.
  uint64_t region_start = lapic_phys < ioapic_phys ? lapic_phys : ioapic_phys;
  region_start &= ~0x1FFFFFULL; // 2MB align down
  uint64_t region_end = (lapic_phys > ioapic_phys ? lapic_phys : ioapic_phys) + 0x1000;
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
    serial_puts("apic: no free PDPT_hh slot\n");
    halt();
  }

  // Allocate and fill a PD for the APIC MMIO region
  uint64_t *pd = (uint64_t *)bump_alloc(4096);
  uintptr_t pd_phys = (__force uintptr_t)PHY_ADDR((uintptr_t)pd);
  for (int i = 0; i < 512; i++) pd[i] = 0;

  // Fill PD with 2MB pages. Mark PCD+PWT for uncacheable MMIO.
  // 0x83 = Present + RW + PS (2MB), + 0x18 = PCD + PWT
  for (size_t n = 0; n < num_2mb; n++) {
    pd[n] = (region_start + n * 0x200000) | 0x9B; // Present+RW+PS+PCD+PWT
  }

  pdpt_hh[pdpt_idx] = pd_phys | PTE_PRESENT | PTE_RW;

  // Compute virtual address for this PDPT slot
  // pdpt_hh[i] maps virtual: VMA_BASE + (i - 510) * 0x40000000
  uint64_t vma = (0xFFFFULL << 48) | (511ULL << 39) | ((uint64_t)pdpt_idx << 30);
  // Account for device_vma_base tracking (must be within this PDPT slot's range)
  // The PD starts at the beginning of the PDPT slot's 1GB region
  uint64_t apic_vma = vma; // PD entry 0 = start of this 1GB region
  device_vma_base = apic_vma + num_2mb * 0x200000;

  flush_tlb();

  lapic_vaddr = (void __iomem __force *)(apic_vma + (lapic_phys - region_start));
  ioapic_vaddr = (void __iomem __force *)(apic_vma + (ioapic_phys - region_start));
}

// ===================== APIC init =====================
void apic_init() {
  uint64_t lapic_phys = g_madt.lapic_base;
  uint64_t ioapic_phys = g_madt.ioapic_base;
  if (lapic_phys == 0) lapic_phys = 0xFEE00000;
  if (ioapic_phys == 0) ioapic_phys = 0xFEC00000;

  // 1. Map APIC MMIO
  map_apic_mmio(lapic_phys, ioapic_phys);
  cpu_locals[0].lapic_base = lapic_vaddr;

  // 2. Enable LAPIC globally via IA32_APIC_BASE MSR
  uint64_t msr = rdmsr(MSR_IA32_APIC_BASE);
  msr |= APIC_BASE_ENABLE;
  wrmsr(MSR_IA32_APIC_BASE, msr);

  // 3. Software-enable LAPIC via Spurious Interrupt Vector Register
  lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFF);

  // 4. Remap PIC (don't fully disable yet — keyboard may still route through PIC)
  outb(0x20, 0x11); outb(0xA0, 0x11);
  outb(0x21, 0x20); outb(0xA1, 0x28);
  outb(0x21, 0x04); outb(0xA1, 0x02);
  outb(0x21, 0x01); outb(0xA1, 0x01);
  // Mask all PIC IRQs (interrupts now routed through I/O APIC)
  outb(0x21, 0xFF);
  outb(0xA1, 0xFF);

  // 5. Mask unused LAPIC LVT entries
  lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);

  // 6. Configure I/O APIC: mask all, then unmask timer + keyboard
  uint32_t bsp_apic_id = lapic_read(LAPIC_ID) >> 24;

  for (int i = 0; i < 24; i++) {
    ioapic_set_irq(i, 32 + i, bsp_apic_id, true);
  }
  ioapic_set_irq(0, 32, bsp_apic_id, false); // timer, edge
  // Keyboard: GSI 1, edge-triggered (PS/2 keyboard is edge-triggered)
  ioapic_set_irq(1, 33, bsp_apic_id, false);

  // 7. Calibrate and start LAPIC timer
  uint32_t ticks = calibrate_lapic_timer();
  lapic_timer_ticks_calibrated = ticks;

  // 8. Calibrate TSC using the same PIT 10ms window
  // Re-run the LAPIC calibration to measure TSC ticks per 10ms
  {
    uint16_t divisor = 11932;
    uint64_t tsc_start = rdtsc64();

    // PIT mode 4: counter stops at 0
    outb(0x43, 0x38); // channel 0, lobyte/hibyte, mode 4, binary
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    for (;;) {
      outb(0x43, 0x00);
      uint8_t lo = inb(0x40);
      uint8_t hi = inb(0x40);
      uint16_t cur = (uint16_t)((hi << 8) | lo);
      if (cur == 0) break;
    }

    uint64_t tsc_end = rdtsc64();
    uint64_t tsc_delta = tsc_end - tsc_start;
    // tsc_delta = TSC ticks per 10ms, multiply by 100 for Hz
    tsc_freq = tsc_delta * 100;
    tsc_per_ms = tsc_freq / 1000;
  }

  // Record TSC baseline for sched_clock()
  tsc_base = rdtsc64();

  // Start LAPIC periodic timer
  lapic_write(LAPIC_TIMER_DCR, 0x0B); // divide by 1
  lapic_write(LAPIC_LVT_TIMER, 32 | LAPIC_LVT_TIMER_PERIODIC);
  lapic_write(LAPIC_TIMER_ICR, ticks);
}
