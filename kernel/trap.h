#ifndef KERNEL_TRAP_H
#define KERNEL_TRAP_H

#include "arch/x86/trap.h"

typedef void (*irq_handler_t)(trapframe_t *);

extern "C" {
void register_irq(int vec, irq_handler_t fn);
void trap_dispatch(trapframe_t *tf);
void syscall_dispatch(trapframe_t *tf);
void isr_init();
}

#endif // KERNEL_TRAP_H
