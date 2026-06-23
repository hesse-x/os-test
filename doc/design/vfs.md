# VFS + fd 系统重构设计

> FAT32 文件系统从用户态 fs_driver 搬入内核，建立 inode + page cache + VFS 统一 I/O 架构。所有 fd 类型走 `sys_read/sys_write/sys_close` 内核统一分发，消除 IPC 代理开销。devtmpfs 提供 `/dev/` 设备节点命名空间。

## 1. 架构总览

```
用户进程
  │
  ├─ open("/path") = sys_open(path, flags)        ← 统一入口（内核直接处理）
  ├─ read(fd, buf, n) = sys_read(fd, buf, n)     ← 统一入口（内核 dispatch）
  ├─ write(fd, buf, n) = sys_write(fd, buf, n)   ← 统一入口
  ├─ close(fd) = sys_close(fd)                    ← 统一入口
  ├─ dup2(old, new) = sys_dup2(old, new)         ← 统一入口
  ├─ lseek(fd, off, whence) = sys_lseek(...)      ← 统一入口
  │
  ▼
内核 VFS dispatch（按 fd type 分发）
  │
  ├─ FD_REGULAR: inode → FAT32 → page cache → AHCI  ← 零 IPC
  ├─ FD_DEV:     inode → driver ops → kernel_msg_send → 用户态驱动 ← IPC 代理
  ├─ FD_PIPE:    内核 ring buffer 直通                  ← 零 IPC
  ├─ FD_SOCKET:  per-socket lock + skb queue            ← 零 IPC
  ├─ FD_SHM:     mmap 映射访问                          ← 零 IPC
  ├─ FD_SERIAL:  内核 UART ring buffer                  ← 零 IPC
```

**核心变化**：FD_REGULAR（原 FD_FILE）不再 IPC 到用户态 fs_driver，FAT32 在内核直接操作。FD_DEV 走内核 IPC 代理到用户态驱动，协议由驱动注册的 `dev_ops` 决定（`driver_pid=0` 为内核设备，直接处理）。

## 2. 与旧架构对比

| 维度 | 旧架构 | 新架构 |
|------|--------|--------|
| FAT32 位置 | 用户态 fs_driver (4167 行) | 内核 `kernel/fat32.c` |
| 文件 I/O 路径 | user→kernel→IPC→fs_driver→kernel→AHCI→disk→返回 (4+ context switch) | user→kernel→FAT32→AHCI→disk→返回 (0 context switch) |
| fd 元信息 | `struct file.file_data {fs_pid, fs_fd, offset, file_size, ref_count}` | `struct file.inode + offset` (offset per-open, ref_count per-inode) |
| fd 分配 | 线性扫描 MAX_FD=32 固定数组 | bitmap + 动态扩展（初始 32，超限 kmalloc） |
| libc fd_table | libc 维护独立 fd_table 做类型分发 | 删除，统一走内核 syscall |
| /dev/ 发现 | `dev_table[DEV_TYPE]` + `sys_open_dev` | devtmpfs (`/dev/` 内存伪文件系统) + `sys_open` |
| 设备注册 | `sys_load_dev(pid, DEV_KBD)` | `dev_create("/dev/kbd", &ops)` → devtmpfs inode |
| socket 锁 | 全局 socket_lock | per-socket mutex |
| pipe buffer | 4KB + byte-by-byte | 64KB + batch memcpy |
| SCM_RIGHTS | lazy install（无 inflight ref） | unix_inflight: sendmsg 时 bump ref_count |
| close FD_DEV | proc_reap 阻塞等 IPC 回复 | 异步 sys_notify，不阻塞内核 |
| page cache | 无 | 4KB page, (inode, page_index) key, 4MB LRU |
| inode | 无 | per-file inode, ref_count + per-inode mutex |
| O_APPEND | 无 | per-inode mutex 保护 offset = file_size |
| O_CREAT | libc 三次 IPC (open→create→open) | 内核一次操作 |

## 3. inode 抽象

### 3.1 struct inode

```c
// kernel/inode.h

#define INODE_REGULAR  1   // FAT32 文件
#define INODE_DIR      2   // FAT32 目录
#define INODE_DEV      3   // devtmpfs 设备节点

struct inode {
    int      type;           // INODE_REGULAR / INODE_DIR / INODE_DEV
    uint32_t ino;            // inode 号 = FAT32 start_cluster（REGULAR/DIR）
    uint64_t size;           // 文件大小
    uint32_t mode;           // 权限位 (S_IFREG | 0644 等)
    int      nlink;          // 硬链接数（FAT32 无硬链接，始终 1）
    int      ref_count;      // 打开 fd 数 + dentry 引用
    spinlock_t i_lock;       // per-inode spinlock（保护 size 写入 + FAT 修改）
    void    *i_priv;         // INODE_DEV: 指向 dev_ops*; INODE_REGULAR: NULL

    // FAT32 专属元数据（REGULAR/DIR）
    uint32_t start_cluster;
    uint32_t dir_start_cluster;  // 所在目录的 start_cluster
    int      dir_entry_index;    // 在目录中的短名 entry index

    // Hash chain
    struct inode *hash_next;
    struct inode *hash_prev;
};
```

**inode 号 (ino)**：FAT32 的 start_cluster 作为 inode 号，自然唯一。根目录 ino = BPB.root_cluster。

### 3.2 inode cache

全局 hash 表，key = `(ino)`，O(1) 查找：

```c
// kernel/inode.c
#define INODE_HASH_BITS  6
#define INODE_HASH_SIZE  (1 << INODE_HASH_BITS)  // 64 buckets

struct inode *inode_hash_table[INODE_HASH_SIZE];

static unsigned inode_hash(uint32_t ino) {
    return ino & (INODE_HASH_SIZE - 1);
}

struct inode *inode_lookup(uint32_t ino);
struct inode *inode_create(uint32_t ino, int type, ...);
void inode_put(struct inode *inode);  // ref_count--, 0 时从 hash 表移除并释放
```

**生命周期**：
- `open()` → `inode_lookup(ino)` 或 `inode_create()` → ref_count=1
- 同文件再次 open → `inode_lookup` 命中 → ref_count++
- `close()` → `inode_put()` → ref_count-- → >0 保留在 hash 表
- ref_count=0 → 从 hash 表移除，释放 inode struct（回到 slab allocator）
- 即使 ref_count=0，如果 dentry cache 还引用此 inode，inode 不会被释放（dentry 持有引用）

### 3.3 inode 锁模型

- **i_lock**：per-inode spinlock，保护以下操作的原子性：
  - O_APPEND write：`offset = i_size` 必须和数据写入在同一锁内
  - FAT 修改：cluster 分配 + FAT dual-write + 目录项更新
  - 文件 size 更新
- **fat_lock**：全局 spinlock，保护 FAT 表空闲簇查找 + FAT entry 修改
  - 未来优化为 per-FAT-sector spinlock
- **锁获取顺序**：`i_lock → fat_lock`（固定，防死锁）
- **写路径锁序列**：acquire i_lock → 写数据 → acquire fat_lock → FAT 分配/修改 → release fat_lock → 更新目录项 → release i_lock

## 4. struct file（fd entry）

```c
// kernel/proc.h

#define FD_NONE    0
#define FD_PIPE    1
#define FD_SHM     2
#define FD_DEV     3    // 设备 fd（IPC 代理到用户态驱动）
#define FD_FILE    4   // 旧文件 fd（IPC 代理到 fs_driver，过渡保留）
#define FD_SOCKET  5   // AF_UNIX socket
#define FD_SERIAL  6   // 内核串口
#define FD_REGULAR 7   // 文件 fd（内核 FAT32 直通）

struct file {
    int      type;
    int      flags;           // O_RDONLY / O_WRONLY / O_RDWR / O_APPEND / O_NONBLOCK
    struct inode *inode;      // FD_REGULAR 和 FD_DEV 共用
    uint64_t offset;          // per-open 文件偏移（FD_REGULAR 专用）
    union {
        struct pipe    *pipe;    // FD_PIPE
        struct shm     *shm;     // FD_SHM
        pid_t target_pid;       // FD_DEV (driver PID)
        struct {                 // FD_FILE (过渡)
            pid_t fs_pid;
            int   fs_fd;
            uint64_t file_size;
            int ref_count;
        };
        struct unix_sock *sock;  // FD_SOCKET
        // FD_SERIAL: 无额外数据
    };
};
```

**关键变化**：
- offset 从 `file_data` 移到 `struct file` 顶层（per-open，per-fd）
- ref_count 从 fd entry 移到 inode（per-file，共享）
- inode 指针统一 FD_REGULAR 和 FD_DEV
- FD_FILE 保留作为过渡，新文件操作统一走 FD_REGULAR

## 5. Page cache

### 5.1 设计参数

| 参数 | 值 | 说明 |
|------|-----|------|
| page 粒度 | 4KB | 匹配 x86-64 PAGE_SIZE，mmap 共享 |
| cache key | `(inode*, page_index)` | page_index = offset / PAGE_SIZE |
| cache 大小 | 1024 pages (4MB) | 固定大小，LRU 淘汰 |
| 淘汰策略 | LRU | 简单有效，未来可加 reclaim |

### 5.2 数据结构

```c
// kernel/page_cache.h

struct cache_page {
    struct inode   *inode;
    uint64_t        page_index;   // offset / PAGE_SIZE
    uint8_t        *data;         // 4KB 数据（物理页）
    int             pin_count;    // >0 时不允许淘汰（写期间 pin）
    bool            dirty;        // 需要写回磁盘
    struct cache_page *lru_next;
    struct cache_page *lru_prev;
    struct cache_page *hash_next;  // hash 链
};

#define PAGE_CACHE_HASH_BITS  6
#define PAGE_CACHE_SIZE       1024  // pages

struct page_cache {
    struct cache_page *hash_table[1 << PAGE_CACHE_HASH_BITS];
    struct cache_page *lru_head;     // 最近使用
    struct cache_page *lru_tail;     // 最久未用（淘汰候选）
    struct cache_page *free_list;    // 预分配 page slots
    int                nr_pages;     // 当前在用 page 数
};
```

### 5.3 操作

- `page_cache_lookup(inode, page_index)` → 命中返回 data ptr，未命中返回 NULL
- `page_cache_fill(inode, page_index)` → 未命中时从磁盘读 sector(s) 到 page，插入 hash + LRU head
- `page_cache_evict()` → LRU tail 淘汰（pin_count=0 且 dirty=false 时才可淘汰）
- `page_cache_mark_dirty(inode, page_index)` → 标记 dirty，淘汰前需写回磁盘
- `page_cache_writeback(inode, page_index)` → dirty page 写回磁盘

### 5.4 与 mmap 共享

`sys_mmap(fd)` 对 FD_REGULAR → 找 inode → page_cache_lookup → 命中时直接映射物理页到用户地址空间。同一文件 read + mmap 共享数据，0 重复缓存。

## 6. Devtmpfs

### 6.1 概述

`/dev/` 是内核维护的内存伪文件系统。驱动启动时通过 `dev_create()` 注册设备节点，创建 inode（INODE_DEV）。用户 `open("/dev/kbd")` 时 VFS 通过前缀拦截跳转到 devtmpfs。

### 6.2 dev_create

```c
// 新 syscall: sys_dev_create(name, type, ops_ptr)
// 或: 驱动调用 dev_create("/dev/kbd", S_IFCHR, &kbd_ops)

struct dev_ops {
    pid_t    driver_pid;        // 驱动进程 PID（0 = 内核设备，不走 IPC 代理）
    uint32_t device_type;       // DEV_KBD, DEV_KMS 等
};
```

驱动启动后：

```
kbd_driver: getpid() = 8
kbd_driver: dev_create("/dev/kbd", S_IFCHR, &kbd_ops)  → 内核在 devtmpfs 创建 inode
  inode.ino = auto_increment
  inode.i_priv = &kbd_ops
```

驱动 crash → 内核 `dev_table_cleanup(pid=8)` → 扫描 devtmpfs → 删除 driver_pid=8 的所有 inode → `open("/dev/kbd")` 返回 -ENXIO

### 6.3 VFS 路径拦截（过渡方案）

```c
// kernel/vfs.c — sys_open 实现
int sys_open(const char *path, int flags, ...) {
    if (strncmp(path, "/dev/", 5) == 0) {
        // 前缀拦截： 跳过 FAT32，直接查 devtmpfs
        return devtmpfs_open(path + 5, flags);
    }
    // 否则: FAT32 路径解析
    return fat32_open(path, flags);
}
```

**预留 mount 接口**：未来支持 `mount("/dev", devtmpfs)` 挂载点穿越，路径解析时每个分量检查 mount_table。

## 7. FD_DEV 内核 IPC 代理

### 7.1 read/write 代理

```c
sys_read(fd, buf, len):
    if fd->type == FD_DEV:
        inode = fd->inode;
        dev_ops *ops = (dev_ops*)inode->i_priv;
        // 构造 IPC 消息
        req = {cmd: ops->read_cmd, session_fd: inode->ino, count: len};
        // 内核 IPC 代理到驱动
        n = kernel_msg_send(ops->driver_pid, &req, ..., buf, len);
        return n;

sys_write(fd, buf, len):
    if fd->type == FD_DEV:
        // 同理，用 ops->write_cmd
        ...
```

### 7.2 close 代理（异步通知）

```c
// proc_reap 清理 FD_DEV:
if fd->type == FD_DEV:
    inode = fd->inode;
    dev_ops*ops = (dev_ops*)inode->i_priv;
    // 检查驱动是否存活（查 devtmpfs 中 driver_pid 是否还有 inode）
    if (driver_alive(ops->driver_pid)) {
        // 异步通知，不阻塞内核
        sys_notify(ops->driver_pid, ops->close_cmd, inode->ino);
    }
    inode_put(inode);  // 释放 inode ref
```

### 7.3 poll（SHM 状态 + sys_notify）

```c
sys_poll(fd, ...):
    if fd->type == FD_DEV:
        dev_ops*ops = (dev_ops*)inode->i_priv;
        // 读 SHM 状态检查是否有数据（零 IPC）
        uint32_t count = *(uint32_t*)(ops->shm_addr + ops->shm_count_offset);
        if (count > 0) return POLLIN;
        // 没数据: 注册为等待者，阻塞
        register_poll_waiter(fd, current_proc->pid);
        return BLOCKED;
```

驱动有新数据时：

```
kbd_driver: 有按键事件 → sys_notify(kernel_pid, KBD_NOTIFY_DATA_READY)
→ 内核唤醒 poll 等待者 → sys_poll 返回 POLLIN
```

## 8. Pipe 优化

| 参数 | 旧值 | 新值 | 说明 |
|------|------|------|------|
| buffer 大小 | 4KB (PIPE_BUF_SIZE) | 64KB | 和 Linux 对齐（PIPE_BUF=64KB） |
| 写入方式 | byte-by-byte 循环 | batch memcpy | 一次拷贝最大可用空间 |
| 读取方式 | byte-by-byte 循环| batch memcpy | 一次拷贝到 tail→head 之间所有数据 |
| POSIX guarantee | 不变 | 不变 | write ≤ PIPE_BUF 保证原子写入 |

| 阻塞模型 | WAIT_PIPE + 单 PID | WAIT_PIPE + 单 PID | 逻辑不变，只是 buffer 更大 |

## 9. Socket 改进

| 参数 | 旧值 | 新值 |
|------|------|------|
| 锁 | 全局 socket_lock | per-socket mutex |
| 死锁预防 | 无 | 固定获取顺序: listener → child, procs_lock → socket_mutex → scheduler_lock |
| poll | 全局 socket_lock 下 | per-socket mutex 下，不阻塞无关 socket pair |

connect/accept 同时锁两个 socket（listener + child），固定顺序防死锁。单一 sendmsg/recvmsg 只锁自己一端的 socket。

## 10. SCM_RIGHTS 修复

### 旧问题

sendmsg 存储原始 fd 号到 skb->fds[]，recvmsg 时从发送者的 fd_table 拷贝。如果发送者中间 close 或 exit，资源可能被释放 → 悬空引用。

### 新方案（unix_inflight）

```c
sendmsg(fd_array):
    for each fd in fd_array:
        validate fd exists
        bump ref_count of underlying resource:  // "inflight"
            FD_PIPE:  pipe->ref_count++
            FD_REGULAR: inode->ref_count++
            FD_SOCKET: unix_sock_acquire(sock)
            FD_SHM:    shm_get(shm)
        skb->fds[i] = fd   // 仍然存储 fd 号，但资源已被保护

recvmsg(skb):
    for each fd in skb->fds:
        copy sender->fd_table[fd] to receiver->fd_table[new_fd]
        bump ref_count again（双重 bump？不，inflight 已经 bump 了一次）
        // 实际上：只拷贝 entry，不再 bump（inflight 已经保活）
    // skb 完全消费后：
        for each fd in skb->fds:
            // 释放 inflight 引用（抵消 sendmsg 时的 bump）
            resource_put(sender->fd_table[fd])  // ref_count--
```

**效果**：即使发送者 close 或 exit，资源的 inflight ref_count 保证资源不被释放，直到接收者安装或 skb 被丢弃。

## 11. fd table 动态扩展

```c
// kernel/proc.h

struct fd_table {
    struct file  *entries;      // fd entry 数组（初始 32，可 kmalloc 扩展）
    unsigned int *open_fds;     // bitmap: 1=已分配, 0=空闲
    int           max_fds;      // 当前数组大小（初始 MAX_FD_INIT=32）
};

// proc_t 内嵌初始 fd_table
struct proc_t {
    struct file   fd_entries_init[32];  // 嵌入 PCB 的初始数组
    unsigned int  fd_bitmap_init[1];    // 32 bits = 1 uint
    struct fd_table fds;                // 指向上面两个（或 kmalloc 的大数组）
    ...
};
```

**分配**：`find_next_zero_bit(open_fds, max_fds, start_fd)` — O(1)
**释放**：`clear_bit(open_fds, fd)` — O(1)
**扩展**：超过 max_fds → `kmalloc(new_max * sizeof(file))` + `kmalloc(new_max / 32 * sizeof(uint))` + 拷贝旧数据

**初始 max_fds=32**，首次扩展到 128，再翻倍。构建 gcc 时可能需要 256+。

## 12. FAT32 内核模块

### 12.1 目录结构

```
kernel/
  fat32.c / fat32.h         — FAT32 文件系统实现（从 fs_driver.cc 迁入）
  vfs.c / vfs.h             — VFS 层（sys_open/sys_stat/sys_mkdir/sys_unlink 等）
  devtmpfs.c / devtmpfs.h   — /dev/ 内存伪文件系统
  inode.c / inode.h         — inode cache +hash 表+引用计数
  page_cache.c / page_cache.h — page cache (4KB pages, LRU)
  blk_dev.c / blk_dev.h     — 块设备抽象层（AHCI 接口）
```

### 12.2 FAT32 锁

| 锁 | 类型 | 保护范围 |
|----|------|---------|
| i_lock | per-inode spinlock | 文件 size 更新、O_APPEND offset 写入、目录项修改 |
| fat_lock | 全局 spinlock | FAT 空闲簇查找 + FAT entry 修改（未来: per-FAT-sector spinlock） |
| page_cache_lock | 全局 spinlock | page cache hash +LRU 操作 |

### 12.3 FAT32 操作

| 操作 | 说明 |
|------|------|
| fat32_open(path, flags) | 路径解析 → 创建 inode → 安装 fd → 返回 fd |
| fat32_read(inode, offset, buf, count) | page cache 查找/填充 → 拷贝到用户 buf |
| fat32_write(inode, offset, buf, count) | page cache 查找/填充 → 拷贝到 page → mark dirty → 写回 |
| fat32_close(inode) | inode_put → ref_count=0 时释放 inode |
| fat32_stat(path) | 路径解析 → 返回 inode 信息 |
| fat32_mkdir(path) | 爯目录解析 → 分配簇 → 初始化 `.` `..` → 写目录项 |
| fat32_unlink(path) | 标记目录项 0xE5 → 释放 FAT 簇链 |
| fat32_rmdir(path) | 验证目录空 → 标记删除 → 释放簇 |

O_CREAT/O_TRUNC 在 fat32_open 内处理：文件不存在时直接创建（一次操作，非三次 IPC）。

## 13. 启动流程变化

### 旧流程
```
UEFI → kernel_main → spawn fs_driver.elf → spawn init.elf → init spawn kbd/kms/terminal → shell
```

### 新流程
```
UEFI → kernel_main → FAT32 初始化 → spawn init.elf → init spawn kbd/kms/terminal → shell
```

不再加载 fs_driver.elf。磁盘布局删除 LBA 101-200 的 fs_driver slot。

init.c 不再 spawn fs_driver +waitpid(fs_driver_pid)。

## 14. 过渡与删除的组件

| 组件 | 文件 | 说明 |
|------|------|------|
| fs_driver | `driver/fs_driver.cc` | 4167 行，整文件删除 |
| sys_open_dev | syscall #32 | devtmpfs 替代 |
| sys_load_dev | syscall #18 | dev_create 替代 |
| dev_table 数组 | `kernel/trap.cc` | 被 devtmpfs inode 替代 |
| fs_driver.elf 磁盘 slot | `build_script/mkdisk.sh` | LBA 101-200 删除 |

**过渡保留**：FD_FILE、sys_install_fd (#36)、kernel_msg_send 仍保留，供旧路径过渡使用。待 libc 全部迁移到 sys_open 后可删除。

## 15. 与 Linux VFS 对比

| 特性 | Linux | 新设计 |
|------|-------|--------|
| fd_table | 内核 `files_struct` + 动态 `fdtable` | 内核 `fd_table` + bitmap + 动态扩展 |
| 文件系统 | 内核 ext4/btrfs | 内核 FAT32 |
| inode | 内核 `struct inode` + hash cache | 内核 `struct inode` + hash cache |
| page cache | 4KB page, (mapping, index) | 4KB page, (inode, page_index) |
| 设备发现 | devtmpfs + major:minor | devtmpfs + inode |
| file_operations | 函数指针表 | type dispatch +IPC 代理(FD_DEV) |
| pipe | 内核 pipe_read/write | 内核 ring buffer (64KB) |
| socket lock | per-socket lock | per-socket mutex |
| SCM_RIGHTS | unix_inflight ref bump | unix_inflight ref bump |
| mount | mount_table + 挂载点穿越 | 前缀拦截（预留 mount 接口） |

## 16. 性能优化项（todo）

| 优化 | 当前方案 | 目标方案 | 优先级 |
|------|----------|----------|--------|
| FAT 锁细化 | 全局 fat_lock (spinlock) | per-FAT-sector spinlock | 中 |
| dentry cache | 无（每次 open 读磁盘解析路径） | `(path, inode)` hash + LRU | 高 |
| readahead | 无 | 顺序检测 + 预取 4 簇 | 中 |
| page cache 动态扩展 | 固定 4MB | 按空闲内存比例 reclaim | 低 |
| block device 层 | AHCI 直接调用 | 通用 blk_dev 接口（支持 NVMe 等） | 低 |
| dirty page writeback | 写时立即写磁盘 | 延迟写回（mark dirty → 定时 flush） | 中 |
