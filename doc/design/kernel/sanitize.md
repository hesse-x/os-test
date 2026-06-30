# KASAN 消毒器设计

## 概述

为内核添加 KASAN（Kernel Address Sanitizer），通过 `build.sh --sanitizer` 启用（cmake 选项 `-DSANITIZE=1`），宏名 `SANITIZER` 控制。KASAN 检测越界访问、use-after-free 等内存安全问题。

## 构建集成

### cmake 选项

```cmake
if(SANITIZE)
  set(KASAN_CFLAGS "-fsanitize=kernel-address" "-DKASAN_SHADOW_OFFSET=0xDFFFFC0000000000ULL" "-DSANITIZER=1")
endif()
```

KASAN 标志仅通过 `kernel_rules.cmake` 作用于内核 OBJECT 库，不影响用户态 ELF 编译。

用户通过 build.sh 启用：`./build.sh --sanitizer`，底层传 cmake `-DSANITIZE=1`。不带 `--sanitizer` 时 build.sh 显式传 `-DSANITIZE=0`，防止 CMake 缓存保留上次构建的值。

### 链接

KASAN runtime 函数（`__asan_loadN`、`__asan_memcpy` 等）在 `kernel/mem/kasan.c` 中自定义实现（`#ifdef SANITIZER` 条件编译），不链接 GCC 的 `libclang_rt.kasan-x86_64.a`。非 SANITIZE 构建时 `kasan.c` 不参与编译，`kasan.h` 中的所有 API 展开为内联空操作，二进制无任何 KASAN 代码。

## Shadow 内存布局

**公式**：`shadow_addr = (addr >> 3) + KASAN_SHADOW_OFFSET`

**KASAN_SHADOW_OFFSET**：`0xDFFFFC0000000000`

推导：
- 内核 higher-half 映射从 `VMA_BASE = 0xFFFFFFFF80000000` 起
- shadow(VMA_BASE) = `(0xFFFFFFFF80000000 >> 3) + 0xDFFFFC0000000000`
- = `0x1FFFFFFF0000000 + 0xDFFFFC0000000000`
- = `0xFFFFFBFFF0000000`（PML4[503]）

**Shadow 覆盖范围**：

| 真实地址范围 | Shadow 地址范围 | 大小 |
|------------|---------------|------|
| `0xFFFFFFFF80000000` (VMA_BASE) | `0xFFFFFBFFF0000000` | - |
| `0xFFFFFFFFFFFFFFFF` (高半部末端) | `0xFFFFFBFFFFFFFFFF` | 256MB |

**Shadow PML4 索引**：503（独立于内核 PML4[511]，需要在新进程创建时显式复制）。

**非 canonical shadow 地址区域**：identity map（0-1GB）和用户态地址空间的 shadow 都落在非 canonical 地址空间。4-level paging 下不存在同时让 identity map 和 higher-half shadow 都 canonical 的 offset——这是 128TB canonical 空间的固有限制。因此：

- **Identity map 区域**：保留 identity mapping（PML4[0]），所有 identity-map 代码必须 `no_sanitize`。漏标时 inline shadow load 会 #GP，由 #GP handler 诊断捕获。
- **用户态地址**：内核访问用户虚拟地址必须通过 `copy_from_user/copy_to_user`（见下文"用户态地址访问"），直接访问会 #GP。

### #GP handler 诊断

在 `trap_dispatch` 的 #GP 处理中增加 KASAN 诊断：当内核态 #GP(0) 发生且 `kasan_shadow_exists()` 为 true 时，输出：

```
KASAN: possible shadow access to non-canonical address
  Check if __user pointer was used without copy_from_user/to_user
```

这让漏标 `no_sanitize` 的 identity-map 或用户地址访问不再 silent triple fault，而是给出可定位的诊断信息。

## Shadow 映射初始化

在 `kernel_init_finish()` 后、`slab_init()` 前调用 `kasan_init()`：

1. **计算 shadow 覆盖范围**：`shadow_start = KASAN_MEM_TO_SHADOW(VMA_BASE)`，`shadow_end = KASAN_MEM_TO_SHADOW(0xFFFFFFFFFFFFFFFF)`，向上对齐到 PAGE_SIZE（65536 页 = 256MB）
2. **分配物理页并映射**：逐页从 BFC 分配，通过 `kasan_ensure_pdpt/pd/pt` 在 PML4[503] 下建立 4KB 页三级页表结构
3. **Flush TLB**：reload CR3 使 shadow 映射生效
4. **初始化 shadow 内容**：
   - **先全标 0xFF**：`__memset` 把整个 shadow 区域标为不可访问
   - **Unpoison 内核映像**：`kasan_unpoison_shadow(0xFFFFFFFF80100000, kernel_end - 0xFFFFFFFF80100000)`
5. **遍历 `__kasan_globals` 段**：`kasan_poison_globals()` 对每个全局变量 unpoison 本体 + poison 红区
6. **设置 `kasan_ready = true`**：启用 KASAN 检查

**BFC 页自动 unpoison**：`bfc_alloc_page`/`bfc_alloc_page_low` 返回时调用 `kasan_bfc_alloc()`，自动 `kasan_unpoison_shadow`。`kasan_bfc_alloc` 内部检查 `if (!kasan_ready) return;` 防止 kasan_init 期间递归。

**4KB pages 选择的理由**：每个 shadow 物理页只需 `bfc_alloc_page(1)`，无需对齐无需连续。2MB huge pages 要求物理地址 2MB 对齐 + 512 页连续，BFC 不保证对齐分配。

**kasan_init 及其调用链的 no_sanitize 要求**：kasan_init 本身、BFC alloc_page（被 kasan_init 调用）、以及 kasan 内部 runtime 函数都必须 `no_sanitize("kernel-address")`，因为 shadow 页表在 kasan_init 执行期间尚未映射，任何 inline shadow load 会 #PF。

## Shadow PML4 跨进程共享

**需要显式复制 PML4[503]**。shadow 区域落在 PML4[503]（`0xFFFFFBFFF0000000`），不属于 PML4[511] 的寻址范围。`process_create_elf` 中在 `new_pml4[511] = pml4[511]` 之后必须加：

```c
#ifdef SANITIZER
new_pml4[503] = pml4[503];
#endif
```

这确保用户进程在 syscall 上下文中能访问影子内存。如果遗漏，IRET 到用户态后内核访问影子内存会触发 page fault → triple fault。

## 编译器插桩

GCC `-fsanitize=kernel-address` 在每个 load/store 前插入 inline shadow 检查：

```asm
mov rax, addr
shr rax, 3
add rax, KASAN_SHADOW_OFFSET
movzx edx, byte [rax]       ; load shadow byte ← 无条件执行
test edx, edx
jne .Lcheck                  ; shadow != 0 → 调用详细检查
.Lcheck:
  call __asan_loadN / __asan_storeN
```

**重要**：inline shadow load（`movzx edx, byte [rax]`）是无条件执行的，发生在 `__asan_loadN` 调用之前。`kasan_ready = false` 只控制 runtime 函数的跳过，**不影响 inline shadow load**。因此：
- shadow 未映射时（kasan_init 前）：inline shadow load 会 #PF → 必须用 `no_sanitize` 属性
- shadow 地址非 canonical 时（identity map、用户态地址）：inline shadow load 会 #GP → 必须用 `no_sanitize` 属性或通过 `copy_from_user/copy_to_user` 路径

## 自定义 Runtime

在 `kernel/mem/kasan.c` 中实现以下函数（替代 GCC runtime 库），全部 `#ifdef SANITIZER` 条件编译，全部标记 `__attribute__((no_sanitize("kernel-address")))`：

```c
// 核心检查函数
void kasan_check_read(const void *addr, size_t size);
void kasan_check_write(const void *addr, size_t size);

// __asan_load/store 固定大小变体（1/2/4/8/16/N + _noabort 版本）
void __asan_load1(unsigned long addr);  // → kasan_check_read(addr, 1)
void __asan_store1(unsigned long addr); // → kasan_check_write(addr, 1)
// ... __asan_load2/4/8/16/N, __asan_store2/4/8/16/N, _noabort 版本

// memcpy/memset/memmove 拦截
void *__asan_memcpy(void *dst, const void *src, size_t n);   // kasan_check 两侧 + __memcpy
void *__asan_memset(void *dst, int c, size_t n);             // kasan_check 写侧 + __memset
void *__asan_memmove(void *dst, const void *src, size_t n);  // kasan_check 两侧 + __memmove

// 全局变量红区注册
void __asan_register_globals(const struct kasan_global *globals, size_t n);
void __asan_unregister_globals(const struct kasan_global *globals, size_t n);

// 栈红区管理（空实现）
void __asan_handle_no_return(void);
void __asan_before_dynamic_init(const char *module);
void __asan_after_dynamic_init(void);

// shadow 操作 API
void kasan_unpoison_shadow(const void *addr, size_t size);
void kasan_poison_shadow(const void *addr, size_t size, uint8_t value);

// 分配器 hook
void kasan_slab_alloc(const void *object, size_t size);
void kasan_slab_free(const void *object, size_t size);
void kasan_bfc_alloc(const void *addr, size_t size);
void kasan_bfc_free(const void *addr, size_t size);

// 诊断
bool kasan_shadow_exists(void);
size_t copy_from_user(void *dst, const void __user *src, size_t size);
size_t copy_to_user(void __user *dst, const void *src, size_t size);
```

### kasan_check_read/write 实现

逐字节检查影子内存，跳过非内核地址：

```c
void kasan_check_read(const void *addr, size_t size) {
    if (!kasan_ready) return;
    uint64_t a = (uint64_t)addr;
    if (a < 0xFFFFFFFF80000000ULL) return;  // 跳过用户态地址
    uint8_t *shadow = KASAN_MEM_TO_SHADOW(addr);
    for (size_t i = 0; i < size; i++) {
        uint8_t s = shadow[i >> 3];
        if (s == 0) continue;
        if (s >= 0xF9) { kasan_report(addr, size, false); return; }
        size_t offset_in_block = i & 7;
        if (offset_in_block >= s) { kasan_report(addr, size, false); return; }
    }
}
```

## no_sanitize 标注完整清单

以下函数/类别必须加 `__attribute__((no_sanitize("kernel-address")))`：

**A. 早期启动（shadow 未映射，inline shadow load 会 #PF）**

| 函数 | 原因 |
|------|------|
| `enable_paging` / `gdt_init` (arch/x64/paging.c) | 构建页表，shadow 页表尚未存在 |
| `extend_mapping` (arch/x64/paging.c) | 早期扩展页表 |
| `bump_alloc` / `bump_init_phys` / `bump_disable` / `bump_end_phys` (arch/x64/paging.c) | init_mem 前期，shadow 未映射 |
| `init_mem` | 初始化 BFC，shadow 初始化在其后 |
| `kernel_init_finish` | 调用 bump_disable，在 kasan_init 前 |
| `acpi_init` / `acpi_find_table` | 遍历 identity-mapped ACPI 表，shadow 地址非 canonical |
| `kasan_init` (kernel/mem/kasan.c) | shadow 映射尚未完成 |

**B. BFC 核心（kasan_init 调 BFC 时 shadow 未映射；后续 BFC 因 auto-unpoison 调 kasan 也需 no_sanitize）**

| 函数 | 原因 |
|------|------|
| `bfc_alloc_page` / `bfc_alloc_page_low` (kernel/mem/alloc.c) | kasan_init 期间 shadow 未映射；后续 auto-unpoison 调 kasan 函数 |
| `bfc_free_page` / `bfc_free_page_nums` (kernel/mem/alloc.c) | auto-poison 调 kasan 函数 |
| `page_to_phys` / `phys_to_virt` | 简单地址转换，无需检查 |

**C. KASAN runtime（防递归 + shadow-of-shadow 未映射）**

| 函数 | 原因 |
|------|------|
| 所有 `__asan_*` 函数（含 `__asan_memcpy`/`__asan_memset`/`__asan_memmove`） | 防递归 shadow 检查 |
| `kasan_poison_shadow` / `kasan_unpoison_shadow` | 写 shadow 字节，shadow-of-shadow 未映射 |
| `kasan_check_read` / `kasan_check_write` / `kasan_report` | 同上 |
| `kasan_slab_alloc` / `kasan_slab_free` / `kasan_bfc_alloc` / `kasan_bfc_free` | 操作 shadow |

**D. MMIO（设备寄存器不在 shadow 覆盖范围）**

| 函数 | 原因 |
|------|------|
| `readl` / `writel` (arch/x64/utils.h) | 设备寄存器不在 shadow 覆盖范围 |
| `lapic_read` / `lapic_write` / `lapic_eoi` (arch/x64/apic.h) | LAPIC MMIO |
| `ioapic_read` / `ioapic_write` (arch/x64/apic.h) | I/O APIC MMIO |
| PCI ECAM/BAR 相关函数 (kernel/pci.c) | PCIe ECAM MMIO |

**E. Slab 核心（操作已 freed 对象的 freelist 指针，shadow 不一致）**

| 函数 | 原因 |
|------|------|
| `slab_init` / `slab_page_init` / `kmalloc` / `kfree` / `kcalloc` / `krealloc` (kernel/mem/slab.c) | freelist 指针在 shadow 标记的 slot 内读写 |
| `partial_add` / `partial_remove` | slab 内部链表操作 |

**F. Bulk 内存操作（被 __asan_* 替代，原函数需 no_sanitize）**

| 函数 | 原因 |
|------|------|
| `__memcpy` / `__memset` / `__memmove` (arch/x64/utils.h) | `__asan_memcpy` 等调用这些原始函数做实际拷贝 |

**G. 用户态地址边界（用户地址 shadow 非 canonical，inline load 会 #GP）**

| 函数 | 原因 |
|------|------|
| `copy_from_user` / `copy_to_user` (kernel/mem/kasan.c / kasan.h) | 内核与用户态之间的数据拷贝边界 |

**H. 调度器（锁 + 上下文切换路径，KASAN 检查可能 halt 或死锁）**

| 函数 | 原因 |
|------|------|
| `schedule()` (kernel/proc.c) | 中断禁用 + 自旋锁 + switch_to 跨 CR3，KASAN 误报 → halt 卡死 |
| `idle_entry()` (kernel/proc.c) | 循环调用 schedule() |

**I. 其他需要 no_sanitize 的函数**

| 函数 | 原因 |
|------|------|
| `smp_init_cpu` / `smp_apply_cpu` / `ap_entry_c` / `smp_boot_aps` / `udelay` (arch/x64/smp.c) | per-CPU GDT/TSS + identity trampoline + LAPIC MMIO |
| `init_fb` (kernel/fb.c) | Framebuffer MMIO，init_mem 内部调用 |
| `ahci_init` / `bounce_to_user_pages` (kernel/ahci.c) | DMA 缓冲区 + MMIO |
| `display_mmap_handler` / `display_flip` (kernel/display.c) | Framebuffer MMIO |
| `ensure_pd` / `ensure_pt_in_pd` / `map_user_page_direct` / `map_user_pages` / `unmap_user_pages` (kernel/mem/user_mapping.c) | 页表操作 |

汇编文件（start.S, vectors.S, trapentry.S, ap_trampoline.S）不受编译器插桩影响，无需标注。

## 用户态地址访问

内核代码访问用户态虚拟地址（如 sys_read 的用户 buffer）时，shadow 地址落在非 canonical 空间，inline shadow load 会 #GP。所有内核→用户态的数据传输必须通过 `copy_from_user` / `copy_to_user`：

```c
// SANITIZE 模式 (kasan.c)
__attribute__((no_sanitize("kernel-address")))
size_t copy_from_user(void *dst, const void __user *src, size_t size) {
    __memcpy(dst, (const void __force *)src, size);
    return 0;  // 返回未拷贝字节数，0 表示全部成功
}

// 非 SANITIZE 模式 (kasan.h)
static inline size_t copy_from_user(void *d, const void __user *s, size_t n) {
    __memcpy(d, (const void __force *)s, n); return 0;
}
```

设计要点：
- 整函数 `no_sanitize` → 访问用户地址不走 inline shadow 检查（避免 #GP）
- 返回值：0 表示全部成功，非零表示未拷贝字节数（与 Linux `copy_from_user` 一致）
- 用户侧不做检查（用户地址无 shadow，也无法映射 shadow）
- 内部调用 `__memcpy`（原始拷贝）而非 `__asan_memcpy`（会检查用户侧 shadow → #GP）

替换原则：所有 `__user` 地址的 `__memcpy` 必须替换为 `copy_from_user/copy_to_user`。`__force` cast 直接 memcpy 必须替换。

## Shadow 值语义

| Shadow 值 | 含义 | 对应真实内存状态 |
|-----------|------|----------------|
| 0x00 | 全部 8 字节可访问 | 正常分配的对象 |
| 0x01-0x07 | 前 N 字节可访问 | 对象末尾对齐偏移 |
| 0xFD | 红区 / padding | kmalloc 对象后的 padding |
| 0xFE | 已释放 | kfree 后的对象 |
| 0xFF | 不可访问 | 未映射/保留区域/初始化默认值 |

## KASAN 报告

检测到非法访问时，通过串口输出报告然后 halt：

```
=== KASAN ERROR ===
  out-of-bounds WRITE (global-redzone)
  addr=0xFFFFFFFF80123456 size=4 shadow=0xFD
  backtrace:
    0xFFFFFFFF8010XXXX
    0xFFFFFFFF8010YYYY
===================
```

`kasan_report` 实现：读取 shadow 字节判断错误类型（redzone/freed/不可访问），打印地址/大小/shadow 值，RBP 链回溯最多 16 帧，最后 `halt()`。

## 链接脚本适配

1. **保留全局红区 section**：
   ```
   __start_kasan_globals = .;
   .kasan_globals : AT(ADDR(.kasan_globals) - 0xFFFFFFFF80000000) {
       KEEP(*(__kasan_globals))
   }
   __stop_kasan_globals = .;
   ```
2. **`.eh_frame` 保持丢弃**：`__asan_handle_no_return` 空实现

## 与分配器集成

**Slab 分配器（kmalloc/kfree）**：

| 操作 | KASAN 行为 |
|------|-----------|
| kmalloc 返回前 | `kasan_slab_alloc(obj, cache->obj_size)` → unpoison |
| kfree 入口 | `kasan_slab_free(ptr, size)` → poison 为 0xFE（freed）+ double-free 检测 |

**BFC 分配器（大块分配）**：

| 操作 | KASAN 行为 |
|------|-----------|
| kmalloc 大块返回前 | `kasan_bfc_alloc(ptr, npages * PAGE_SIZE)` → unpoison |
| kfree 大块入口 | `kasan_bfc_free(ptr, n_pages * PAGE_SIZE)` → poison 为 0xFE |
| bfc_alloc_page 返回 | 自动 `kasan_bfc_alloc()` → unpoison |
| bfc_free_page | 目前未自动 poison（BFC free 后页面回到空闲池，下次分配时 unpoison） |

## memcpy/memset/memmove KASAN 处理

GCC `-fsanitize=kernel-address` 会拦截 `memcpy`/`memset`/`memmove` 调用，替换为 `__asan_memcpy`/`__asan_memset`/`__asan_memmove`。三层架构：

| 层 | 函数 | KASAN 行为 |
|---|---|---|
| 原始拷贝 | `__memcpy`/`__memset`/`__memmove` (arch/x64/utils.h) | `no_sanitize`，纯字节拷贝，无任何 KASAN 检查 |
| KASAN 包装 | `__asan_memcpy`/`__asan_memset`/`__asan_memmove` (kernel/mem/kasan.c) | `no_sanitize` + kasan_check 两侧 + 调用 `__memcpy` |
| 用户边界 | `copy_from_user/copy_to_user` | `no_sanitize` + 调用 `__memcpy`（不检查用户侧） |

## 初始化顺序

```
kernel_main:
  init_mem(bi)          → BFC 初始化
  acpi_init(bi->rsdp)   → 遍历 identity-mapped ACPI 表（no_sanitize）
  isr_init()            → 中断/异常/调度器/LAPIC 定时器
  kernel_init_finish()  → bump 禁用（no_sanitize）
  kasan_init()          → shadow 映射 + 内核映像 unpoison + 全局红区 + kasan_ready=true
  slab_init()           → 初始化 9 个 kmem_cache_t
  sig_init()            → signal trampoline 页
  proc_init()
  smp_boot_aps()        → per-CPU GDT/TSS/IST 栈页
  pci_init()            → PCIe ECAM/BAR 映射
  ahci_init()           → AHCI DMA buffer
  vfs_init()
  xhci_init()
  ...
```

## 调试经验

### schedule() 缺少 no_sanitize 导致卡死

**现象**：SANITIZE 构建启动正常，idle 进入后第一次调度到 init 进程时系统无响应（不重启，不输出）。

**根因**：`schedule()` 没有 `no_sanitize`，编译器在 `spin_lock_irqsave` 的 `__atomic_exchange_n(&lk->locked, ...)` 前插入 `__asan_store4(&lk->locked)` 检查。如果 shadow 字节为 0xFF（未被 unpoison）或 0xFD（redzone），`kasan_report` → `halt()`，中断禁用状态下永久卡死。

**修复**：给 `schedule()` 和 `idle_entry()` 加 `no_sanitize`。调度器是 KASAN 最容易出问题的路径，因为锁 + 上下文切换 + CR3 切换的组合。

### copy_from_user 返回值语义错误

**现象**：非 SANITIZE 构建中所有使用 `copy_from_user` 的 syscall 返回 EFAULT。

**根因**：`copy_from_user` 返回 `size`（已拷贝字节数），而调用方用 `if (copy_from_user(...)) return -EFAULT` 检查，非零 = 失败。

**修复**：返回 0（未拷贝字节数），与 Linux 语义一致。

### CMake 缓存保留 SANITIZE=1

**现象**：不带 `--sanitizer` 构建后，二进制仍包含 KASAN 符号。

**根因**：`build.sh` 不传 `-DSANITIZE` 时，CMake 缓存保留上次 `SANITIZE=1` 的值。

**修复**：`build.sh` 在不传 `--sanitizer` 时显式传 `-DSANITIZE=0`。

### 用户进程 PML4 缺少 PML4[503]

**现象**：IRET 到用户态后内核访问影子内存触发 page fault → triple fault 重启循环。

**修复**：`process_create_elf` 中加 `new_pml4[503] = pml4[503]`。

## 文件清单

| 文件 | 内容 |
|------|------|
| `kernel/mem/kasan.h` | KASAN API 声明 + 非 SANITIZE 模式内联空实现 + `copy_from_user/copy_to_user` |
| `kernel/mem/kasan.c` | KASAN runtime（`#ifdef SANITIZER` 条件编译，__asan_* + kasan_init + shadow 映射 + kasan_check + report + 分配器 hook） |

## 技术债务

- **栈回溯**：KASAN 报告使用 RBP 链回溯，需要 `-fno-omit-frame-pointer` 才有完整回溯
- **copy_from_user/copy_to_user 缺少 page fault 处理**：当前实现假设用户地址有效，无 invalid address 处理。后续可增加 exception table 机制
- **Slab 红区**：当前 slab 分配器未实现 redzone 扩展（slot_size = obj_size），KASAN 对 slab 内部越界检测能力有限
- **SHM 页 re-poison**：BFC auto-unpoison 后 SHM 页 shadow 为 0x00，内核不读写共享页内容，理想情况应 re-poison 为 0xFF
- **bfc_free_page 未自动 poison**：BFC 释放页后未 poison，依赖下次分配时 unpoison 覆盖
- **KCSAN**：数据竞争检测器尚未实现
