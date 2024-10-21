#include "os-test/cpu/isr.h"
#include "os-test/drivers/screen.h"
#include "os-test/libc/string.h"

extern char etext[], edata[], end[], kern_start[];
void kernel_start(void) {
  // Here, we have taken over the entire boot process,
  // and this function will no longer return,
  // so we completely reset the stack(ebp/esp).
  __asm__ volatile (
      "mov %%eax, %%esp;"
      "mov %%eax, %%ebp;"
      :                       // No result.
      : "a" (0x9000)          // Operand %eax
      : "memory"              // May write memory
  );

  kprint("Kernel start...\n");
  isr_install();
  irq_install();

  kprint("Type something, it will go through the kernel\n"
         "Type END to halt the CPU\n> ");
  while(1);
}
