#ifndef UTILS_GLOBAL_CONSTEXPR_H_
#define UTILS_GLOBAL_CONSTEXPR_H_
#include <stdint.h>
#define KERNEL_ENTRY_ADDR 0x10000
#define KERNEL_STACK_TOP 0x90000
static inline void init_stack(uint32_t) __attribute__((always_inline));

static inline void init_stack(uint32_t stack_top) {
  __asm__ volatile("mov %%eax, %%esp;"
                   "mov %%eax, %%ebp;"
                   :                // No result.
                   : "a"(stack_top) // Operand %eax
                   : "memory"       // May write memory
  );
}

#endif // UTILS_GLOBAL_CONSTEXPR_H_
