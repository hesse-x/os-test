#ifndef ARCH_X64_MEMLAYOUT_H
#define ARCH_X64_MEMLAYOUT_H

// x86-64 内存布局常量（内核/用户态共享）
#define PAGE_SHIFT   12
#define PAGE_SIZE    (1 << PAGE_SHIFT)   // 4096
#define PAGE_SIZE_2M 0x200000

#define PHY_TO_PAGE(addr)  ((addr) >> PAGE_SHIFT)
#define GET_PAGE_NUM(len)  (((len) + PAGE_SIZE - 1) / PAGE_SIZE)

// Linker symbol: end of kernel image (used by allocators)
extern uint8_t kernel_end[];

#endif // ARCH_X64_MEMLAYOUT_H
