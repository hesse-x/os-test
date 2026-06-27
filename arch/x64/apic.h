#ifndef ARCH_X64_APIC_H
#define ARCH_X64_APIC_H

#include <stdbool.h>
#include <stdint.h>
#include "kernel/sparse.h"

// LAPIC register offsets (MMIO)
#define LAPIC_ID        0x020
#define LAPIC_VERSION   0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ICR_LOW   0x300
#define LAPIC_ICR_HIGH  0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_TIMER_ICR 0x380  // initial count
#define LAPIC_TIMER_CCR 0x390  // current count
#define LAPIC_TIMER_DCR 0x3E0  // divide configuration

// LAPIC SVR flags
#define LAPIC_SVR_ENABLE  0x100

// LAPIC LVT timer modes
#define LAPIC_LVT_TIMER_PERIODIC  0x20000

// LAPIC LVT mask bit
#define LAPIC_LVT_MASKED  0x10000

// I/O APIC register offsets (MMIO)
#define IOAPIC_REG  0x00
#define IOAPIC_DATA 0x10

// I/O APIC registers
#define IOAPIC_ID    0x00
#define IOAPIC_VER   0x01
#define IOAPIC_ARB   0x02

// I/O APIC redirection entry
#define IOAPIC_REDIR_OFFSET(irq) (0x10 + (irq) * 2)

// I/O APIC redirection entry flags
#define IOAPIC_DELIVERY_FIXED    0x00000
#define IOAPIC_DELIVERY_LOWPRIO  0x20000
#define IOAPIC_DELIVERY_SMI      0x40000
#define IOAPIC_DELIVERY_NMI      0x60000
#define IOAPIC_DELIVERY_INIT     0x80000
#define IOAPIC_DELIVERY_EXTINT   0xA0000
#define IOAPIC_DEST_MODE_PHYS    0x00000
#define IOAPIC_DEST_MODE_LOGIC   0x80000
#define IOAPIC_INTR_MASKED       0x10000
#define IOAPIC_TRIGGER_EDGE      0x00000
#define IOAPIC_TRIGGER_LEVEL     0x800000000ULL
#define IOAPIC_POLARITY_HIGH     0x00000
#define IOAPIC_POLARITY_LOW      0x4000000000ULL

// IA32_APIC_BASE MSR
#define MSR_IA32_APIC_BASE 0x1B
#define APIC_BASE_ENABLE    0x800
#define APIC_BASE_BSP       0x100

// Timer frequency (Hz)
#define LAPIC_TIMER_HZ 100

extern void __iomem *lapic_vaddr;
extern void __iomem *ioapic_vaddr;

// LAPIC register access
__attribute__((no_sanitize("kernel-address")))
static inline uint32_t lapic_read(uint32_t offset) {
  return *(volatile uint32_t __force *)(lapic_vaddr + offset);
}

__attribute__((no_sanitize("kernel-address")))
static inline void lapic_write(uint32_t offset, uint32_t val) {
  *(volatile uint32_t __force *)(lapic_vaddr + offset) = val;
}

__attribute__((no_sanitize("kernel-address")))
static inline void lapic_eoi() {
  lapic_write(LAPIC_EOI, 0);
}

// I/O APIC register access
__attribute__((no_sanitize("kernel-address")))
static inline uint32_t ioapic_read(uint32_t reg) {
  *(volatile uint32_t __force *)(ioapic_vaddr + IOAPIC_REG) = reg;
  return *(volatile uint32_t __force *)(ioapic_vaddr + IOAPIC_DATA);
}

__attribute__((no_sanitize("kernel-address")))
static inline void ioapic_write(uint32_t reg, uint32_t val) {
  *(volatile uint32_t __force *)(ioapic_vaddr + IOAPIC_REG) = reg;
  *(volatile uint32_t __force *)(ioapic_vaddr + IOAPIC_DATA) = val;
}

void apic_init();
void pic_disable();

// I/O APIC IRQ configuration (called by sys_irq_bind to unmask)
void ioapic_set_irq(uint32_t gsi, uint8_t vector, uint32_t apic_id, bool masked);

// BSP-calibrated LAPIC timer ticks (set by apic_init, used by AP init)
extern uint32_t lapic_timer_ticks_calibrated;

// TSC frequency (set by apic_init during PIT-based calibration)
extern uint64_t tsc_freq;       // TSC ticks per second
extern uint64_t tsc_per_ms;     // TSC ticks per millisecond

// Monotonic nanosecond clock since boot (TSC-based)
uint64_t sched_clock();

#endif // ARCH_X64_APIC_H
