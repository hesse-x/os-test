#include "isr.h"
#include "serial.h"
#include "kbd.h"
#include "mem.h"

// Vector stubs defined in vectors.S
#define V(N) extern "C" void vector##N();
V(0) V(1) V(2) V(3) V(4) V(5) V(6) V(7)
V(8) V(9) V(10) V(11) V(12) V(13) V(14) V(15)
V(16) V(17) V(18) V(19) V(20) V(21) V(22) V(23)
V(24) V(25) V(26) V(27) V(28) V(29) V(30) V(31)
V(32) V(33) V(34) V(35) V(36) V(37) V(38) V(39)
V(40) V(41) V(42) V(43) V(44) V(45) V(46) V(47)
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
  __asm__ volatile("lidtl (%0)" : : "r"(&idt_reg));
}

static void idt_init() {
  for (int i = 0; i < IDT_ENTRIES; i++) {
    set_idt_gate(i, __vectors[i]);
  }
  set_idt();
}

// ===================== PIC =====================
static void pic_remap() {
  /* ICW1: start initialization + ICW4 needed */
  outb(0x20, 0x11);
  outb(0xA0, 0x11);
  /* ICW2: vector offsets — master at 32, slave at 40 */
  outb(0x21, 0x20);
  outb(0xA1, 0x28);
  /* ICW3: master sees slave on IRQ2, slave cascades on IRQ2 */
  outb(0x21, 0x04);
  outb(0xA1, 0x02);
  /* ICW4: 8086 mode */
  outb(0x21, 0x01);
  outb(0xA1, 0x01);
  /* Mask all except IRQ0 (timer) and IRQ1 (keyboard) */
  outb(0x21, 0xFC);
  outb(0xA1, 0xFF);
}

// ===================== PIT =====================
static void pit_init() {
  uint16_t divisor = 11932; /* 1193182 / 100 Hz ≈ 11932 */
  outb(0x43, 0x36);         /* channel 0, lobyte/hibyte, rate generator */
  outb(0x40, divisor & 0xFF);
  outb(0x40, (divisor >> 8) & 0xFF);
}

// ===================== Trap dispatch =====================
static uint32_t tick = 0;

void trap(trapframe_t *tf) {
  if (tf->trapno == 32) {
    tick++;
    outb(0x20, 0x20); /* EOI */
    return;
  }
  if (tf->trapno == 33) {
    kbd_handle();
    outb(0x20, 0x20); /* EOI */
    return;
  }
  if (tf->trapno >= 32 && tf->trapno <= 47) {
    /* Spurious/other IRQ: send EOI */
    if (tf->trapno >= 40)
      outb(0xA0, 0x20);
    outb(0x20, 0x20);
    return;
  }
  /* CPU exception: print diagnostic and halt */
  serial_puts("EXCEPTION: vector ");
  serial_put_hex(tf->trapno);
  serial_puts(" err ");
  serial_put_hex(tf->err_code);
  serial_puts(" eip ");
  serial_put_hex(tf->eip);
  serial_puts("\n");
  __asm__ volatile("cli; hlt");
}

// ===================== Init =====================
void isr_init() {
  gdt_init();
  idt_init();
  pic_remap();
  pit_init();
  kbd_init();
  __asm__ volatile("sti");
}
