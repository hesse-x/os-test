#ifndef KERNEL_TRAP_H
#define KERNEL_TRAP_H

#include "arch/x86/trap.h"

typedef void (*irq_handler_t)(trapframe_t *);
typedef uint32_t (*syscall_fn_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

extern "C" {
void register_irq(int vec, irq_handler_t fn);
void trap_dispatch(trapframe_t *tf);
void syscall_dispatch(trapframe_t *tf);
void isr_init();

// Syscalls
uint32_t sys_putc(uint32_t arg1, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t sys_getpid(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t sys_yield(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t sys_getc(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

// Kbd buffer
bool kbd_buffer_empty();
char kbd_buffer_pop();
}

#endif // KERNEL_TRAP_H
