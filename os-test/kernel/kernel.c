#include "os-test/cpu/isr.h"
#include "os-test/drivers/screen.h"
#include "os-test/libc/string.h"

void kernel_start() {
  kprint("Kernel start...\n");
  isr_install();
  irq_install();

  kprint("Type something, it will go through the kernel\n"
         "Type END to halt the CPU\n> ");
}
