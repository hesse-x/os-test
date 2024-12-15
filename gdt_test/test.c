#include "update_gdt.h"
#include <stdio.h>

extern uint32_t __gdtdesc;
extern uint32_t __gdt;
void update_s(uint32_t offset);

int main() {
//  printf("sd_ptr: %p\n",(void*)(__gdtdesc - 24));
//  struct pseudodesc *gdt = (struct pseudodesc *)__gdtdesc;
//  gdt->pd_base = __gdtdesc - 24;
  print_gdt((struct pseudodesc *)&__gdtdesc);
  print_ptr(print_ptr);
  update_s(0x100000);
  print_gdt((struct pseudodesc *)&__gdtdesc);
}
