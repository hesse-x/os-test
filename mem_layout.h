#ifndef MEM_LAYOUT_H
#define MEM_LAYOUT_H

#include <stdint.h>

#define VMA_BASE 0xC0000000
#define KERNEL_VMA_BASE 0xC0100000
#define KERNEL_LMA_BASE 0x100000
#define PHY_ADDR(addr) ((uintptr_t)(addr) - VMA_BASE)

#endif // MEM_LAYOUT_H