# KASAN 消毒器

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 启用方式 | `build.sh --sanitizer` → cmake `-DSANITIZE=1`，宏 `SANITIZER` 控制 | 不带时 build.sh 显式传 `-DSANITIZE=0`，防止 CMake 缓存保留旧值 |
| 2 | Runtime | 自定义实现（`kasan.c`），不链接 `libclang_rt.kasan-x86_64.a` | freestanding 内核无法链接 GCC runtime；非 SANITIZE 构建时 kasan.c 不编译，kasan.h API 展开为内联空操作 |
| 3 | Shadow 页粒度 | 4KB pages | 2MB huge pages 要求 2MB 对齐 + 512 页连续，BFC 不保证对齐分配；4KB 每页只需 `bfc_alloc_page(1)` |
| 4 | 用户态地址 | `copy_from_user/copy_to_user`（整函数 `no_sanitize`） | 用户地址 shadow 非 canonical，inline shadow load 会 #GP；内部调 `__memcpy` 不走 KASAN 检查 |
| 5 | Identity map | 保留 identity mapping（PML4[0]），所有 identity-map 代码必须 `no_sanitize` | shadow 地址非 canonical，漏标时 #GP handler 诊断捕获 |
| 6 | PML4[503] 跨进程共享 | `process_create_elf` 中显式复制 `new_pml4[503] = pml4[503]` | shadow 区域在 PML4[503]，不属于 PML4[511] 寻址范围；遗漏则 IRET 后内核访问 shadow → PF → triple fault |
| 7 | Slab 红区 | 当前不扩展（slot_size = obj_size） | KASAN 对 slab 内部越界检测能力有限；红区扩展需改 slab 分配器 |

### Shadow 内存布局

公式：`shadow_addr = (addr >> 3) + KASAN_SHADOW_OFFSET`

KASAN_SHADOW_OFFSET：`0xDFFFFB9000000000`（推导：shadow(VMA_BASE) = PML4[503] = `0xFFFFFB8000000000`）

| 真实地址范围 | Shadow 地址范围 | 大小 |
|------------|---------------|------|
| `0xFFFFFF8000000000` (VMA_BASE) | `0xFFFFFB8000000000` | - |
| `0xFFFFFFFFFFFFFFFF` | `0xFFFFFB8FFFFFFFFF` | 见下 |

Shadow PML4 索引：503（与 direct map 所在 PML4[511] 不同 slot，不重叠）。shadow 覆盖范围在 `kasan_init` 中按 `total_page_frames` 动态计算（只 shadow 实际物理 RAM，而非整个 64GB direct-map 窗口），shadow 大小 = total_page_frames*PAGE_SIZE/8（如 4GB RAM → 512MB shadow）。Identity map 和用户态地址的 shadow 均落在非 canonical 区域（4-level paging 128TB 固有限制），内核访问这些区域必须走 `no_sanitize` 或 `copy_from_user/copy_to_user`。

### Shadow 值语义

| Shadow 值 | 含义 | 对应真实内存状态 |
|-----------|------|----------------|
| 0x00 | 全部 8 字节可访问 | 正常分配的对象 |
| 0x01-0x07 | 前 N 字节可访问 | 对象末尾对齐偏移 |
| 0xFD | 红区 / padding | kmalloc 对象后的 padding |
| 0xFE | 已释放 | kfree 后的对象 |
| 0xFF | 不可访问 | 未映射/保留区域/初始化默认值 |

### Shadow 映射初始化

在 `kernel_init_finish()` 后、`slab_init()` 前调用 `kasan_init()`：

1. 计算 shadow 覆盖范围，向上对齐到 PAGE_SIZE（65536 页 = 256MB）
2. 逐页从 BFC 分配，通过 `kasan_ensure_pdpt/pd/pt` 在 PML4[503] 下建立 4KB 页三级页表结构
3. Flush TLB（reload CR3）
4. 先 `__memset` 全标 0xFF → unpoison 内核映像 → 遍历 `__kasan_globals` 段 poison 全局红区
5. 设置 `kasan_ready = true`

BFC 页自动 unpoison：`bfc_alloc_page`/`bfc_alloc_page_low` 返回时调用 `kasan_bfc_alloc()`（内部 `if (!kasan_ready) return;` 防递归）。

kasan_init 及其调用链（BFC alloc_page、kasan runtime 函数）都必须 `no_sanitize("kernel-address")`，因为 shadow 页表在 kasan_init 执行期间尚未映射，inline shadow load 会 #PF。

实现：`kernel/xcore/mem/kasan.c` : kasan_init；初始化顺序见 `kernel/kernel.c` : kernel_main

### 编译器插桩

GCC `-fsanitize=kernel-address` 在每个 load/store 前插入 inline shadow 检查：shadow load（`movzx edx, byte [rax]`）无条件执行，shadow!=0 时调 `__asan_loadN/__asan_storeN`。`kasan_ready = false` 只控制 runtime 函数跳过，不影响 inline shadow load——shadow 未映射时必须 `no_sanitize`。

### 自定义 Runtime

`kernel/xcore/mem/kasan.c` 实现（`#ifdef SANITIZER` 条件编译，全部 `no_sanitize`）：

核心函数：`kasan_check_read/write`（逐字节检查 shadow，跳过 < VMA_BASE 地址）→ `__asan_load/store1/2/4/8/16/N`（固定大小变体）→ `__asan_memcpy/memset/memmove`（检查两侧 + 调 `__memcpy/__memset/__memmove`）

分配器 hook：`kasan_slab_alloc/free`（unpoison/poison 为 0xFE + double-free 检测）、`kasan_bfc_alloc/free`（unpoison/poison）

全局红区：`__asan_register_globals/unregister_globals`

用户边界：`copy_from_user/copy_to_user`（整函数 `no_sanitize`，调 `__memcpy` 不检查用户侧）

shadow 操作 API：`kasan_unpoison_shadow/kasan_poison_shadow`

### #GP handler 诊断

`trap_dispatch` #GP 处理：内核态 #GP(0) 且 `kasan_shadow_exists()` 为 true 时输出诊断提示（"possible shadow access to non-canonical address, check __user pointer usage"），让漏标 `no_sanitize` 的身份/用户地址访问不再 silent triple fault。

### KASAN 报告

检测到非法访问时串口输出：错误类型（redzone/freed/不可访问）+ 地址/大小/shadow 值 + RBP 链回溯（最多 16 帧，需 `-fno-omit-frame-pointer`），最后 `halt()`。

实现：`kernel/xcore/mem/kasan.c` : kasan_report

### 与分配器集成

| 分配器 | kmalloc 返回前 | kfree 入口 |
|--------|---------------|-----------|
| Slab | `kasan_slab_alloc(obj, cache->obj_size)` → unpoison | `kasan_slab_free(ptr, size)` → poison 0xFE + double-free 检测 |
| BFC | `kasan_bfc_alloc(ptr, npages * PAGE_SIZE)` → unpoison | `kasan_bfc_free(ptr, n_pages * PAGE_SIZE)` → poison 0xFE |

BFC 页自动 unpoison：`bfc_alloc_page` 返回时调 `kasan_bfc_alloc()`。`bfc_free_page` 未自动 poison（依赖下次分配时 unpoison 覆盖）。

### memcpy/memset/memmove 三层架构

| 层 | 函数 | KASAN 行为 |
|---|---|---|
| 原始拷贝 | `__memcpy/__memset/__memmove`（arch/x64/utils.h） | `no_sanitize`，纯字节拷贝 |
| KASAN 包装 | `__asan_memcpy/memset/memmove`（kernel/xcore/mem/kasan.c） | `no_sanitize` + kasan_check 两侧 + 调原始函数 |
| 用户边界 | `copy_from_user/copy_to_user` | `no_sanitize` + 调 `__memcpy`（不检查用户侧） |

### no_sanitize 标注清单

九类必须 `no_sanitize("kernel-address")` 的函数/类别：

A. 早期启动（shadow 未映射）：`enable_paging/gdt_init/extend_mapping/bump_*`/`init_mem/kernel_init_finish/acpi_*`/`kasan_init`
B. BFC 核心（kasan_init 期间 shadow 未映射 + auto-unpoison 调 kasan）：`bfc_alloc_page/free_page`/`page_to_phys/phys_to_virt`
C. KASAN runtime（防递归）：所有 `__asan_*` 函数 + `kasan_poison/unpoison/check/report/slab_*`/`bfc_*`
D. MMIO（设备寄存器不在 shadow 覆盖范围）：`readl/writel`/`lapic_read/write/eoi`/`ioapic_read/write`/PCI ECAM/BAR
E. Slab 核心（freelist 指针在 shadow 标记 slot 内读写）：`slab_init/kmalloc/kfree/kcalloc/krealloc`/`partial_add/remove`
F. Bulk 内存操作（被 __asan_* 替代）：`__memcpy/__memset/__memmove`
G. 用户态地址边界：`copy_from_user/copy_to_user`
H. 调度器（锁 + 上下文切换 + CR3，KASAN 误报 → halt 卡死）：`schedule/idle_entry`
I. 其他：`smp_init_cpu/apply_cpu/ap_entry_c/smp_boot_aps/udelay`/`init_fb/ahci_init/bounce_to_user_pages`/`display_mmap_handler/flip`/`ensure_pd/ensure_pt_in_pd/map_user_page_*`/`unmap_user_pages`

汇编文件（start.S, vectors.S, trapentry.S, ap_trampoline.S）不受编译器插桩影响，无需标注。

### 链接脚本适配

保留 `__kasan_globals` section（`KEEP(*(__kasan_globals))`）供全局红区注册。`.eh_frame` 保持丢弃（`__asan_handle_no_return` 空实现）。

### 文件清单

| 文件 | 内容 |
|------|------|
| `kernel/xcore/mem/kasan.h` | KASAN API 声明 + 非 SANITIZE 模式内联空实现 + `copy_from_user/copy_to_user` |
| `kernel/xcore/mem/kasan.c` | KASAN runtime（`#ifdef SANITIZER` 条件编译，__asan_* + kasan_init + shadow 映射 + kasan_check + report + 分配器 hook） |

### 与其他模块的关系

| 模块 | 关系 | 文档 |
|------|------|------|
| 内存管理 | BFC/Slab 分配器 hook | [mem.md](mem.md) |
| 进程管理 | PML4[503] 跨进程复制 | [proc.md](proc.md) |
| 分页 | Shadow 页表映射 + PML4[503] | [page.md](page.md) |
| 调度器 | schedule() no_sanitize | [schedule.md](schedule.md) |
| ACPI | identity-map 遍历 no_sanitize | [smp.md](smp.md) |

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| copy_from_user page fault | 当前实现假设用户地址有效，无 invalid address 处理；需 exception table 机制 | 中 |
| Slab 红区扩展 | slot_size = obj_size，KASAN 对 slab 内部越界检测能力有限；需 obj_size + redzone 扩展 | 低 |
| SHM 页 re-poison | BFC auto-unpoison 后 SHM 页 shadow 为 0x00，理想应为 0xFF（内核不读写共享页内容） | 低 |
| bfc_free_page 自动 poison | 释放页后未 poison，依赖下次分配时覆盖 | 低 |
| KCSAN | 数据竞争检测器尚未实现 | 低 |
