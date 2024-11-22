#include "os-test/kernel/interrupt/isr.h"
#include "os-test/drivers/keyboard.h"
#include "os-test/drivers/screen.h"
#include "os-test/kernel/interrupt/idt.h"
#include "os-test/kernel/interrupt/timer.h"
#include "os-test/utils/x86.h"

isr_t interrupt_handlers[256];

void isr_handler(registers_t *r);
void irq_handler(registers_t *r);

static inline void __isr(int id, isr_t handle) __attribute__((always_inline));
static void __isr(int id, isr_t handle) {
  asm volatile("pushl $0\n"
               "movl %0, %%ebx\n"
               "push %%ebx\n"
               :
               : "r"(id)
               : "ebx");

  asm volatile("pusha\n"
               "push %%ds\n"
               "push %%es\n"
               "push %%fs\n"
               "push %%gs\n"
               :
               :
               : "memory");

  asm volatile("movw $0x10, %%ax\n"
               "movw %%ax, %%ds\n"
               "movw %%ax, %%es\n"
               "movw %%ax, %%fs\n"
               "movw %%ax, %%gs\n"
               "push %%esp\n"
               "cld\n"
               :
               :
               : "memory");

  asm volatile("movl %0, %%ebx\n"
               "call *%%ebx\n"
               "pop %%ebx\n"
               :
               : "r"(handle)
               : "ebx");

  asm volatile("pop %%ds\n"
               "pop %%es\n"
               "pop %%fs\n"
               "pop %%gs\n"
               "popa\n"
               "addl $8, %%esp\n"
               "iret"
               :
               :
               : "memory");
}

#define FOREACH(F)                                                             \
  F(0)                                                                         \
  F(1)                                                                         \
  F(2)                                                                         \
  F(3)                                                                         \
  F(4)                                                                         \
  F(5)                                                                         \
  F(6)                                                                         \
  F(7)                                                                         \
  F(8)                                                                         \
  F(9)                                                                         \
  F(10)                                                                        \
  F(11)                                                                        \
  F(12)                                                                        \
  F(13)                                                                        \
  F(14)                                                                        \
  F(15)                                                                        \
  F(16)                                                                        \
  F(17)                                                                        \
  F(18)                                                                        \
  F(19)                                                                        \
  F(20)                                                                        \
  F(21)                                                                        \
  F(22)                                                                        \
  F(23)                                                                        \
  F(24)                                                                        \
  F(25)                                                                        \
  F(26)                                                                        \
  F(27)                                                                        \
  F(28)                                                                        \
  F(29)                                                                        \
  F(30)                                                                        \
  F(31)

#define GEN_ISR(N)                                                             \
  __attribute__((naked)) void isr##N() { __isr(N, isr_handler); }
FOREACH(GEN_ISR)
#undef GEN_ISR

void interrupt_install() {
#define GEN_ISR_INSTALL(N) set_idt_gate(N, (uint32_t)isr##N);
  FOREACH(GEN_ISR_INSTALL)
#undef GEN_ISR_INSTALL
}
#undef FOREACH

#define FOREACH(F)                                                             \
  F(0)                                                                         \
  F(1)                                                                         \
  F(2)                                                                         \
  F(3)                                                                         \
  F(4)                                                                         \
  F(5)                                                                         \
  F(6)                                                                         \
  F(7)                                                                         \
  F(8)                                                                         \
  F(9)                                                                         \
  F(10)                                                                        \
  F(11)                                                                        \
  F(12)                                                                        \
  F(13)                                                                        \
  F(14)                                                                        \
  F(15)

#define GEN_IRQ(N)                                                             \
  __attribute__((naked)) void irq##N() { __isr(N + 31, irq_handler); }
FOREACH(GEN_IRQ)
#undef GEN_IRQ

void irq_reg() {
#define GEN_IRQ_INSTALL(N) set_idt_gate(N + 31, (uint32_t)irq##N);
  FOREACH(GEN_IRQ_INSTALL)
#undef GEN_IRQ_INSTALL
}
#undef FOREACH

/* Can't do this with a loop because we need the address
 * of the function names */
void isr_install() {
  interrupt_install();

  // Remap the PIC
  outb(0x20, 0x11);
  outb(0xA0, 0x11);
  outb(0x21, 0x20);
  outb(0xA1, 0x28);
  outb(0x21, 0x04);
  outb(0xA1, 0x02);
  outb(0x21, 0x01);
  outb(0xA1, 0x01);
  outb(0x21, 0x0);
  outb(0xA1, 0x0);

  // Install the IRQs
  irq_reg();

  set_idt(); // Load with ASM
}

/* To print the message which defines every exception */
char *exception_messages[] = {"Division By Zero",
                              "Debug",
                              "Non Maskable Interrupt",
                              "Breakpoint",
                              "Into Detected Overflow",
                              "Out of Bounds",
                              "Invalid Opcode",
                              "No Coprocessor",

                              "Double Fault",
                              "Coprocessor Segment Overrun",
                              "Bad TSS",
                              "Segment Not Present",
                              "Stack Fault",
                              "General Protection Fault",
                              "Page Fault",
                              "Unknown Interrupt",

                              "Coprocessor Fault",
                              "Alignment Check",
                              "Machine Check",
                              "Reserved",
                              "Reserved",
                              "Reserved",
                              "Reserved",
                              "Reserved",

                              "Reserved",
                              "Reserved",
                              "Reserved",
                              "Reserved",
                              "Reserved",
                              "Reserved",
                              "Reserved",
                              "Reserved"};

void isr_handler(registers_t *r) {
  kprint("received interrupt: ");
  kprint("0x");
  static const char map[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                               '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  char s[5];
  s[0] = map[(r->int_no >> 24 & 0xff)];
  s[1] = map[(r->int_no >> 16 & 0xff)];
  s[2] = map[(r->int_no >> 8 & 0xff)];
  s[3] = map[(r->int_no & 0xff)];
  s[4] = '\0';
  kprint(s);
  kprint("\n");
  kprint(exception_messages[r->int_no]);
  kprint("\n");
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
  interrupt_handlers[n] = handler;
}

void irq_handler(registers_t *r) {
  /* After every interrupt we need to send an EOI to the PICs
   * or they will not send another interrupt again */
  if (r->int_no >= 40)
    outb(0xA0, 0x20); /* slave */
  outb(0x20, 0x20);   /* master */

  /* Handle the interrupt in a more modular way */
  if (interrupt_handlers[r->int_no] != 0) {
    isr_t handler = interrupt_handlers[r->int_no];
    handler(r);
  }
}

void irq_install() {
  /* Enable interruptions */
  asm volatile("sti");
  /* IRQ0: timer */
  init_timer(50);
  /* IRQ1: keyboard */
  init_keyboard();
}
