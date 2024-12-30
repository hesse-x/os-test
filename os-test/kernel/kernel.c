#include "os-test/drivers/screen.h"
#include "os-test/kernel/interrupt/isr.h"
#include "os-test/kernel/mem/mmu.h"
#include "os-test/kernel/mem/pmm.h"
#include "os-test/utils/os_utils.h"

extern char etext[], edata[], end[], kern_start[];
void kernel_init(void) {
  pmm_init();

  init_screen();
  isr_install();
  irq_install();
  kprint("Kernel init finished\n");
  kprint("Type something, it will go through the kernel\n"
         "Type END to halt the CPU\n> ");
  while (1)
    ;
}
