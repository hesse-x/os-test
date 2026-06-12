#ifndef KERNEL_TRAP_H
#define KERNEL_TRAP_H

#include "arch/x64/trap.h"

typedef void (*irq_handler_t)(trapframe_t *);
typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

extern "C" {
void register_irq(int vec, irq_handler_t fn);
void trap_dispatch(trapframe_t *tf);
void syscall_dispatch(trapframe_t *tf);
void isr_init();

// Syscalls
uint64_t sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_yield(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_getc(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_wait(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_notify(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_irq_bind(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_sbrk(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_exit(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_waitpid(uint64_t arg1, uint64_t arg2, uint64_t, uint64_t, uint64_t);
uint64_t sys_spawn(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t);
}

#endif // KERNEL_TRAP_H
