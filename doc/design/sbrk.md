# 用户态堆内存分配 — sys_sbrk

## 设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 堆起始地址 | 0x600000 | 紧接共享页（0x500000-0x506000）之后，留出共享页扩展空间 |
| syscall 语义 | sbrk(increment) | 比 brk(addr) 更简单直接，用户态 malloc 一次调用即可获取可用内存 |
| 缩小堆 | 不支持（increment < 0 返回 -EINVAL） | 缩小需逐页解映射+释放，复杂度高，等进程退出时一起做 |
| 非页对齐 increment | 内核按页向上取整映射 | 传统 sbrk 行为，用户态无需关心页大小 |
| 堆页 PTE 标志 | PTE_PRESENT \| PTE_RW \| PTE_USER \| PTE_NX | W^X 原则，堆是数据区不应执行 |
| 映射函数 | 新增 map_user_pages() 批量映射 | sbrk 可能一次映射多页，未来 mmap 也可复用 |
| 分配失败 | 回滚已映射页，brk 不变 | sbrk 语义要求全有或全无，部分成功导致状态不一致 |
| proc_t 记录 | 新增 uint64_t brk 字段 | idle 进程 brk=0，用户进程 brk 初始=0x600000 |
| syscall 编号 | SYS_SBRK = 7 | 紧接现有 0-6 |
| 错误返回 | -E*（负 errno） | 统一错误码体系，新增 common/errno.h |

## 地址空间布局

```
0x400000 - 0x4FFFFF   代码区（ELF 加载）
0x500000 - 0x506FFF   共享页（KBD_SHM / DISK_REQ / DISK_RESP / FS_REQ / FS_RESP）
0x600000 - ?          堆区（sbrk 按页向上扩展）
...
0x7FFFFFFFD000        用户栈（向下增长）
```

## sys_sbrk 语义

```
uint64_t sys_sbrk(uint64_t increment)
```

- increment = 0：返回当前 brk，不做映射
- increment > 0：扩展堆，返回旧 brk；内部按页取整映射新页
- increment < 0：返回 -EINVAL，不支持缩小
- 分配失败：返回 -ENOMEM，brk 不变，已映射页回滚

返回值：
- 成功：旧 brk 地址
- 失败：负 errno（-EINVAL / -ENOMEM）

## 内核实现

### 1. proc_t 扩展

```c
struct proc_t {
    // ... existing fields ...
    uint64_t brk;          // 堆顶，idle=0，用户进程=0x600000
};
```

初始化：`proc_init()` 中 `procs[i].brk = 0`；`process_create` / `process_create_elf` 中 `proc->brk = 0x600000`。

> **注意**：页表操作函数（`ensure_pd`、`ensure_pt_in_pd`、`map_user_page_direct`、`map_user_pages`、`unmap_user_pages`）已从 `kernel/proc.cc` 移至 `kernel/mem/user_mapping.cc`，声明在 `kernel/mem/alloc.h`。`page_to_phys`/`phys_to_virt` 移至 `kernel/mem/alloc.cc`。

### 2. map_user_pages() — 批量页映射

```c
// 在 vaddr_start 到 vaddr_end（不含）范围内，按 4KB 页逐一分配物理页并映射到 pml4。
// flags: PTE 标志（如 PTE_PRESENT|PTE_RW|PTE_USER|PTE_NX）
// pages_mapped: [out] 实际成功映射的页数（用于失败回滚）
// 返回: true 成功，false 失败（已映射的页需调用者回滚）
bool map_user_pages(uint64_t *pml4, uint64_t vaddr_start, uint64_t vaddr_end,
                    uint64_t flags, int *pages_mapped);
```

内部逻辑：
1. 按 PAGE_SIZE 对齐 vaddr_start（向上）和 vaddr_end（向下），跳过已映射的页
2. 循环：`bfc_alloc.alloc_page(1)` → `map_user_page_direct(pml4, vaddr, phys, flags)`
3. 任一分配失败则返回 false，`*pages_mapped` 记录已映射页数

### 3. unmap_user_pages() — 批量页解映射（回滚用）

```c
// 解映射 vaddr_start 到 vaddr_end（不含）范围内的页，归还物理页给 BFC。
void unmap_user_pages(uint64_t *pml4, uint64_t vaddr_start, uint64_t vaddr_end,
                      int count);
```

内部逻辑：
1. 遍历页表，对每个 PTE：取物理地址 → `bfc_alloc.free_page()` → PTE 清零
2. 只解映射 count 个页（部分回滚场景）

### 4. sys_sbrk 实现

```c
uint64_t sys_sbrk(uint64_t increment, uint64_t, uint64_t, uint64_t, uint64_t) {
    if ((int64_t)increment < 0)
        return -EINVAL;

    uint64_t old_brk = current_proc->brk;

    if (increment == 0)
        return old_brk;

    uint64_t new_brk = old_brk + increment;

    // 需要映射的页范围：[old_brk 向上取整, new_brk 向上取整)
    uint64_t page_start = ALIGN_UP(old_brk, PAGE_SIZE);
    uint64_t page_end   = ALIGN_UP(new_brk, PAGE_SIZE);

    if (page_start < page_end) {
        int pages_mapped = 0;
        uint64_t *pml4 = (uint64_t *)phys_to_virt(current_proc->cr3);
        uint64_t flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

        if (!map_user_pages(pml4, page_start, page_end, flags, &pages_mapped)) {
            // 回滚已映射的页
            if (pages_mapped > 0)
                unmap_user_pages(pml4, page_start, page_start + pages_mapped * PAGE_SIZE, pages_mapped);
            return -ENOMEM;
        }
    }

    current_proc->brk = new_brk;
    return old_brk;
}
```

### 5. 错误码体系 — common/errno.h

```c
#ifndef COMMON_ERRNO_H
#define COMMON_ERRNO_H

#define EOK      0   // 成功
#define EPERM    1   // 操作不允许
#define ENOENT   2   // 实体不存在
#define ENOMEM   3   // 内存不足
#define EINVAL   4   // 无效参数
#define ENOSYS   5   // 未实现 syscall

#endif
```

### 6. 统一现有 syscall 错误返回

| syscall | 当前 | 改为 |
|---------|------|------|
| sys_getc (废弃) | return -1 | return -EPERM |
| sys_irq_bind (越界) | return -1 | return -EINVAL |
| syscall_dispatch (无效号) | return -1 | return -ENOSYS |

## 用户态封装

### common/syscall.h 新增

```c
#define SYS_SBRK 7

static inline int64_t sys_sbrk(int64_t increment) {
    return __syscall1(SYS_SBRK, increment);
}
```

## 实现步骤

```
1. 新增 common/errno.h（错误码枚举）
   → 验证: 编译通过

2. proc_t 新增 brk 字段 + 初始化（proc_init=0, process_create/process_create_elf=0x600000）
   → 验证: 编译通过，现有进程运行无回归

3. kernel/mem/user_mapping.cc: 新增 map_user_pages() / unmap_user_pages() + sys_sbrk 实现（声明在 kernel/mem/alloc.h）
   → 验证: 编译通过

4. 统一现有 syscall 错误返回（-1 → -E*）
   → 验证: 编译通过，shell 交互正常

5. common/syscall.h: 新增 SYS_SBRK + sys_sbrk 封装
   → 验证: 用户态可调用 sys_sbrk

6. shell 中添加 sbrk 测试命令（如 `sbrk 4096` 申请一页，打印返回地址）
   → 验证: sbrk(0) 返回 0x600000，sbrk(4096) 返回旧 brk，连续 sbrk 正确
```

---

## 用户态 malloc/free — 设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 算法 | 显式空闲链表 + 边界标记（boundary tag） | free 时 O(1) 合并相邻空闲块（看前一块 footer），malloc first-fit 遍历 |
| 块布局 | 已分配: header(size\|alloc\|prev_size) + payload；空闲: header + prev + next + footer | 已分配块无 footer 省空间，prev_size 支持向前合并 |
| 对齐 | 16 字节，最小块 32 字节 | x86-64 ABI 标准，最小块恰好放 header(8)+prev(8)+next(8)+footer(8) |
| sbrk 策略 | 固定增量 4KB 起步，连续 sbrk 翻倍至上限 64KB | 减少系统调用次数，小分配不浪费，大分配不过度 |
| 归还内存 | 内核侧支持 sbrk 缩小堆，用户态暂不归还 | 用户态归还需检测堆顶连续空闲块，复杂度留后续 |
| malloc(0) | 返回唯一可 free 指针 | POSIX 规范兼容 |
| free(NULL) | no-op | POSIX 规范 |
| 线程安全 | 不加锁，留 MALLOC_LOCK/MALLOC_UNLOCK 宏占位 | 现无用户态线程，将来替换为实际锁 |
| 接口 | malloc/free/calloc/realloc | 完整 C 标准内存分配接口，gcc 程序依赖 calloc/realloc |
| 头文件 | user/include/stdlib.h | 标准 C 路径，gcc 编译程序 #include <stdlib.h> |
| 实现 | user/lib/malloc.cc | 与头文件分离 |
| 链接方式 | 静态链接 .o | 现阶段最简，动态库加载留后续 |
| 分配失败 | 返回 NULL | C 标准行为 |
| size_t 来源 | #include <stddef.h> | gcc freestanding 模式可用 |
| 调诊输出 | 直接串口打印，后续统一 NDEBUG 宏控制 | 开发期可见，后续统一管控 |
| 堆起始 | sys_sbrk(0) 动态获取 | 与内核解耦，堆起始地址变更时用户态无需改 |

## malloc/free 块结构

```
已分配块:
  +--------+--------+--------+--------+
  | header |        payload          ...|
  +--------+--------+--------+--------+
  header = [size(高位) | alloc(1bit) | prev_size(低位)]

空闲块:
  +--------+--------+--------+--------+--------+
  | header |  prev  |  next  |   free  | footer |
  |        |  ptr   |  ptr   |  space  | =header|
  +--------+--------+--------+--------+--------+

最小块 = header(8) + prev(8) + next(8) + footer(8) = 32 字节
```

### header 编码

```
64 位 header:
  [63:1]  block_size（字节为单位，含 header，16 字节对齐，低 4 位为 0）
  [0]     alloc 标志（1=已分配，0=空闲）
```

- `size & ~1` = 块大小（含 header）
- `size & 1` = alloc 标志
- prev_size 存储前一块大小，用于 free 时向前合并

## malloc 算法

```
1. 请求 size 字节，对齐到 16 字节，加上 header 大小 (8 字节)
2. 遍历空闲链表，first-fit 找到 >= 需要的块
3. 若找到且剩余 >= 最小块(32B)，split 为两块
4. 若未找到，调用 sbrk 扩展堆，将新块加入空闲链表，重新搜索
5. 返回 payload 地址 = 块起始 + header 大小
```

## free 算法

```
1. ptr → 计算 block 起始 = ptr - header_size
2. 标记 alloc=0，写入 footer
3. 检查后一块（通过 block_start + block_size）是否空闲，是则合并（从链表摘除后块）
4. 检查前一块是否空闲（通过 prev_size 找前块 header，检查 alloc 位），是则合并（从链表摘除前块）
5. 将合并后的块插入空闲链表头部
```

## sbrk 增量策略

```
初始增量 = 4KB (一页)
每次触发 sbrk: increment *= 2
上限 = 64KB
```

## calloc / realloc

```
calloc(nmemb, size):
  total = nmemb * size（溢出检查）
  p = malloc(total)
  memset(p, 0, total)
  return p

realloc(ptr, size):
  ptr=NULL → malloc(size)
  size=0   → free(ptr), return NULL
  否则: malloc(new_size) → memcpy(旧数据) → free(旧块) → 返回新块
  （简单实现，不尝试原地扩展）
```

## 实现步骤

```
1. 内核: sys_sbrk 支持缩小堆（increment < 0 时解映射+释放物理页）
   → 验证: sbrk(-4096) 返回旧 brk，brk 减小

2. 新增 user/include/stdlib.h（malloc/free/calloc/realloc 声明 + size_t）
   → 验证: 用户态 #include <stdlib.h> 编译通过

3. 新增 user/lib/malloc.cc（malloc/free/calloc/realloc 实现）
   → 验证: 编译为 .o

4. shell 链接 malloc.o，添加 malloc 测试命令
   → 验证: malloc(100) 返回非 NULL，连续 malloc/free 正确，合并验证
```
