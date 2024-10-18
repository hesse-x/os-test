#include "cpu/isr.h"
#include "drivers/screen.h"
#include "libc/string.h"

void kernel_start() {
  kprint("Kernel start...\n");
  isr_install();
  irq_install();

  kprint("Type something, it will go through the kernel\n"
         "Type END to halt the CPU\n> ");
}
