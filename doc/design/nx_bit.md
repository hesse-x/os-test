# NX 位（No-Execute）设计

> **已实现**。CR4.NXDE + EFER.NXE 已启用，PTE bit 63 标记不可执行页，用户栈页映射时加 PTE_NX。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | NX 启用方式 | CR4.NXDE(bit 5) + EFER.NXE(bit 11) | x86-64 标准 W^X 保护机制，两步启用 |
| 2 | 启用时机 | `isr_init()` 中，`idt_install()` 之前 | 页表映射建立后启用，确保后续 PTE_NX 位生效 |
| 3 | 内核 huge page | 不标 PTE_NX | 内核代码数据混合在同一 2MB huge page，标记 NX 会禁止内核代码执行 |
| 4 | 用户栈页 | 标记 PTE_NX | 防止栈上代码执行，基本 W^X 保护 |
| 5 | 用户代码页 | 不标 PTE_NX | 可执行，正常用户程序代码 |
| 6 | 共享页 | 标记 PTE_NX | 共享页用于数据传递（键盘/磁盘缓冲区），不应可执行 |
| 7 | AP trampoline | EFER 写入增加 NXE | AP 启动时也需启用 NX，否则 AP 上的 PTE_NX 位无效 |
| 8 | PTE 常量化 | 定义 PTE_PRESENT/PTE_RW/PTE_USER/PTE_PS/PTE_NX | 替代硬编码魔术数字，可读性和维护性 |

## enable_nx() 实现（arch/x64/paging.cc）

```c
extern "C" void enable_nx() {
  // Enable CR4.NXDE (bit 5)
  uint64_t cr4 = read_cr4();
  cr4 |= (1ULL << 5);
  write_cr4(cr4);

  // Enable EFER.NXE (bit 11)
  uint64_t efer = rdmsr(MSR_EFER);
  efer |= EFER_NXE;
  wrmsr(MSR_EFER, efer);
}
```

两步启用：CR4.NXDE 允许 PTE bit 63 作为 NX 标志，EFER.NXE 使 NX 标志生效。缺任一步 PTE_NX 位被忽略。

## PTE 标志常量（arch/x64/paging.h）

```c
#define PTE_PRESENT  (1ULL << 0)
#define PTE_RW       (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_PS       (1ULL << 7)   // 2MB huge page at PD level
#define PTE_NX       (1ULL << 63)  // No-execute
```

## 各区域 PTE_NX 使用

| 区域 | PTE_NX | 理由 |
|------|--------|------|
| 内核 huge page (PD 级) | 不标 | 内核代码数据混合，标 NX 会禁止内核执行 |
| 用户代码页 (0x400000+) | 不标 | 可执行代码 |
| 用户栈页 (0x7FFFFFFFD000) | 标 | 防止栈溢出后执行 shellcode |
| 共享页 (0x500000-0x502000) | 标 | 数据传递，不应可执行 |

## AP 启动中的 NX

AP trampoline（`arch/x64/ap_trampoline.S`）中 EFER 写入增加 NXE 位：

```asm
# EFER.LME = 1 + EFER.NXE = 1
movl $0xC0000080, %ecx
rdmsr
orl $(1 << 8) | $(1 << 11), %eax    # LME + NXE
wrmsr
```

AP 在 `ap_entry_c()` 中不再次调用 `enable_nx()`，因为 trampoline 已设置 EFER.NXE，且 CR4.NXDE 在 BSP 初始化时已全局生效（CR4 是 per-CPU 寄存器，但 BSP 设置的 NXDE 不会被 AP trampoline 清除——AP 在进入长模式前需自行设置 CR4.PAE，此时应同时设 NXDE）。