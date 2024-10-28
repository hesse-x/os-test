#include "os-test/cpu/isr.h"
#include "os-test/drivers/screen.h"
#include "os-test/kernel/mmu.h"
#include "os-test/kernel/pmm.h"
#include "os-test/utils/os_utils.h"

extern char etext[], edata[], end[], kern_start[];
void kernel_init(void) {
  init_screen();
  kprint("Kernel start...\n");
  pmm_init();
  isr_install();
  irq_install();
  kprint("Type something, it will go through the kernel\n"
         "Type END to halt the CPU\n> ");
  while (1)
    ;
}
