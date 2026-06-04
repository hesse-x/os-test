#include "arch/x86/trap.h"
#include "arch/x86/utils.h"
#include "arch/x86/paging.h"

// Vector stubs defined in vectors.S
#define V(N) extern "C" void vector##N();
V(0) V(1) V(2) V(3) V(4) V(5) V(6) V(7)
V(8) V(9) V(10) V(11) V(12) V(13) V(14) V(15)
V(16) V(17) V(18) V(19) V(20) V(21) V(22) V(23)
V(24) V(25) V(26) V(27) V(28) V(29) V(30) V(31)
V(32) V(33) V(34) V(35) V(36) V(37) V(38) V(39)
V(40) V(41) V(42) V(43) V(44) V(45) V(46) V(47)
V(128)
#undef V

static uint32_t __vectors[IDT_ENTRIES] = {
#define V(N) (uint32_t)vector##N,
    V(0) V(1) V(2) V(3) V(4) V(5) V(6) V(7)
    V(8) V(9) V(10) V(11) V(12) V(13) V(14) V(15)
    V(16) V(17) V(18) V(19) V(20) V(21) V(22) V(23)
    V(24) V(25) V(26) V(27) V(28) V(29) V(30) V(31)
    V(32) V(33) V(34) V(35) V(36) V(37) V(38) V(39)
    V(40) V(41) V(42) V(43) V(44) V(45) V(46) V(47)
#undef V
};

// ===================== IDT =====================
static idt_gate_t idt[IDT_ENTRIES];
static idt_register_t idt_reg;

void set_idt_gate(int n, uint32_t handler, uint8_t flags) {
  idt[n].low_offset = L16(handler);
  idt[n].sel = KERNEL_CS;
  idt[n].always0 = 0;
  idt[n].flags = flags;
  idt[n].high_offset = H16(handler);
}

void set_idt() {
  idt_reg.base = (uint32_t)&idt;
  idt_reg.limit = IDT_ENTRIES * sizeof(idt_gate_t) - 1;
  __asm__ volatile("lidtl (%0)" : : "r"(&idt_reg));
}

void idt_install() {
  for (int i = 0; i < 48; i++) {
    set_idt_gate(i, __vectors[i]);
  }
  // vector128: syscall gate, DPL=3 (interrupt gate, user-callable)
  set_idt_gate(128, (uint32_t)vector128, 0xEE);
  set_idt();
}

// ===================== PIC =====================
void pic_remap() {
  outb(0x20, 0x11);
  outb(0xA0, 0x11);
  outb(0x21, 0x20);
  outb(0xA1, 0x28);
  outb(0x21, 0x04);
  outb(0xA1, 0x02);
  outb(0x21, 0x01);
  outb(0xA1, 0x01);
  outb(0x21, 0xFC);
  outb(0xA1, 0xFF);
}

// ===================== PIT =====================
void pit_init() {
  uint16_t divisor = 11932;
  outb(0x43, 0x36);
  outb(0x40, divisor & 0xFF);
  outb(0x40, (divisor >> 8) & 0xFF);
}
