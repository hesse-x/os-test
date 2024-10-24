#ifndef UTILS_GLOBAL_CONSTEXPR_H_
#define UTILS_GLOBAL_CONSTEXPR_H_
#include <stdint.h>
#define KERNEL_ELF_ADDR 0x10000
#define KERNEL_ENTRY_ADDR 0x100000
#define KERNEL_STACK_TOP 0x90000
static inline void init_stack_and_call(uint32_t, uint32_t) __attribute__((always_inline));

static inline void init_stack_and_call(uint32_t fn, uint32_t stack_top) {
    asm volatile (
        "movl %%ecx, %%esp\n"
        "movl %%ecx, %%ebp\n"
        "call *%%ebx\n"
        :
        : "b"(fn), "c"(stack_top)
        : "memory"
    );
}

#endif // UTILS_GLOBAL_CONSTEXPR_H_
