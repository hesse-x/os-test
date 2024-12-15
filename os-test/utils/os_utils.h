#ifndef UTILS_GLOBAL_CONSTEXPR_H_
#define UTILS_GLOBAL_CONSTEXPR_H_

#define PAGE_SIZE 0x1000
#define KERNEL_ELF_ADDR 0x10000
#define KERNEL_ENTRY_ADDR 0xC0100000
#define KERNEL_STACK_BOTTOM 0x90000
#define KERNEL_STACK_SIZE 0x80000

#ifdef __ASSEMBLER__
#define SEG_NULL                                                \
    .word 0, 0;                                                 \
    .byte 0, 0, 0, 0

#define SEG_ASM(type,base,lim)                                  \
    .word (((lim) >> 12) & 0xffff), ((base) & 0xffff);          \
    .byte (((base) >> 16) & 0xff), (0x90 | (type)),             \
        (0xC0 | (((lim) >> 28) & 0xf)), (((base) >> 24) & 0xff)

#else
#include <stdint.h>
#define UNUSED(x) (void)(x)

#define L16(address) (uint16_t)((address)&0xFFFF)
#define H16(address) (uint16_t)(((address) >> 16) & 0xFFFF)
#define ALIGN(addr,size)   (((addr)+(size)-1)&(~((size)-1)))
#endif // __ASSEMBLER__
#endif // UTILS_GLOBAL_CONSTEXPR_H_
