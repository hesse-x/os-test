# 分页设计

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 页大小 | 4KB + 2MB huge pages | 内核映射用 2MB huge pages 减少 PT 层级，用户页映射用 4KB |
| 2 | 地址模型 | higher-half（VMA_BASE = 0xFFFFFFFF80000000） | 内核与用户地址空间分离，内核在所有进程 PML4 中共享映射 |
| 3 | 内核寻址 | -fPIE + RIP-relative | 物理地址和虚拟地址阶段 RIP-relative 自动给出正确地址，不需要 GOT fixup |
| 4 | 内核 huge page NX | 不标 NX 位 | 初始映射使用代码数据混合的 huge page，不细分 NX |
| 5 | 用户页 NX | 按需设置 PTE_NX | 栈页、SHM 页加 NX，代码页不加 |
| 6 | 设备映射区 | device_vma_base = ALIGN_UP(VMA_BASE + max_phys_addr, 1GB) | framebuffer 等映射到此区域 |
| 7 | NX 启用方式 | CR4.NXDE(bit 5) + EFER.NXE(bit 11) | x86-64 标准 W^X 保护，两步启用缺一无效 |
| 8 | NX 启用时机 | isr_init() 中 idt_install 之前 | 页表映射建立后启用，确保后续 PTE_NX 生效 |
| 9 | AP 启动 NX | AP trampoline EFER 写入增加 NXE | AP 需自行启用 NX，否则 AP 上 PTE_NX 位无效 |

### 地址映射

- 物理 0-1GB → 虚拟 0-1GB（identity map，PML4[0] → PDPT_ident）
- 物理 0-1GB → 虚拟 0xFFFFFFFF80000000-0xFFFFFFFFC0000000（higher-half，PML4[511] → PDPT_hh[510]）
- 2MB huge pages（PD 级别 PS=1），初始映射覆盖 1GB
- extend_mapping 动态扩展：每 1GB 物理块对应 PDPT_hh[510+n]
- 设备映射区：device_vma_base 起，framebuffer 等映射到此区域

### 地址常量

| 常量 | 值 | 说明 |
|------|-----|------|
| VMA_BASE | 0xFFFFFFFF80000000 | higher-half 基址 |
| KERNEL_VMA_BASE | 0xFFFFFFFF80100000 | 内核 VMA |
| PAGE_SIZE / PAGE_SIZE_2M | 4096 / 0x200000 | 页大小常量 |

地址转换：vaddr = paddr + VMA_BASE，PHY_ADDR(vaddr) = vaddr - VMA_BASE

### 启动分页流程

1. _start（arch/x64/start.S，物理地址）：设置物理栈 → 保存 boot_info*（r12）
2. enable_paging（arch/x64/paging.cc，物理地址运行）：构建 4 级页表（PML4→PDPT→PD）+ 加载 CR3
3. gdt_init：8 项 GDT + TSS + ltr
4. lretq → _entry64（虚拟地址）
5. _entry64：切换到虚拟地址栈 → kernel_main
6. kernel_main → init_mem：Bump 分配 frames 数组 + 扩展 PT → BFC 初始化

### RIP-relative 寻址

-fPIE 在 x86-64 使用 RIP-relative 寻址：所有符号访问通过 [rip + offset]，无需 GOT 间接。物理地址运行时自动给出物理地址；虚拟地址运行时自动给出虚拟地址。不需要 GOT fixup。纯汇编文件需手动使用 symbol(%rip)。

### Bump 分配器

arch/x64/paging.cc : Bump 分配器

极简线性分配，kernel_end 起始，仅向前增长。用于 init_mem 阶段分配 frames 数组和页表。返回虚拟地址。kernel_init_finish 后禁用。

### 页表结构

4 级页表：PML4（512 项）→ PDPT（512 项）→ PD（512 项），使用 2MB huge pages（PD 级别 PS=1），无需 PT 级别。初始映射 1GB 只需 1 个 PML4 项 + 1 个 PDPT 项 + 1 个 PD 页（4KB）。

### NX 保护（W^X）

启用流程：arch/x64/paging.cc : enable_nx() — 先设 CR4.NXDE(bit 5)，再设 EFER.NXE(bit 11)。两步缺一 PTE_NX 位被忽略。

PTE 标志常量：arch/x64/paging.h — PTE_PRESENT / PTE_RW / PTE_USER / PTE_PS / PTE_NX(bit 63)

各区域 PTE_NX 使用：

| 区域 | PTE_NX | 理由 |
|------|--------|------|
| 内核 huge page (PD 级) | 不标 | 内核代码数据混合，标 NX 会禁止内核执行 |
| 用户代码页 (0x400000+) | 不标 | 可执行代码 |
| 用户栈页 (0x7FFFFFFFD000) | 标 | 防止栈上 shellcode 执行 |
| 共享页 (SHM) | 标 | 数据传递，不应可执行 |

AP 启动时 trampoline EFER 写入增加 NXE 位，AP 在 ap_entry_c() 中不再次调用 enable_nx()（trampoline 已设置，且 CR4.NXDE 在 BSP 初始化时已全局生效）。arch/x64/ap_trampoline.S : EFER.LME+NXE 设置。

### 关键源码位置

- 页表构建：arch/x64/paging.cc : enable_paging / extend_mapping / gdt_init
- 内核入口：arch/x64/start.S : _start / _entry64
- 用户页映射：kernel/mem/user_mapping.cc : ensure_pd / ensure_pt / map_user_page_direct / map_user_pages / unmap_user_pages
- 常量定义：arch/x64/memlayout.h
- NX 启用：arch/x64/paging.cc : enable_nx
- AP trampoline NXE：arch/x64/ap_trampoline.S
- PTE 标志：arch/x64/paging.h : PTE_PRESENT / PTE_RW / PTE_USER / PTE_PS / PTE_NX

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| 4KB 页 PT 层级 | 当前内核映射仅用 2MB huge pages，用户页映射已用 4KB + PT，内核大页应支持按需拆为 4KB | 中 |
| NX 精细化 | 当前内核 huge page 不标 NX（代码数据混合），应拆分 huge page 后使 .text 可执行、.data/.bss 不可执行 | 低 |
| 页面回收 | BFC 空闲页不归还物理内存（无 balloon 驱动），QEMU 下内存只增不减 | 低 |
