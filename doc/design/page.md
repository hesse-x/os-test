# 分页设计

> **已实现**。当前架构为 x86-64 / UEFI / 4 级页表 / 2MB huge pages / `-fPIE` + RIP-relative。以下描述当前实现；末尾附 x86-32 阶段的历史方案供参考。

## 当前实现（x86-64）

### 地址映射

- 物理 0-1GB → 虚拟 0-1GB（identity map，PML4[0] → PDPT_ident）
- 物理 0-1GB → 虚拟 0xFFFFFFFF80000000-0xFFFFFFFFC0000000（higher-half，PML4[511] → PDPT_hh[510]）
- 使用 2MB huge pages（PD 级别 PS=1），初始映射覆盖 1GB
- `extend_mapping` 动态扩展：每 1GB 物理块对应 PDPT_hh[510+n]
- 设备映射区：`device_vma_base = ALIGN_UP(VMA_BASE + max_phys_addr, 1GB)`，framebuffer 等映射到此区域

### 地址常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `VMA_BASE` | `0xFFFFFFFF80000000` | higher-half 基址 |
| `KERNEL_VMA_BASE` | `0xFFFFFFFF80100000` | 内核 VMA |
| `PTE_PRESENT/RW/USER/PS/NX` | `paging.h` | PTE 标志常量 |

地址转换：`vaddr = paddr + VMA_BASE`，`PHY_ADDR(vaddr) = vaddr - VMA_BASE`

### 启动分页流程

1. `_start`（start.S，物理地址）：设置物理栈 → 保存 `boot_info*`（r12）
2. `enable_paging`（paging.cc，物理地址运行）：构建 4 级页表（PML4→PDPT→PD）+ 加载 CR3
3. `gdt_init`：8 项 GDT + TSS + `ltr`
4. `lretq` → `_entry64`（虚拟地址）
5. `_entry64`：切换到虚拟地址栈 → `kernel_main`
6. `kernel_main` → `init_mem`：Bump 分配器分配 frames 数组 + 扩展 PT → BFC 初始化

### RIP-relative 寻址

`-fPIE` 在 x86-64 使用 RIP-relative 寻址（非 x86-32 的 GOTOFF）：
- 所有符号访问通过 `[rip + offset]` 形式，无需 GOT 间接
- 物理地址运行时 RIP-relative 自动给出物理地址；虚拟地址运行时自动给出虚拟地址
- 不需要 GOT fixup
- 纯汇编文件需手动使用 `symbol(%rip)` 或计算偏移

### Bump 分配器

极简线性分配，`kernel_end` 起始，仅向前增长。定义在 `arch/x64/paging.cc`。用于 `init_mem` 阶段分配 frames 数组和页表。返回虚拟地址。`kernel_init_finish` 后禁用。

### 页表结构

4 级页表：PML4（512 项）→ PDPT（512 项）→ PD（512 项），使用 2MB huge pages（PD 级别 PS=1），无需 PT 级别。初始映射 1GB 只需 1 个 PML4 项 + 1 个 PDPT 项 + 1 个 PD 页（4KB）。

### 内核 huge page 不标 NX

初始映射使用代码数据混合的 huge page，不标 NX 位。用户页映射时按需设置 PTE_NX（栈页、SHM 页加 NX，代码页不加）。

---

## 历史：x86-32 阶段分页方案

### Higher-Half Kernel PIE（GOTOFF）

x86-32 上 `-fPIE` 使用 GOTOFF 寻址：`call __x86.get_pc_thunk.bx` 获取当前 EIP → `add ebx, GOT_base - $` 推导 GOT 基址 → `[ebx + symbol@GOTOFF]` 直接偏移访问。不需要 GOT fixup，分页前 GOTOFF 给出物理地址，分页后给出虚拟地址。

VMA_BASE = `0xC0000000`，KERNEL_VMA_BASE = `0xC0100000`，KERNEL_LMA_BASE = `0x100000`。4KB 小页，PD entry 0 和 768 共享同一个页表。链接脚本所有段 VMA=0xC0100000，LMA 从 0x100000 连续排列（AT() 指定）。

### 分阶段映射

将分页设置分为两个阶段：
1. 物理地址阶段：`enable_page()` 只做最小映射（identity 4MB + higher-half 4MB），确保能跳转到虚拟地址
2. 虚拟地址阶段：`init_mem()` 解析 mmap、bump 分配 frames 数组 + 扩展 PT、初始化 BFC

栈平移：`addl $VMA_BASE, %esp` 将 ESP 从物理地址平移到虚拟地址（指向同一物理内存），然后 `jmp *kernel_main_higher`。multiboot info 地址通过全局变量传递。

初始 4MB 映射必须覆盖所有 bump 分配（frames 数组 + 扩展 PT 物理地址须在 0-4MB 范围内）。512MB RAM 下约需 2.5MB，3MB 空间够用。
