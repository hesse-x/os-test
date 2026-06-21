#include "arch/x64/trap.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "arch/x64/paging.h"

// Vector stubs defined in vectors.S
#define V(N) void vector##N();
V(0) V(1) V(2) V(3) V(4) V(5) V(6) V(7)
V(8) V(9) V(10) V(11) V(12) V(13) V(14) V(15)
V(16) V(17) V(18) V(19) V(20) V(21) V(22) V(23)
V(24) V(25) V(26) V(27) V(28) V(29) V(30) V(31)
V(32) V(33) V(34) V(35) V(36) V(37) V(38) V(39)
V(40) V(41) V(42) V(43) V(44) V(45) V(46) V(47)
V(48) V(49) V(50) V(51) V(52) V(53) V(54) V(55)
V(56) V(57) V(58) V(59) V(60) V(61) V(62) V(63)
V(64) V(65) V(66) V(67) V(68) V(69) V(70) V(71)
V(72) V(73) V(74) V(75) V(76) V(77) V(78) V(79)
V(80) V(81) V(82) V(83) V(84) V(85) V(86) V(87)
V(88) V(89) V(90) V(91) V(92) V(93) V(94) V(95)
V(96) V(97) V(98) V(99) V(100) V(101) V(102) V(103)
V(104) V(105) V(106) V(107) V(108) V(109) V(110) V(111)
V(112) V(113) V(114) V(115) V(116) V(117) V(118) V(119)
V(120) V(121) V(122) V(123) V(124) V(125) V(126) V(127)
#undef V

static uint64_t __vectors[IDT_ENTRIES] = {
#define V(N) (uint64_t)vector##N,
    V(0) V(1) V(2) V(3) V(4) V(5) V(6) V(7)
    V(8) V(9) V(10) V(11) V(12) V(13) V(14) V(15)
    V(16) V(17) V(18) V(19) V(20) V(21) V(22) V(23)
    V(24) V(25) V(26) V(27) V(28) V(29) V(30) V(31)
    V(32) V(33) V(34) V(35) V(36) V(37) V(38) V(39)
    V(40) V(41) V(42) V(43) V(44) V(45) V(46) V(47)
    V(48) V(49) V(50) V(51) V(52) V(53) V(54) V(55)
    V(56) V(57) V(58) V(59) V(60) V(61) V(62) V(63)
    V(64) V(65) V(66) V(67) V(68) V(69) V(70) V(71)
    V(72) V(73) V(74) V(75) V(76) V(77) V(78) V(79)
    V(80) V(81) V(82) V(83) V(84) V(85) V(86) V(87)
    V(88) V(89) V(90) V(91) V(92) V(93) V(94) V(95)
    V(96) V(97) V(98) V(99) V(100) V(101) V(102) V(103)
    V(104) V(105) V(106) V(107) V(108) V(109) V(110) V(111)
    V(112) V(113) V(114) V(115) V(116) V(117) V(118) V(119)
    V(120) V(121) V(122) V(123) V(124) V(125) V(126) V(127)
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
  for (int i = 0; i < 128; i++) {
    set_idt_gate(i, __vectors[i], 0x8E, 0);
  }
  // IST assignments for critical exceptions
  set_idt_gate(2, (uint64_t)vector2, 0x8E, 1);   // NMI → IST1
  set_idt_gate(8, (uint64_t)vector8, 0x8E, 2);   // Double Fault → IST2
  set_idt_gate(18, (uint64_t)vector18, 0x8E, 3);  // Machine Check → IST3
  set_idt();
}

// ===================== SYSCALL/SYSRET MSR setup =====================
void syscall_fast_entry(void);

void setup_syscall() {
  // MSR_STAR: [47:32] = kernel CS (0x08), [63:48] = SYSRET base (0x18)
  // SYSCALL: CS = STAR[47:32] = 0x08, SS = STAR[47:32]+8 = 0x10
  // SYSRET64: CS = STAR[63:48]+16 | 3 = 0x18+16 | 3 = 0x2B, SS = STAR[63:48]+8 | 3 = 0x18+8 | 3 = 0x23
  // SYSRET32: CS = STAR[63:48] | 3 = 0x18 | 3 = 0x1B, SS = STAR[63:48]+8 | 3 = 0x23
  uint64_t star = ((uint64_t)0x08 << 32) | ((uint64_t)0x18 << 48);
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
