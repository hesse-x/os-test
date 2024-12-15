#include "os-test/kernel/mem/mmu.h"
#include "os-test/utils/x86.h"
static inline void store_addr(struct segdesc *sd, int32_t base_addr) __attribute__((always_inline));

static inline int32_t read_addr(struct segdesc *sd) __attribute__((always_inline));
void update_gdt(int32_t new_offset, uint32_t gdt_addr, int32_t n) {
  struct pseudodesc *gdt = (struct pseudodesc *)gdt_addr;
  gdt->pd_base += new_offset;
  struct segdesc *sd = (struct segdesc *)(gdt->pd_base);
  for (int i = 1; i < n; i++) {
    int32_t base_addr = read_addr(sd + i);
    base_addr += new_offset;
    store_addr(sd + i, base_addr);
  }
}

static inline int32_t read_addr(struct segdesc *sd) {
  int32_t base_addr = 0;
  base_addr |= sd->sd_base_15_0;
  base_addr |= sd->sd_base_23_16 << 16;
  base_addr |= sd->sd_base_31_24 << 24;
  return base_addr;
}

static inline void store_addr(struct segdesc *sd, int32_t base_addr) {
  sd->sd_base_15_0 = (base_addr & 0x0000FFFF);
  sd->sd_base_23_16 = (base_addr & 0x00FF0000) >> 16;
  sd->sd_base_31_24 = (base_addr & 0xFF000000) >> 24;
}
