#ifndef KERNEL_MEMLAYOUT_H_
#define KERNEL_MEMLAYOUT_H_
/* This file contains the definitions for memory management in our OS. */

/* global segment number */
#define SEG_KTEXT 1
#define SEG_KDATA 2
#define SEG_UTEXT 3
#define SEG_UDATA 4
#define SEG_TSS 5

/* global descriptor numbers */
#define GD_KTEXT ((SEG_KTEXT) << 3) // kernel text
#define GD_KDATA ((SEG_KDATA) << 3) // kernel data
#define GD_UTEXT ((SEG_UTEXT) << 3) // user text
#define GD_UDATA ((SEG_UDATA) << 3) // user data
#define GD_TSS ((SEG_TSS) << 3)     // task segment selector

#define DPL_KERNEL (0)
#define DPL_USER (3)

#define KERNEL_CS ((GD_KTEXT) | DPL_KERNEL)
#define KERNEL_DS ((GD_KDATA) | DPL_KERNEL)
#define USER_CS ((GD_UTEXT) | DPL_USER)
#define USER_DS ((GD_UDATA) | DPL_USER)

#define E820MAX 20 // number of entries in E820MAP
#define E820_ARM 1 // address range memory
#define E820_ARR 2 // address range reserved

#define KERNEL_ELF_ADDR 0x10000
#define KERNEL_ENTRY_ADDR 0x100000
#define KERNEL_BASE_VADDR 0xC0000000
#define KERNEL_ENTRY_VADDR 0xC0100000
#define KERNEL_RESERVE_MEM_SIZE 0x100000
#define KERNEL_STACK_SIZE 0x10000

#ifndef __ASSEMBLER__
#include "stdint.h"

struct e820map {
  int nr_map;
  struct {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
  } __attribute__((packed)) map[E820MAX];
};

#endif  // __ASSEMBLER__

#endif // KERNEL_MEMLAYOUT_H_
