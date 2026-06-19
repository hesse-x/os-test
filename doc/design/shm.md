# SHM 重构：fd 化设计

> **计划中**。将共享内存从硬编码 vaddr + `shm_regions[]` 数组模型重构为 fd 模型，对齐 Linux `memfd_create` + `mmap` + `SCM_RIGHTS` 语义。

## 动机

### 当前问题

| 问题 | 描述 |
|------|------|
| 硬编码地址 | `SHM_VADDR_BASE = 0x71000000`，所有 SHM vaddr 从此线性扫描分配 |
| shm_regions 数组 | `proc_t::shm_regions[MAX_SHM_PER_PROC=4]`，限制最多 4 块 SHM/进程 |
| vaddr 分配代码重复 | 三段完全相同"找 slot → 找 vaddr → map 页"的逻辑在 `sys_shm_create`/`sys_shm_attach(mode=0/1)` 中 |
| attach 只取 first-fit | `sys_shm_attach(pid, 0)` 只 attach 目标进程的第一个 active SHM region |
| 清理 O(64×4) 扫描 | `proc_reap` 遍历全部 64 进程 × 4 slot 来算引用计数 |
| sys_shm_create 返回 vaddr | 不符合 fd 模式，无法通过 SCM_RIGHTS 传递 |
| 无独立 fd 句柄 | `sys_mmap` 没有第五个 fd 参数，无法 mmap shm fd |
| 无引用计数对接 | fd close 和 munmap 没有协同生命周期管理 |

### 目标

- `sys_shm_create(size)` → 返回 fd（类似 `memfd_create`）
- `sys_mmap(fd, ...)` → 映射到地址空间得到 vaddr
- fd 可 close（不影响已有映射），可 SCM_RIGHTS 传递（等 socket 就绪）
- 引用计数：fd 和 vma 各自保活，两者归零 → 释放物理页
- 删除 `shm_regions[]`、`SHM_VADDR_BASE`、`MAX_SHM_PER_PROC`、`kernel_shm_table`
- 统一 MAP_PHYSICAL 和 SHM 的 mmap 路径

## 设计

### 新数据结构

```c
// kernel/proc.h

// SHM 底层对象（kmalloc 分配，引用计数管理）
struct shm {
    uint64_t phys;       // 物理页起始地址
    size_t   npages;     // 连续页数
    int      ref_count;  // 引用计数：fd + 1，mmap vma + 1
    int      flags;      // SHM_KERNEL 等
};

// fd_table 扩展
#define FD_SHM   2       // 新类型，填充 FD_NONE(0)/FD_PIPE(1)/FD_DEV(3) 之间的空隙

struct file {
    int type;            // FD_NONE / FD_PIPE / FD_SHM / FD_DEV
    int flags;
    struct pipe *pipe;
    struct shm  *shm;    // if type == FD_SHM
    pid_t target_pid;
};

// mmap_region 扩展
struct mmap_region {
    uint64_t vaddr;
    uint64_t size;
    uint64_t phys;           // MAP_PHYSICAL 专用（旧方式）
    struct shm *shm_obj;     // mmap(SHM fd) 时非 NULL，其 phys/npages 覆盖 phys/size
    mmap_region *next;
};
```

### 删除的数据结构

| 字段 | 所在文件 |
|------|---------|
| `shm_region shm_regions[MAX_SHM_PER_PROC]` | `proc_t` in `kernel/proc.h` |
| `#define SHM_VADDR_BASE` | `kernel/proc.h` |
| `#define MAX_SHM_PER_PROC` | `kernel/proc.h` |
| `kernel_shm_table[]` / `register_kernel_shm()` | `kernel/trap.cc` |
| `struct kernel_shm_region` | `kernel/trap.cc` |

### syscall 接口变化

#### `sys_mmap` — 改 6 参对齐 Linux

```c
// 旧: void *sys_mmap(void *addr, size_t size, int prot, int flags, uint64_t offset);
// 新: void *sys_mmap(void *addr, size_t size, int prot, int flags, int fd, uint64_t offset);
```

- `flags` 含 `MAP_SHARED` 且 `fd >= 0`：映射 SHM fd → `mmap_region.shm_obj = shm`, `shm->ref_count++`
- `flags` 含 `MAP_ANONYMOUS`：匿名映射（旧行为，不变）
- `flags` 含 `MAP_PHYSICAL`：物理地址映射（旧行为，`offset` 做物理地址）

#### `sys_shm_create` — 改返回 fd

```c
// 旧: void *sys_shm_create(size_t size);     // 返回 vaddr
// 新: int sys_shm_create(size_t size);        // 返回 fd（0 = 失败）
```

内核操作：
1. `bfc_alloc.alloc_page(npages)` 分配物理页
2. `kmalloc(sizeof(struct shm))` 分配 shm 对象
3. 填 `shm->phys/npages/ref_count=1`
4. `fd_table[fd]` type=FD_SHM, shm=上一步的指针
5. 返回 fd

#### `sys_shm_attach` — 改返回 fd（过渡用）

```c
// 旧: void *sys_shm_attach(int32_t id, int mode);  // 返回 vaddr
// 新: int sys_shm_attach(int32_t id, int mode);     // 返回 fd（0 = 失败）
```

- 不再返回 vaddr、不再自行映射页、不再操作 `shm_regions[]`
- 不再有 mode=0 和 mode=1 两个代码路径

语义变为：
- 查找目标 SHM 对象（根据 mode 决定：进程 SHM 还是 kernel SHM）
- `shm->ref_count++`
- 分配 fd slot，`fd_table[fd].type=FD_SHM`, `.shm=shm`
- 返回 fd

调用方随后必须 `sys_mmap(fd, ...)` 获取 vaddr。

等 SCM_RIGHTS 就绪后此 syscall 可删除。

#### 6 参数 syscall 支撑

```c
// arch/x64/utils.h 新增
static inline int64_t __syscall6(int64_t num, int64_t a1, int64_t a2, int64_t a3,
                                 int64_t a4, int64_t a5, int64_t a6) {
    int64_t ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(a4), "r8"(a5), "r9"(a6)
        : "rcx", "r11", "memory");
    return ret;
}
```

```c
// kernel/trap.h
typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// kernel/trap.cc
void syscall_dispatch(trapframe_t *tf) {
    if (tf->rax < NR_SYSCALL) {
        tf->rax = syscall_table[tf->rax](
            tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);  // +r9
    }
}
```

所有 36 个 syscall 实现函数尾部加 `, uint64_t arg6` 参数（编译器优化掉 unused）。

### 引用计数生命周期

```
                   ┌─────────────┐
                   │  struct shm │
                   │  ref_count  │
                   └──────┬──────┘
                          │
            ┌─────────────┼─────────────┐
            │ +1          │ +1          │ +1 (未来)
            ▼             ▼             ▼
      fd_table[fd]   mmap_region     SCM_RIGHTS 传 fd
      (FD_SHM)       (.shm_obj)      (新进程 fd)
            │             │
            │ close(fd)   │ munmap / proc_reap
            │ ref_count-- │ ref_count--
            └─────────────┘
                          │
                   ref_count == 0
                          │
                          ▼
                  kfree(shm) + bfc_alloc.free_page(phys, npages)
```

**规则：**

| 操作 | ref_count 变化 | 说明 |
|------|---------------|------|
| `sys_shm_create(size)` | =1（fd 持有） | fd 本身计 1 引用 |
| `sys_mmap(fd, ...)` | +1 | vma 映射计 1 |
| `close(fd)` | -1 | fd 释放 |
| `munmap(vaddr)` | -1 | vma 释放 |
| SCM_RIGHTS 发送 fd | +1 | 接收端 fd 计 1 |
| `dup2(old_fd, new_fd)` | +1 | 复制 fd 计 1 |
| 进程 exit：close all fds | -1 per FD_SHM | fd_table 遍历 |
| 进程 exit：unmap all vmas | -1 per shm_obj | mmap_regions 遍历 |

**kernel SHM（USB HID）特殊处理：**

```c
#define SHM_KERNEL 1  // 页由内核管理，ref_count==0 时不释放物理页

// 内核初始化时
struct shm *kshm = kmalloc(sizeof(struct shm));
kshm->phys = usb_hid_shm_phys;
kshm->npages = 1;
kshm->ref_count = 0;   // kernel 本身不计数
kshm->flags = SHM_KERNEL;
register_kernel_shm(USB_HID_SHM_ID, kshm);

// kbd_driver 请求时
int fd = sys_shm_attach(USB_HID_SHM_ID, 1);  // → kshm->ref_count++ (变 1)
void *vaddr = sys_mmap(fd, ...);             // → kshm->ref_count++ (变 2)
// close(fd) → ref_count=1, munmap → ref_count=0 → SHM_KERNEL 标记 → 不释放页
```

`register_kernel_shm` 改为接受 `struct shm *` 而非 `(phys, npages)`，同时引入头文件声明（不再裸调用）。

### proc_reap 清理路径变更

```c
// 旧路径（将被删除）:
proc_reap:
    // step 1: PML4 遍历，跳过 shm_regions 的物理页
    // step 5: 扫描全体 procs[].shm_regions 算引用，归零时释放

// 新路径:
proc_reap:
    // 遍历 fd_table: type==FD_SHM → shm_put(shm) { ref_count--; 归零时 kfree+释放页 }
    // 遍历 mmap_regions: shm_obj != NULL → shm_put(shm_obj)
    //
    // 注意：PML4 遍历时通过 mmap_region.shm_obj 识别
    // 非 SHM 页（匿名 mmap、MAP_PHYSICAL）正常释放 PTE 和物理页
    // SHM 页（mmap_region.shm_obj != NULL）只 unmap PTE，不释放物理页（由 shm_put 在 ref_count==0 时释放）
```

### 驱动适配

所有 `shm_create()` / `shm_attach()` 调用点改为：
```
// 旧
vaddr = shm_create(4096);
vaddr = shm_attach(PID, 0);
vaddr = shm_attach_kernel(USB_HID_SHM_ID, &addr);

// 新
int fd = sys_shm_create(4096);
void *vaddr = sys_mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
sys_close(fd);   // 可选：关闭 fd，mmap 映射仍有效

int fd = sys_shm_attach(target_pid, 0);    // 过渡期
void *vaddr = sys_mmap(fd, ...);

int fd = sys_shm_attach(USB_HID_SHM_ID, 1);
void *vaddr = sys_mmap(fd, ...);
```

各驱动改动量：
| 驱动 | SHM 个数 | 改动点 |
|------|---------|--------|
| `kbd_driver` | 2（自创 kbd SHM + attach USB HID）| 2 处 shm_create + 1 处 attach_kernel |
| `kms_driver` | 1（display SHM via display_backend_init）| 1 处 shm_create |
| `terminal` | 2（mmap display SHM + mmap kbd SHM）| 2 处 mmap(MAP_SHARED) |
| `fs_driver` | 1（自创 4 页 SHM）| 1 处 shm_create |

### 用户态 libc 适配

```c
// user/lib/sys_shm.cc — shm_create 封装
int shm_create(size_t size, void **addr) {
    int fd = sys_shm_create(size);
    if (fd <= 0) return -1;
    *addr = sys_mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (!*addr) { sys_close(fd); return -1; }
    return fd;
}

// user/lib/sys_mman.cc — mmap 适配 6 参
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    // MAP_SHARED + fd ≥ 0 → SHM fd 映射
    // MAP_ANONYMOUS + fd == -1 → 匿名映射
    // MAP_PHYSICAL → 物理地址映射
    return sys_mmap(addr, length, prot, flags, fd, offset);
}
```

## 实现顺序

### Phase 1 — 基础设施（不改驱动行为）

```
1. kernel/proc.h
   - 定义 struct shm
   - struct file 加 FD_SHM + .shm 字段
   - struct mmap_region 加 .shm_obj 字段
   - 定义 SHM_KERNEL 常量
   - 声明 shm_get()/shm_put() 辅助函数

2. arch/x64/utils.h  + __syscall6

3. kernel/trap.h
   - syscall_fn_t 改 6 参

4. kernel/trap.cc
   - 所有 syscall 实现签名加 `, uint64_t arg6`
   - syscall_dispatch 传 tf->r9
```

### Phase 2 — 核心逻辑

```
5. kernel/trap.cc
   - 实现 shm_get()/shm_put()
   - 重写 sys_shm_create → 返回 fd
   - 重写 sys_shm_attach → 返回 fd（过渡期，与原 mode=0/1 逻辑解耦）
   - 重写 sys_mmap → 6 参 + SHM fd 映射路径
   - sys_munmap → 识别 mmap_region.shm_obj 并 shm_put
   - sys_close → FD_SHM 路径 shm_put
   - register_kernel_shm → 接受 struct shm * 指针

6. common/syscall.h
   - sys_mmap 签名改 6 参
   - sys_shm_create/sys_shm_attach 返回 int（fd）
```

### Phase 3 — 用户态适配

```
7. user/lib/sys_shm.cc — shm_create/shm_attach/shm_attach_kernel 适配新内核
8. user/lib/sys_mman.cc — mmap 6 参
9. driver/kbd_driver.cc — fd + mmap
10. driver/kms_driver.cc — fd + mmap
11. driver/terminal.cc — fd + mmap
12. driver/fs_driver.cc — fd + mmap
13. driver/display.h — display_backend_init/display_client_init 内联调用适配
14. init/init.c, shell/shell.cc — 检查 SHM 调用
```

### Phase 4 — 清理

```
15. kernel/proc.h — 删 shm_region、SHM_VADDR_BASE、MAX_SHM_PER_PROC
16. kernel/trap.cc — 删 kernel_shm_table、register_kernel_shm 旧实现
17. kernel/proc.cc — proc_reap 删 shm_regions 扫描、proc_init 删 shm_regions 清零
18. common/shm.h — 确认结构体定义兼容
19. Build & test
```

## 与其他文档的关系

| 文档 | 关系 |
|------|------|
| `doc/design/ipc.md` | SHM fd 化是 P1 项的前置条件（"SHM 改为 fd 模式"） |
| `doc/design/dynamic_shm_migration.md` | 本文完成后，dynamic_shm_migration.md 描述的历史过渡方案被彻底替代 |
| `doc/design/kms.md` | Display SHM 从 `shm_attach` 改为 `mmap(fd, ...)` |
| `doc/design/kbd.md` | USB HID SHM + kbd SHM 均改为 fd 模式 |
| `doc/design/libc.md` | `sys_mmap` 签名变化影响 libc 封装 |
