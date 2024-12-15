#include <stdio.h>
#include "update_gdt.h"
static inline void store_addr(struct segdesc *sd, uint32_t base_addr) __attribute__((always_inline));

static inline uint32_t read_addr(struct segdesc *sd) __attribute__((always_inline));
void update_gdt(uint32_t new_offset, uint32_t gdt_addr) {
  struct pseudodesc *gdt = (struct pseudodesc *)gdt_addr;
  struct segdesc *sd = (struct segdesc *)(gdt->pd_base);

  for (int i = 0; i < 3; i++) {
    uint32_t base_addr = read_addr(sd + i);
    base_addr += new_offset;
    store_addr(sd + i, base_addr);
  }
}

void print_ptr(void *ptr) {
//  struct pseudodesc *gdt = (struct pseudodesc *)ptr;
//  printf("ptr: %p, sd: %p\n", ptr, (void*)(gdt->pd_base));
  printf("ptr: %p\n", ptr);
}

void print_gdt(struct pseudodesc *gdt) {
  struct segdesc *sd = (struct segdesc *)(gdt->pd_base);
  for (int i = 0; i < 3; i++) {
    uint32_t base_addr = read_addr(sd + i);
    printf("sd%d: %p\n", i, (void*)base_addr);
  }
}

static inline uint32_t read_addr(struct segdesc *sd) {
  uint32_t base_addr = 0;
  base_addr |= sd->sd_base_15_0;
  base_addr |= sd->sd_base_23_16 << 16;
  base_addr |= sd->sd_base_31_24 << 24;
  return base_addr;
}

static inline void store_addr(struct segdesc *sd, uint32_t base_addr) {
  sd->sd_base_15_0 = (base_addr & 0x0000FFFF);
  sd->sd_base_23_16 = (base_addr & 0x00FF0000) >> 16;
  sd->sd_base_31_24 = (base_addr & 0xFF000000) >> 24;
}
