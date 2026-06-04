#ifndef ISR_H
#define ISR_H

#include <stddef.h>
#include <stdint.h>

// ===================== I/O port helpers =====================
static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

// ===================== Constants =====================
#define KERNEL_CS 0x08
#define IDT_ENTRIES 48

#define L16(x) ((uint16_t)((x) & 0xFFFF))
#define H16(x) ((uint16_t)(((x) >> 16) & 0xFFFF))

// ===================== IDT =====================

/* How every interrupt gate (handler) is defined */
typedef struct {
  uint16_t low_offset; /* Lower 16 bits of handler function address */
  uint16_t sel;        /* Kernel segment selector */
  uint8_t always0;
  /* First byte
   * Bit 7: "Interrupt is present"
   * Bits 6-5: Privilege level of caller (0=kernel..3=user)
   * Bit 4: Set to 0 for interrupt gates
   * Bits 3-0: bits 1110 = decimal 14 = "32 bit interrupt gate" */
  uint8_t flags;
  uint16_t high_offset; /* Higher 16 bits of handler function address */
} __attribute__((packed)) idt_gate_t;

/* A pointer to the array of interrupt handlers.
 * Assembly instruction 'lidt' will read it */
typedef struct {
  uint16_t limit;
  uint32_t base;
} __attribute__((packed)) idt_register_t;

// ===================== Trapframe =====================

/* registers as pushed by pushal */
typedef struct {
  uint32_t edi;
  uint32_t esi;
  uint32_t ebp;
  uint32_t esp_ignored;
  uint32_t ebx;
  uint32_t edx;
  uint32_t ecx;
  uint32_t eax;
} pushregs_t;

/* Trapframe layout must match the push order in trapentry.S */
typedef struct {
  pushregs_t regs;
  uint32_t gs;
  uint32_t fs;
  uint32_t es;
  uint32_t ds;
  /* pushed by vectorN stubs */
  uint32_t trapno;
  uint32_t err_code;
  /* CPU automatic push */
  uint32_t eip;
  uint32_t cs;
  uint32_t eflags;
} trapframe_t;

// ===================== Export interface =====================
extern "C" {
void set_idt_gate(int n, uint32_t handler);
void set_idt();

void isr_init();
void trap(trapframe_t *tf);
}

#endif // ISR_H
