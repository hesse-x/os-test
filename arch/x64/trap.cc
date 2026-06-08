#include "arch/x64/trap.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "arch/x64/paging.h"

// Vector stubs defined in vectors.S
#define V(N) extern "C" void vector##N();
V(0) V(1) V(2) V(3) V(4) V(5) V(6) V(7)
V(8) V(9) V(10) V(11) V(12) V(13) V(14) V(15)
V(16) V(17) V(18) V(19) V(20) V(21) V(22) V(23)
V(24) V(25) V(26) V(27) V(28) V(29) V(30) V(31)
V(32) V(33) V(34) V(35) V(36) V(37) V(38) V(39)
V(40) V(41) V(42) V(43) V(44) V(45) V(46) V(47)
#undef V

static uint64_t __vectors[IDT_ENTRIES] = {
#define V(N) (uint64_t)vector##N,
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

void set_idt_gate(int n, uint64_t handler, uint8_t flags, uint8_t ist) {
  idt[n].offset_low = L16(handler);
  idt[n].sel = KERNEL_CS;
  idt[n].ist = ist;
  idt[n].flags = flags;
  idt[n].offset_mid = H16(handler);
  idt[n].offset_high = H32(handler);
  idt[n].reserved = 0;
}

void set_idt() {
  uint64_t base = (uint64_t)&idt;
  idt_reg.limit = IDT_ENTRIES * sizeof(idt_gate_t) - 1;
  idt_reg.base_low = L16(base);
  idt_reg.base_high = (uint32_t)(base >> 16);  // bits 31:16
  // For lidt in 64-bit mode, we use inline asm with the full 10-byte descriptor
  struct {
    uint16_t limit;
    uint64_t base;
  } __attribute__((packed)) idtr;
  idtr.limit = idt_reg.limit;
  idtr.base = base;
  lidt(&idtr);
}

void idt_install() {
  for (int i = 0; i < 48; i++) {
    set_idt_gate(i, __vectors[i]);
  }
  // IST assignments for critical exceptions
  set_idt_gate(2, (uint64_t)vector2, 0x8E, 1);   // NMI → IST1
  set_idt_gate(8, (uint64_t)vector8, 0x8E, 2);   // Double Fault → IST2
  set_idt_gate(18, (uint64_t)vector18, 0x8E, 3);  // Machine Check → IST3
  set_idt();
}

// ===================== SYSCALL/SYSRET MSR setup =====================
extern "C" void syscall_fast_entry(void);

void setup_syscall() {
  // MSR_STAR: [63:32] = kernel CS (0x08), [31:0] = user CS base (0x0B for SYSRET)
  // SYSCALL loads CS from STAR[47:32] = 0x08, SS = 0x08+8 = 0x10
  // SYSRET loads CS from STAR[63:48]+16 = 0x0B+16 = 0x1B, SS = 0x0B+8 = 0x23
  uint64_t star = ((uint64_t)0x08 << 32) | ((uint64_t)0x0B << 48);
  // Wait, STAR layout: [63:48] = SYSRET CS base, [47:32] = SYSCALL CS base
  // SYSCALL: CS = STAR[47:32], SS = STAR[47:32]+8
  // SYSRET: CS = STAR[63:48]+16, SS = STAR[63:48]+8
  // So: STAR[47:32] = 0x08 (kernel CS), STAR[63:48] = 0x0B (user CS base: 0x0B+16=0x1B, 0x0B+8=0x23)
  star = ((uint64_t)0x08 << 32) | ((uint64_t)0x0B << 48);
  wrmsr(MSR_STAR, star);

  // MSR_LSTAR: SYSCALL entry point (64-bit)
  wrmsr(MSR_LSTAR, (uint64_t)syscall_fast_entry);

  // MSR_CSTAR: compatibility mode entry (not used, set to 0)
  wrmsr(MSR_CSTAR, 0);

  // MSR_SFMASK: flags to clear on SYSCALL
  // Clear IF (bit 9) so SYSCALL always enters with interrupts disabled
  // Also clear TF (bit 8) to avoid single-stepping the kernel entry
  wrmsr(MSR_SFMASK, (1ULL << 9) | (1ULL << 8));

  // Enable SYSCALL/SYSRET via EFER.SCE
  uint64_t efer = rdmsr(MSR_EFER);
  efer |= EFER_SCE;
  wrmsr(MSR_EFER, efer);
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
