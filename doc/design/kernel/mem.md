# 内存管理

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 内核小分配 | per-CPU active slab + per-cache partial list（简化 SLUB） | fast path 无锁，slow path 持 per-cache 锁，2-4 CPU 够用 |
| 2 | 用户态小分配 | size-class slab + sys_mmap | 替代旧 sbrk + 显式空闲链表方案 |
| 3 | size class 统一 | 9 个 class（8/16/32/64/128/256/512/1024/2048），内核/用户态共享 | 4KB 页整除所有 class，无页内浪费 |
| 4 | slab 元数据 | Page 描述符 union（bfc/slab 覆盖） | 复用 frames[]，整页空间给对象，不浪费 slab 页首存 header |
| 5 | 用户态底层 | sys_mmap 统一 | 小分配和大分配底层均用 mmap，不再用 sys_sbrk |
| 6 | 侵入式空闲链表 | 空闲对象首 8 字节存 next 指针 | 分配时覆盖为用户数据，无需额外 freelist 数组 |
| 7 | kfree 区分来源 | frames[PHY_TO_PAGE] 判断 status==SLAB 或 USED | slab 对象走 slab 释放，BFC 大分配走 BFC 释放 |

### 地址空间布局

```
0x00000000 - 0x3FFFFFFF  identity map（物理 0-1GB，2MB huge pages）
0x400000                 用户代码区（ELF 加载）
0x500000 - 0x509FFF      共享页（信号 trampoline + vdso）
0x800000 - ?             mmap 区域（匿名私有映射 / SHM 映射）
0x7FFFFFFFD000           用户栈
0xFFFFFFFF80000000       higher-half 内核映射基址（VMA_BASE）
0xFFFFFFFF80100000       内核入口（KERNEL_VMA_BASE）
```

地址映射和分页设计详见 [page.md](page.md)。

### 内存布局常量

arch/x64/memlayout.h（内核/用户态共享）：

| 宏 | 值 | 说明 |
|----|-----|------|
| PAGE_SHIFT | 12 | 页偏移位数 |
| PAGE_SIZE | 4096 | 页大小 |
| PAGE_SIZE_2M | 0x200000 | 2MB 大页 |
| PHY_TO_PAGE(addr) | addr >> 12 | 物理地址→页号 |

### Size Class

9 个 class，内核/用户态统一：

| Class | 对象大小 | 每页对象数 |
|-------|---------|-----------|
| 0 | 8B | 512 |
| 1 | 16B | 256 |
| 2 | 32B | 128 |
| 3 | 64B | 64 |
| 4 | 128B | 32 |
| 5 | 256B | 16 |
| 6 | 512B | 8 |
| 7 | 1024B | 4 |
| 8 | 2048B | 2 |

分配规则：size ≤ 2048 → slab 分配（找最近 class）；size > 2048 → BFC 页级分配（内核）/ sys_mmap 大分配（用户态）。

KASAN 红区预留：后续启用时每个 class 增加 redzone_size，每页对象数相应减少。当前不预留。

### 分配器层级

| 分配器 | 位置 | 说明 |
|--------|------|------|
| Bump | arch/x64/paging.c | 极简线性分配，kernel_end 起始，kernel_init_finish 后禁用 |
| BFC | kernel/xcore/mem/alloc.c : BFCAllocator | Best-Fit Contiguous 页面级分配，frames[] + free_list + bfc_lock |
| 内核 slab | kernel/xcore/mem/slab.c | per-CPU active slab + per-cache partial list |
| 用户态 slab | user/lib/malloc.cc | size-class slab + sys_mmap |

### Page 描述符

kernel/mem/alloc.h : Page（union 覆盖）

字段：
- status : PageStatus — FREE / USED / SLAB / RESERVED
- p_refcount : refcount_t — 物理页引用计数（0=free，1=exclusive，>1=COW 共享）
- bfc（BFC 空闲页用）：
  - cont_page_num : size_t — 连续页数
  - prev / next : Page* — free list 双向链表
- slab（slab 页用）：
  - cache : kmem_cache_t* — 所属 cache
  - freelist : void* — 空闲对象链表头
  - inuse : uint32_t — 已分配对象数
  - obj_count : uint32_t — 页内总对象数
  - cpu_id : int8_t — 使用此页的 CPU（-1 = 无）
  - partial_next / partial_prev : Page* — partial list 链接

### 内核 slab 分配器

kernel/mem/slab.cc : kmem_cache_t

字段：
- obj_size : size_t — 对象大小（不含红区）
- redzone_size : size_t — 红区大小（当前 = 0）
- lock : spinlock_t — per-cache 锁（保护 partial list）
- partial : Page* — 有空闲对象的 slab 链表

全局数组：kmalloc_caches[NUM_KMALLOC_CLASSES]（9 个 cache）。

per-CPU 活跃 slab：cpu_local_t.active_slab[NUM_KMALLOC_CLASSES]。

分配路径：
- fast path（无锁）：active_slab[c] 非空且 freelist 非空 → pop 对象，inuse++
- slow path（持 per-cache 锁）：freelist 空 → spin_lock → 从 partial list 取或 BFC alloc_page(1) 新建 → spin_unlock

释放路径：
- 同 CPU（cpu_id == current）：push freelist，inuse--，无锁；从 full 变 partial 时移入 partial list
- 跨 CPU：持 per-cache lock，push freelist + inuse-- + 可能移入 partial list

API：kernel/mem/slab.h — kmalloc / kfree / kcalloc / krealloc

### sys_mmap / sys_munmap

sys_mmap（syscall #8）：纯匿名私有映射，内核自动选择虚拟地址（0x800000 起，mmap_brk 递增），size 按页对齐。扩展支持 MAP_PHYSICAL（详见 [boot.md](boot.md)）。MAP_SHARED+fd≥0 映射 SHM fd（memfd）；设备 fd 从 inode->shm 取物理页映射（用户态驱动 dev_create 关联的 SHM）。mmap_region 新增 prot 字段（PROT_READ | PROT_WRITE | PROT_EXEC），sys_mmap 和 elf_load 时设置，fork COW 用 prot 判断页分类（详见 [page.md](page.md) COW 章节）。

sys_munmap（syscall #9）：遍历 mmap_regions 找匹配区域 → 逐页 unmap + 释放物理页 → 从链表删除 region → kfree(region)。

proc_t 扩展字段：mmap_brk（uint64_t，mmap 区域高水位）+ mmap_regions（mmap_region*，mmap 区域链表头）。

mmap_region 字段：vaddr / size / phys / shm_obj / prot（PROT_READ|PROT_WRITE|PROT_EXEC）/ next。

proc_reap 时遍历 mmap_regions 释放 region 元数据节点。

### 用户态 malloc/free

user/lib/malloc.cc — size-class slab 方案。

user_slab_header（页首）字段：magic / class_idx / inuse / obj_count / freelist / next。

big_alloc_header（大分配页首）字段：magic / npages。

全局状态：class_freelist[9] + class_partial[9]，单线程无锁。

malloc 路径：size > 2048 → sys_mmap 大分配（页首放 big_alloc_header）；size ≤ 2048 → 从 class_freelist pop 或从 partial slab 取或 sys_mmap 新建 slab 页。

free 路径：ALIGN_DOWN(ptr, PAGE_SIZE) 读页首 magic → USER_SLAB_MAGIC 走 slab 释放 → BIG_ALLOC_MAGIC 起大分配释放（sys_munmap）。

### 初始化顺序

kernel_main：init_mem → isr_init → kernel_init_finish（bump 禁用）→ slab_init（初始化 9 个 kmem_cache_t）→ proc_init → smp_boot_aps

slab_init 不预分配 slab 页，首次 kmalloc 时按需从 BFC 获取。AP 的 active_slab 在 smp_init_cpu 中初始化为 NULL。

### 关键源码位置

- BFC 分配器：kernel/xcore/mem/alloc.c : BFCAllocator
- Page 描述符：kernel/xcore/mem/alloc.h : Page / PageStatus / p_refcount
- Slab 分配器：kernel/xcore/mem/slab.c / slab.h
- 用户页映射：kernel/xcore/mem/user_mapping.c : map_user_page_direct / unmap_user_pages
- copy_to_user / copy_from_user：kernel/xcore/mem/copy_user.c
- 用户态 malloc：user/lib/malloc.cc
- sys_mmap / sys_munmap：kernel/bsd/syscall.c

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| 空 slab 页回收 | partial list 空 slab 页（inuse==0）不归还 BFC/不 munmap，内存只增不减 | 中 |
| 共享 mmap（MAP_SHARED） | SHM fd 和设备 inode->shm 的 MAP_SHARED 映射已实现；FD_REGULAR 文件 mmap 共享（page cache）待实现 | 中 |
| KASAN 红区集成 | 每个 size class 增加 redzone_size，kmalloc 时 unpoison 对象 + poison 红区，kfree 时 poison 整个 slot。详见 [sanitize.md](sanitize.md) | 中 |
| 大分配连续物理页 | >2048 的分配逐页从 BFC 分配，无连续物理页保证，应改为 BFC 连续分配 + 整块映射 | 中 |
| 进程创建失败内存泄漏 | process_create_elf 后续 alloc_page 失败时前面已分配页面未释放 | 高 |
| mmap 虚拟地址复用 | munmap 后的虚拟地址洞不再分配，mmap_brk 只增不减 | 低 |
| per-CPU slab cache 优化 | 当前 per-CPU 单 active slab + per-cache 单锁，CPU 数增加后可改为 per-CPU 多 slab | 低 |
| malloc free 不验证指针合法性 | user/lib/malloc.cc 任意指针损坏空闲链表，应加 basic sanity check | 中 |
| 页面换出 (swap) | BFC 空闲页不归还物理内存（无 balloon 驱动），QEMU 下内存只增不减 | 低 |
