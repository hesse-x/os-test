# 内存管理系统设计

## 概述

统一的内核/用户态内存管理方案，基于简化 SLUB 思路。内核侧新增 slab 分配器（kmalloc/kfree），用户侧重写 malloc/free 为 size-class slab 方案，新增 sys_mmap/sys_munmap 替代 sbrk 作为用户态分配的底层支撑。

### 设计原则

- **简化 SLUB**：per-CPU 活跃 slab 无锁 fast path，per-cache partial list 持锁 slow path
- **统一 size class**：内核/用户态共享 9 个 size class（8/16/32/64/128/256/512/1024/2048）
- **元数据放 Page 描述符**：内核 slab 页元数据复用 `frames[]` 的 `Page` union，整页空间给对象
- **用户态统一 mmap**：小分配和大分配底层均用 sys_mmap，不再用 sys_sbrk

## 已完成

| 功能 | 说明 |
|------|------|
| Bump 分配器 | `arch/x64/paging.cc`，物理地址阶段线性分配，`kernel_init_finish` 后禁用 |
| BFC 分配器 | `kernel/mem/alloc.cc`，Best-Fit Contiguous 页面级分配，frames[] + free_list + bfc_lock |
| 页表映射 | `arch/x64/paging.cc`，4 级页表，2MB huge pages，identity + higher-half 双映射 |
| 用户页映射 | `kernel/mem/user_mapping.cc`，ensure_pd/ensure_pt/map_user_page_direct/map_user_pages/unmap_user_pages |
| **内核 slab 分配器** | `kernel/mem/slab.cc`，per-CPU active slab + per-cache partial list，kmalloc/kfree/kcalloc/krealloc |
| **sys_mmap / sys_munmap** | syscall #8/#9，匿名私有映射，proc_t.mmap_brk + mmap_regions 管理 |
| **用户态 slab malloc** | `user/lib/malloc.cc`，size-class slab + sys_mmap，替代旧显式空闲链表方案 |

## 地址空间布局

```
0x00000000 - 0x3FFFFFFF  identity map（物理 0-1GB，2MB huge pages）
0x400000                 用户代码区（ELF 加载）
0x500000 - 0x509FFF      共享页（KBD/DISK/FS/KMS IPC）
0x600000 - ?             brk 堆区（sbrk，保留但 malloc 不再用）
0x800000 - ?             mmap 区域（匿名私有映射）
...
0x7FFFFFFFD000           用户栈
0xFFFFFFFF80000000       higher-half 内核映射基址（VMA_BASE）
0xFFFFFFFF80100000       内核入口（KERNEL_VMA_BASE）
```

## 内存布局常量

架构相关的页大小常量统一定义在 `arch/x64/memlayout.h`（内核/用户态共享）：

| 宏 | 值 | 说明 |
|----|----|------|
| `PAGE_SHIFT` | 12 | 页偏移位数 |
| `PAGE_SIZE` | 4096 | 页大小 |
| `PAGE_SIZE_2M` | 0x200000 | 2MB 大页 |
| `PHY_TO_PAGE(addr)` | `addr >> 12` | 物理地址→页号 |
| `GET_PAGE_NUM(len)` | 向上取整页数 | 字节数→页数 |

通用工具宏（`ALIGN_UP`/`ALIGN_DOWN`）在 `common/macro.h`。

## Size Class 定义

9 个 class，内核/用户态统一：

| Class | 对象大小 | 每页对象数 | 页内浪费 |
|-------|---------|-----------|---------|
| 0     | 8B      | 512       | 0B      |
| 1     | 16B     | 256       | 0B      |
| 2     | 32B     | 128       | 0B      |
| 3     | 64B     | 64        | 0B      |
| 4     | 128B    | 32        | 0B      |
| 5     | 256B    | 16        | 0B      |
| 6     | 512B    | 8         | 0B      |
| 7     | 1024B   | 4         | 0B      |
| 8     | 2048B   | 2         | 0B      |

> 4KB 页 = 4096 字节，无红区时所有 class 整除 4096，无浪费。

**KASAN 红区预留**：后续启用 KASAN 时，每个 class 增加 redzone_size 字段，每页对象数相应减少。当前不预留。

**分配规则**：`size <= 2048` → slab 分配（找最近 class）；`size > 2048` → BFC 页级分配（内核）/ sys_mmap 大分配（用户态）。

---

## Phase 1: BFC 加锁 ✅

**状态：已实现**

修复 todo.md bug #1：BFC 分配器无锁保护，SMP 并发损坏 free_list。

### 变更

- `kernel/mem/alloc.h`：新增 `extern spinlock_t bfc_lock`，Page struct 改为 union（bfc/slab），新增 `PageStatus::SLAB`
- `kernel/mem/alloc.cc`：定义 `spinlock_t bfc_lock = {0}`
- `BFCAllocator::alloc_page(n)`：`spin_lock_irqsave(&bfc_lock, &flags)` → 分配 → `spin_unlock_irqrestore`
- `BFCAllocator::free_page(page, n)`：同理加锁
- `BFCAllocator::free_page_nums()`：同理加锁（读 free_list 也需保护）
- 所有 Page 字段访问改 `bfc.` 前缀（`cont_page_num`/`prev`/`next`）

### 验证

- 编译通过
- `-smp 2` 启动稳定，无 double-allocation 或页面丢失

---

## Phase 2: 内核 slab 分配器 ✅

**状态：已实现**

### 2.1 Page 描述符扩展

`Page` 结构改为 union 覆盖——BFC 空闲页用 `bfc` 字段，slab 页用 `slab` 字段：

```c
enum class PageStatus : int8_t {
    FREE,       // BFC 空闲
    USED,       // BFC 已分配（大分配、页表页等）
    SLAB,       // slab 页
    RESERVED    // 硬件/BIOS 占用
};

struct Page {
    PageStatus status;
    union {
        struct {
            size_t cont_page_num;   // 连续页数
            Page *prev;             // BFC free list 双向链表
            Page *next;
        } bfc;

        struct {
            kmem_cache_t *cache;    // 所属 cache
            void *freelist;         // 空闲对象链表头（intrusive，对象首 8 字节存 next）
            uint32_t inuse;         // 已分配对象数
            uint32_t obj_count;     // 页内总对象数
            int8_t cpu_id;          // 正在使用此页的 CPU（-1 = 无）
            Page *partial_next;     // partial list 链接
            Page *partial_prev;
        } slab;
    };
};
```

**侵入式空闲链表**：空闲对象首 8 字节存 `next` 指针（指向下一个空闲对象），分配时覆盖为用户数据。slab 页 `freelist` 指向链表头。

**区分 slab 对象 vs BFC 大分配**：`kfree(ptr)` 时查 `frames[PHY_TO_PAGE(PHY_ADDR(ptr))]`：
- `status == SLAB` → slab 释放
- `status == USED` → BFC 释放

### 2.2 kmem_cache_t 数据结构

```c
#define KMALLOC_SHIFT_LOW   3    // 最小 class = 8B
#define KMALLOC_SHIFT_HIGH  11   // 最大 class = 2048B
#define NUM_KMALLOC_CLASSES 9    // 8/16/32/64/128/256/512/1024/2048（定义在 alloc.h）

struct kmem_cache_t {
    size_t obj_size;              // 对象大小（不含红区）
    size_t redzone_size;          // 红区大小（当前 = 0）
    spinlock_t lock;              // per-cache 锁（保护 partial list）
    Page *partial;                // 有空闲对象的 slab 链表
};

// 全局数组
kmem_cache_t kmalloc_caches[NUM_KMALLOC_CLASSES];
```

### 2.3 Per-CPU 活跃 slab

每个 CPU 每个 class 维护一个活跃 slab 指针：

```c
// cpu_local_t 新增（arch/x64/smp.h）
Page *active_slab[NUM_KMALLOC_CLASSES];  // 当前 CPU 正在分配的 slab 页
```

**分配 fast path**（无锁）：
1. `active_slab[c]` 非空且 freelist 非空 → 从 `page->slab.freelist` pop 对象，`inuse++`

**分配 slow path**（持 per-cache 锁）：
1. `active_slab[c]` 的 freelist 为空或为 NULL → `spin_lock_irqsave(&cache->lock)`
2. partial list 非空 → 取头页作为 active_slab，设 `page->slab.cpu_id = current_cpu`
3. partial list 也空 → `bfc_alloc.alloc_page(1)` → 初始化 slab 页 → 作为 active_slab
4. `spin_unlock_irqrestore`

### 2.4 释放路径

**kfree(ptr)**：
1. `ptr == NULL` → 直接返回
2. `Page *page = &frames[PHY_TO_PAGE(PHY_ADDR(ptr))]`
3. `page->status == USED` → BFC 大分配释放（`bfc_alloc.free_page(page, page->bfc.cont_page_num)`）
4. `page->status == SLAB` → slab 释放：
   - 检查 `page->slab.cpu_id == current_cpu_id`
   - **是（同 CPU）**：push 到 `page->slab.freelist`，`inuse--`，**无锁**
     - 若从 full 变为 partial（`inuse == obj_count - 1`），移入 partial list
   - **否（跨 CPU）**：`spin_lock_irqsave(&page->slab.cache->lock)`，push freelist + `inuse--`，可能移入 partial list，`spin_unlock_irqrestore`

### 2.5 Slab 页初始化

从 BFC 获取 1 页后初始化：`page->status = SLAB`，设置 cache/inuse/obj_count/cpu_id，建立侵入式空闲链表。

### 2.6 API

```c
// kernel/mem/slab.h
void slab_init(void);
void *kmalloc(size_t size);
void kfree(const void *ptr);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t new_size);
```

### 2.7 文件放置

| 文件 | 内容 |
|------|------|
| `kernel/mem/slab.h` | kmem_cache_t, kmalloc/kfree/kcalloc/krealloc 声明 |
| `kernel/mem/slab.cc` | slab 分配器实现 |
| `kernel/mem/alloc.h` | Page union, bfc_lock, NUM_KMALLOC_CLASSES |
| `kernel/mem/alloc.cc` | bfc_lock 定义, alloc_page/free_page 加锁 |
| `kernel/mem/CMakeLists.txt` | 加入 slab.cc |
| `arch/x64/smp.h` | cpu_local_t 新增 active_slab[NUM_KMALLOC_CLASSES] |
| `arch/x64/smp.cc` | smp_init_cpu 初始化 active_slab |

### 2.8 初始化顺序

```
kernel_main:
  init_mem(bi)          → BFC 初始化 + bfc_lock
  isr_init()            → 中断/异常/调度器
  kernel_init_finish()  → bump 禁用
  slab_init()           → 初始化 9 个 kmem_cache_t（设 obj_size, 初始化 lock, partial=NULL）
  proc_init()
  smp_boot_aps()
  ...
```

`slab_init()` 不预分配 slab 页——首次 kmalloc 时按需从 BFC 获取。AP 的 `cpu_local_t` 在 `smp_init_cpu` 中显式初始化 `active_slab` 全为 NULL，首次分配走 slow path。

### 2.9 验证

- 编译通过
- QEMU `-smp 2` 启动正常
- kmalloc 被 sys_mmap 实际消费（mmap_region 节点分配）

---

## Phase 3: sys_mmap / sys_munmap ✅

**状态：已实现**

### 3.1 Syscall 接口

```c
// SYS_MMAP = 11
// 参数: size_t size（字节，内核按页对齐）
// 返回: 映射的虚拟地址，失败返回负 errno
void *sys_mmap(size_t size);

// SYS_MUNMAP = 12
// 参数: void *addr, size_t size
// 返回: 0 成功，负 errno 失败
int sys_munmap(void *addr, size_t size);
```

- 纯匿名私有映射，无文件映射，无共享映射
- `sys_mmap` 内核自动选择虚拟地址（0x800000 起，mmap_brk 递增）
- `size` 按 `ALIGN_UP(size, PAGE_SIZE)` 对齐

### 3.2 进程 mmap 区域

**proc_t 扩展**：

```c
struct mmap_region {
    uint64_t vaddr;          // 起始虚拟地址
    uint64_t size;           // 字节数（页对齐）
    mmap_region *next;       // 链表 next
};

struct proc_t {
    // ... existing fields ...
    uint64_t mmap_brk;       // mmap 区域高水位（初始 0x800000）
    mmap_region *mmap_regions; // mmap 区域链表头
};
```

mmap_region 节点用 `kmalloc` 分配——这是 kmalloc 的第一个实际消费者。

### 3.3 sys_mmap 实现

1. `size = ALIGN_UP(size, PAGE_SIZE)`
2. `vaddr = current_proc->mmap_brk`
3. 分配 `size / PAGE_SIZE` 个物理页（BFC `alloc_page(1)` 逐页分配），用 `kmalloc` 临时数组记录物理地址
4. 逐页映射到当前进程 PML4：`map_user_page_direct(pml4, vaddr, phys, PTE_PRESENT|PTE_RW|PTE_USER|PTE_NX)`
5. 分配 `mmap_region` 节点（`kmalloc(sizeof(mmap_region))`），记录 vaddr/size，加入链表
6. `current_proc->mmap_brk += size`
7. 返回 vaddr

**失败处理**：部分映射失败时回滚已映射页。

### 3.4 sys_munmap 实现

1. 遍历 `mmap_regions` 找到匹配区域（vaddr 匹配）
2. 逐页调用 `unmap_user_pages` 解映射 + 释放物理页
3. 从链表删除 region 节点，`kfree(region)`
4. 返回 0

**proc_reap 扩展**：遍历 `mmap_regions`，释放 region 元数据节点（物理页已在 PML4 遍历中释放）。

### 3.5 syscall 封装

```c
// common/syscall.h
#define SYS_MMAP   11
#define SYS_MUNMAP 12

static inline void *sys_mmap(size_t size) {
    return (void *)__syscall1(SYS_MMAP, size);
}

static inline int sys_munmap(void *addr, size_t size) {
    return (int)__syscall2(SYS_MUNMAP, (uint64_t)addr, size);
}
```

### 3.6 验证

- 编译通过
- `-smp 2` QEMU 启动正常，shell 和 hello.elf 运行正常

---

## Phase 4: 用户态 malloc/free 重写 ✅

**状态：已实现**

### 4.1 设计概览

用 size-class slab 方案替代旧的显式空闲链表 + 边界标记算法。小分配（≤2048）走 slab free list，大分配（>2048）走 sys_mmap。底层统一用 sys_mmap，不再调用 sys_sbrk。

### 4.2 用户态 slab 页

每个 slab 页页首放 `user_slab_header`：

```c
#define USER_SLAB_MAGIC  0x5B  // 标识合法 slab 页
#define BIG_ALLOC_MAGIC  0xA1  // 标识大分配页

struct user_slab_header {
    uint8_t magic;             // = USER_SLAB_MAGIC
    uint8_t class_idx;         // 0-8，size class 索引
    uint16_t inuse;            // 已分配对象数
    uint16_t obj_count;        // 页内总对象数
    void *freelist;            // 空闲对象链表头（侵入式）
    user_slab_header *next;    // partial list
};
```

对象从 header 之后按 obj_size 对齐开始排列。

### 4.3 大分配 header

大分配页首放 `big_alloc_header`：

```c
struct big_alloc_header {
    uint32_t magic;            // = BIG_ALLOC_MAGIC
    uint32_t npages;           // 占用页数
};
```

8 字节，用户拿到的指针 = 页首 + sizeof(big_alloc_header) 偏移。

### 4.4 全局状态

```c
static void *class_freelist[NUM_KMALLOC_CLASSES];  // per-class 空闲对象链表头
static user_slab_header *class_partial[NUM_KMALLOC_CLASSES]; // per-class partial slab 链表
```

单线程，无需锁。

### 4.5 malloc(size)

1. `size == 0` → size = 1
2. `size > 2048` → 大分配：
   - `npages = (size + sizeof(big_alloc_header) + PAGE_SIZE - 1) / PAGE_SIZE`
   - `sys_mmap(npages * PAGE_SIZE)` → 页首写 `big_alloc_header`（magic + npages）
   - 返回 `addr + sizeof(big_alloc_header)`
3. `class = size_to_class(size)` → 小分配：
   - `class_freelist[c]` 非空 → pop，返回
   - 空 → 从 `class_partial[c]` 取 partial slab，搬其 freelist 到 `class_freelist`
   - partial 也空 → `sys_mmap(PAGE_SIZE)` → 初始化 `user_slab_header` + 侵入式空闲链表 → 对象入 `class_freelist` → pop 返回

### 4.6 free(ptr)

1. `ptr == NULL` → 直接返回
2. `page_start = ALIGN_DOWN(ptr, PAGE_SIZE)`
3. `header = (user_slab_header *)page_start`
4. 检查 `header->magic`：
   - `== USER_SLAB_MAGIC` → slab 释放：push ptr 到 `class_freelist[class_idx]`，`header->inuse--`
   - `== BIG_ALLOC_MAGIC` → 大分配释放：`sys_munmap(page_start, npages * PAGE_SIZE)`
   - 其他 → 忽略（非法指针）

### 4.7 calloc / realloc

标准实现：calloc 溢出检查 + malloc + memset(0)；realloc 用 get_alloc_size 获取旧大小 + malloc + memcpy + free。

### 4.8 文件变更

| 文件 | 变更 |
|------|------|
| `user/lib/malloc.cc` | 完全重写为 slab 方案 |
| `user/include/stdlib.h` | 不变（malloc/free/calloc/realloc 声明） |
| `common/syscall.h` | 新增 SYS_MMAP/SYS_MUNMAP 封装 |
| `arch/x64/memlayout.h` | 新建，PAGE_SIZE 等常量（替代旧 paging.h 中的重复定义） |

### 4.9 验证

- 编译通过（libc.a + 所有用户程序）
- QEMU `-smp 2` 启动正常，shell 运行正常
- `user/malloctest.c` 测试程序覆盖：小分配/释放重用/calloc/大分配/realloc

---

## Phase 5: KASAN 红区集成

**状态：未实现** | **设计文档**: [sanitize.md](sanitize.md)

在 Phase 2/4 基础上为每个 size class 增加 redzone_size，KASAN 启用时生效：

| 操作 | KASAN 行为 |
|------|-----------|
| kmalloc 返回对象 | unpoison 对象区域（shadow 标 0），poison 红区（shadow 标 0xF9） |
| kfree 释放对象 | poison 整个 slot（shadow 标 0xFD） |
| 大分配（BFC） | unpoison 分配页，尾部 poison |
| 大释放（BFC） | poison 整个页块 |

依赖：KASAN shadow 映射初始化（`kasan_init`），编译器 `-fsanitize=kernel-address` 插桩。

---

## Phase 6: 共享 mmap

**状态：未实现** | **设计文档**: [sanitize.md](sanitize.md) Wayland 部分提及

`sys_mmap` 加 flags 参数支持 `MAP_SHARED`，多进程映射同一物理页，refcount 管理。依赖 Wayland IPC 需求，约 100-150 行增量。

---

## Phase 7: 空 slab 页回收

**状态：未实现**

当前 partial list 中的空 slab 页（inuse == 0）不归还 BFC/不 munmap。内存只增不减。

**内核侧**：
- 释放对象时若 `inuse == 0` 且非 active_slab → 从 partial list 移除 → `bfc_free_page`
- 需处理跨 CPU 情况（空页可能被其他 CPU 的 partial list 持有）

**用户态侧**：
- 释放对象时若 `inuse == 0` → 从 partial list 移除 → `sys_munmap`

---

## Bug 修复记录

### PTE 物理地址掩码

`& ~0xFFF` 不清除 x86-64 NX 位（bit 63），在有 NX 位的 PTE 上会得到越界物理地址。统一改用 `& 0x000FFFFFFFFFF000ULL`。

**受影响位置**：
- `kernel/mem/user_mapping.cc`：`unmap_user_pages`（leaf PTE）、`ensure_pd`（PDPT entry）、`ensure_pt_in_pd`（PD/PT entry）
- `kernel/proc.cc`：`proc_reap` 中 PDPT/PD/PT entry 的物理地址提取

**症状**：`free_page` 收到非法 page 指针 → #GP。栈回溯定位到 `sys_munmap → unmap_user_pages → free_page`。

---

## 技术债务

| 项目 | 说明 | 优先级 |
|------|------|--------|
| 空 slab 页回收 | partial list 空 slab 页不归还，内存只增不减 | 中 |
| per-CPU slab cache | 当前 per-CPU 活跃 slab + per-cache 单锁，2-4 CPU 够用。CPU 数增加后可优化为 per-CPU 多 slab | 低 |
| sbrk 缩小堆 | sys_sbrk 支持负值 increment，归还堆顶空闲页。用户态 malloc 不再用 sbrk，但保留接口 | 低 |
| mmap 虚拟地址复用 | munmap 后的虚拟地址洞不再分配，mmap_brk 只增不减。长期应加地址复用 | 低 |
| 大分配分配器 | >2048 的分配直接走 BFC 逐页分配，无连续物理页保证。大分配应改为 BFC 连续分配 + 整块映射 | 中 |
| 进程创建失败内存泄漏 | `process_create_elf` 后续 `alloc_page` 失败时前面已分配页面未释放（todo.md bug #9）| 高 |

---

## 构建调试支持

`./build.sh` 支持 `-d` 选项切换 Debug/Release 模式：

- **Release**（默认）：`cmake -DCMAKE_BUILD_TYPE=Release`
- **Debug**（`./build.sh -d`）：`cmake -DCMAKE_BUILD_TYPE=Debug`，加 `-g -fno-omit-frame-pointer`

Debug 模式下内核异常处理会遍历 RBP 链打印栈回溯地址，配合 `addr2line -e build/myos.elf <addr>` 可定位到源码行。

---

## 与现有文档的关系

| 文档 | 处理 |
|------|------|
| `slab.md` | 本文档替代（原设计有误：页首 header 浪费空间、8 个 class 无 8B、per-cache 单锁非 per-CPU） |
| `sbrk.md` | sys_sbrk 实现保留，用户态 malloc 不再调用 sbrk。sbrk 接口和 brk 机制作为历史参考保留 |
| `page_plan.md` | 历史文档（x86-32），不合并 |
| `page_plan1.md` | 历史文档（x86-32），不合并 |
| `sanitize.md` | KASAN/KCSAN 设计保留，Phase 5 实现时参考 |
