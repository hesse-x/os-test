#ifndef ARCH_X64_MEMLAYOUT_H
#define ARCH_X64_MEMLAYOUT_H

// x86-64 内存布局常量（内核/用户态共享）
#define PAGE_SHIFT   12
#define PAGE_SIZE    (1 << PAGE_SHIFT)   // 4096
#define PAGE_SIZE_2M 0x200000

// ld.so 固定基址（栈顶 0x7FFFFFFFE000 下方，固定高位，无 ASLR）
#define LD_SO_BASE   0x7FFFFF000000ULL

// 用户栈顶（与 proc.c / sched.c 中硬编码值一致）
#define USER_STACK_TOP  0x00007FFFFFFFE000ULL

#define PHY_TO_PAGE(addr)  ((addr) >> PAGE_SHIFT)
#define GET_PAGE_NUM(len)  (((len) + PAGE_SIZE - 1) / PAGE_SIZE)

// Linker symbol: end of kernel image (used by allocators)
extern uint8_t kernel_end[];

#endif // ARCH_X64_MEMLAYOUT_H
