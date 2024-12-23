#include "os-test/kernel/interrupt/idt.h"
#include "os-test/utils/os_utils.h"

// ------ internal data ------
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

static idt_gate_t idt[IDT_ENTRIES];
static idt_register_t idt_reg;

// ------ export interface ------
void set_idt_gate(int n, uint32_t handler) {
  idt[n].low_offset = L16(handler);
  idt[n].sel = KERNEL_CS;
  idt[n].always0 = 0;
  idt[n].flags = 0x8E;
  idt[n].high_offset = H16(handler);
}

void set_idt() {
  idt_reg.base = (uint32_t)&idt;
  idt_reg.limit = IDT_ENTRIES * sizeof(idt_gate_t) - 1;
  /* Don't make the mistake of loading &idt -- always load &idt_reg */
  __asm__ __volatile__("lidtl (%0)" : : "r"(&idt_reg));
}
