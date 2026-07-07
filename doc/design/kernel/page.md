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
| 10 | fork 页拷贝 | COW（共享物理页 + PTE_COW 标记） | fork 延迟 O(PTE 修改) vs O(memcpy)，内存不翻倍；行为与 Linux 一致 |
| 11 | COW 只读识别 | PTE_COW 软件位（bit 9） | 区分「COW 只读」（需分配新页）和「真正只读」（SIGSEGV），O(1) 判断，无需反向映射 |

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

PTE 标志常量：arch/x64/paging.h — PTE_PRESENT / PTE_RW / PTE_USER / PTE_PS / PTE_COW(bit 9) / PTE_NX(bit 63)

PTE_COW 语义（bit 9，软件位）：

| PTE 组合 | 含义 | 写 fault 处理 |
|----------|------|-------------|
| PTE_COW=1, PTE_RW=0, PTE_PRESENT=1 | COW 共享只读页 | 分配新私有页 + memcpy + refcount-- |
| PTE_COW=0, PTE_RW=0, PTE_PRESENT=1 | 真正只读页（代码段、mmap PROT_READ） | SIGSEGV |
| PTE_COW=0, PTE_RW=1, PTE_PRESENT=1 | 正常可写私有页 | 无 fault |

各区域 PTE_NX 使用：

| 区域 | PTE_NX | 理由 |
|------|--------|------|
| 内核 huge page (PD 级) | 不标 | 内核代码数据混合，标 NX 会禁止内核执行 |
| 用户代码页 (0x400000+) | 不标 | 可执行代码 |
| 用户栈页 (0x7FFFFFFFD000) | 标 | 防止栈上 shellcode 执行 |
| 共享页 (SHM) | 标 | 数据传递，不应可执行 |

AP 启动时 trampoline EFER 写入增加 NXE 位，AP 在 ap_entry_c() 中不再次调用 enable_nx()（trampoline 已设置，且 CR4.NXDE 在 BSP 初始化时已全局生效）。arch/x64/ap_trampoline.S : EFER.LME+NXE 设置。

### COW (Copy-On-Write)

fork 时物理页共享而非深拷贝，写时按需分配私有页。行为与 Linux 一致：fork COW 共享 → 写 fault 分配新页 → execve/mm_release refcount--。

**物理页引用计数**：Page 新增 `refcount_t p_refcount`（见 [mem.md](mem.md) Page 描述符）。`bfc_alloc_page(n)` 所有 n 页 p_refcount 初始化为 1（不只是首页），COW 共享时 refcount_inc，mm_release 时 refcount_dec_and_test 减到 0 才 bfc_free_page。

**fork COW 流程**（详见 [proc.md](proc.md) sys_fork）：

| 页类型 | fork 处理 | PTE 变化 | refcount 变化 |
|--------|----------|----------|-------------|
| RW 私有页（数据段/BSS/栈/mmap 匿名） | COW 共享 | 父子均：清 PTE_RW + 置 PTE_COW | p_refcount++ |
| RX/RO 页（代码段/mmap PROT_READ） | 直接共享 | 子 PTE 复制父 PTE | p_refcount++ |
| SHM / MAP_PHYSICAL 页 | 跳过 | 子 PTE 复制父 PTE | SHM 自己的 refcount |
| signal trampoline | 跳过 | 子 PTE 复制父 PTE | 全局页，不参与 |

fork 修改父进程 RW PTE 为只读+COW 后，必须刷新 TLB（当前用 `load_cr3(parent->mm->cr3)` 全 TLB flush，未来改为逐页 INVLPG）。

**COW Page Fault Handler**：

trap_dispatch #PF 判断逻辑（实现：kernel/xcore/trap.c : trap_dispatch）：

- is_user && is_present && is_write && PTE_COW → resolve_cow_fault
- is_user && is_present && is_write && !PTE_COW → SIGSEGV SEGV_ACCERR
- is_user && !is_present → SIGSEGV SEGV_MAPERR
- !is_user && is_present && is_write && PTE_COW → resolve_cow_fault (内核态 COW)
- !is_user && other → panic

resolve_cow_fault 逻辑：

1. refcount == 1 → 直接恢复 PTE_RW（对方已 exit/execve 释放引用，零拷贝优化）
2. refcount > 1 → bfc_alloc_page(1) 新页 + memcpy 旧页内容 + 更新 PTE 指向新页 + PTE_RW + 清 PTE_COW + 旧页 refcount_dec + INVLPG

**内核态 COW fault**：`copy_to_user` 写 COW 页触发内核态 #PF（is_user=0）。当前 `copy_to_user` 是直接 memcpy 无 fault 处理，COW resolve 后硬件自动重试 faulting 指令，无需修改 `copy_to_user` 本身。

**mm_release 释放策略**（详见 [proc.md](proc.md) mm_release）：

遍历所有叶 PTE，对私有页 `refcount_dec_and_test`：减到 0 才 bfc_free_page(1)；减到 >0（其他进程仍引用）不释放。SHM/MAP_PHYSICAL/signal trampoline 跳过（原有逻辑不变）。

页表中间页（PML4/PDPT/PD/PT）refcount 不参与 — fork 创建独立的页表结构，mm_release 直接 bfc_free_page 释放所有中间页表页。只有叶页走 refcount。

**execve 与 COW**：sys_execve 创建全新 PML4 + 全新物理页（refcount 均为 1），释放旧空间时共享页 refcount--（减到 >0 不释放）。典型 fork+execve 模式：子进程 execve → 共享页 refcount 2→1 → 父进程 COW fault 发现 refcount==1 → 直接恢复 PTE_RW（零拷贝）。

**并发与锁**：

| 操作 | 锁 |
|------|---|
| Page.p_refcount 操作 | 无（refcount_t 原子操作） |
| COW fault | 无（当前进程独占自己的 PML4） |
| fork 修改父 PTE | tasks_lock（fork 持锁期间父进程不可调度） |

两个进程同时 COW fault 写同一共享页：各自 alloc 新页 + memcpy + refcount_dec，结果正确（各得独立私有页）。refcount 原子操作保证计数准确。

**风险与防呆**：

- p_refcount 溢出：refcount_inc 内置 BUG_ON 检测
- COW fault OOM：bfc_alloc_page 返回 NULL → force_sig SIGSEGV
- PTE_COW vs PTE_DIRTY：COW 页 PTE_RW=0，CPU 不设 DIRTY 位；COW resolve 后恢复 PTE_RW，后续写 CPU 正常设 DIRTY。无需特殊处理
- bfc_alloc_page(n) 必须对所有 n 页设 p_refcount=1（不只是首页），否则 COW refcount_inc 在非首页上触发 BUG_ON（旧值=0）

### 关键源码位置

- 页表构建：arch/x64/paging.c : enable_paging / extend_mapping / gdt_init
- 内核入口：arch/x64/start.S : _start / _entry64
- 用户页映射：kernel/xcore/mem/user_mapping.c : ensure_pd / ensure_pt / map_user_page_direct / map_user_pages / unmap_user_pages
- 常量定义：arch/x64/memlayout.h
- NX 启用：arch/x64/paging.c : enable_nx
- AP trampoline NXE：arch/x64/ap_trampoline.S
- COW 页拷贝：kernel/xcore/mem/user_mapping.c : copy_page_table（COW 共享 + 父 PTE 修改）
- COW fault handler：kernel/xcore/trap.c : trap_dispatch #PF + resolve_cow_fault
- 物理页 refcount：kernel/xcore/atomic.h : refcount_t / kernel/xcore/mem/alloc.h : Page.p_refcount
- PTE 标志：arch/x64/paging.h : PTE_PRESENT / PTE_RW / PTE_USER / PTE_PS / PTE_COW / PTE_NX

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| 4KB 页 PT 层级 | 当前内核映射仅用 2MB huge pages，用户页映射已用 4KB + PT，内核大页应支持按需拆为 4KB | 中 |
| NX 精细化 | 当前内核 huge page 不标 NX（代码数据混合），应拆分 huge page 后使 .text 可执行、.data/.bss 不可执行 | 低 |
| 页面回收 | BFC 空闲页不归还物理内存（无 balloon 驱动），QEMU 下内存只增不减 | 低 |
