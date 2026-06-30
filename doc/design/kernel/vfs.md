# VFS + 文件系统设计

## 当前架构设计

用户进程通过统一 syscall 入口（sys_open/sys_read/sys_write/sys_close/sys_dup2/sys_lseek/sys_ioctl/sys_fstat）访问所有 fd 类型，内核按 fd type 分发：

| fd 类型 | 分发目标 | IPC |
|---------|---------|-----|
| FD_REGULAR(2) | inode → FAT32 → page cache → AHCI | 零 |
| FD_DIR(4) | inode → FAT32 getdents | 零 |
| FD_DEV(3) | inode → dev_ops callback / IPC 代理 | 内核设备零，用户态驱动走 req |
| FD_PIPE(1) | 内核 ring buffer 直通 | 零 |
| FD_SOCKET(5) | per-socket lock + skb queue | 零 |
| FD_SHM(6) | mmap 映射访问 | 零 |
| FD_TTY(8) | pty_read/pty_write | 零 |
| FD_FILE(7) | IPC 代理到 fs_driver（过渡保留） | IPC 代理 |

### struct file（fd entry）

定义：kernel/proc.h : file

字段：
  type : int — FD_NONE/FD_PIPE/FD_REGULAR/FD_DEV/FD_DIR/FD_SOCKET/FD_SHM/FD_FILE/FD_TTY
  flags : int — O_RDONLY / O_WRONLY / O_RDWR / O_APPEND / O_NONBLOCK
  inode : inode* — FD_REGULAR / FD_DEV / FD_DIR 共用
  offset : uint64_t — per-open 文件偏移（FD_REGULAR 专用）
  union:
    pipe : pipe* — FD_PIPE
    shm : shm* — FD_SHM
    target_pid : pid_t — FD_DEV（用户态驱动 PID）
    file_data : { fs_pid : pid_t, fs_fd : int32_t, _offset : uint64_t, file_size : uint64_t, ref_count : int } — FD_FILE（过渡）
    sock : unix_sock* — FD_SOCKET
    pty : pty* — FD_TTY

### fd_table

定义：kernel/proc.h : files_t

files_t 包含 fd_table[MAX_FD=32] 固定数组和 ref_count（fork 共享引用计数）。FD 分配：线性扫描 fd_table[3..31] 找 type==FD_NONE 空槽。files_t 独立引用计数，fork 时共享（ref_count++），写时按需复制。

---

# 一、inode 抽象

### struct inode

定义：kernel/inode.h : inode

类型常量：INODE_REGULAR=1, INODE_DIR=2, INODE_DEV=3

字段：
  type : int — INODE_REGULAR / INODE_DIR / INODE_DEV
  ino : uint32_t — inode 号（FAT32 start_cluster，设备自动递增）
  size : uint64_t
  mode : uint32_t — 权限位 (S_IFREG | 0644 等)
  nlink : int — 硬链接数（FAT32 无硬链接，始终 1）
  ref_count : int — 打开 fd 数 + dentry 引用
  i_lock : spinlock_t — per-inode spinlock
  i_priv : void* — INODE_DEV: 指向 dev_ops*; INODE_REGULAR/DIR: NULL
  shm : shm* — INODE_DEV 关联的 SHM（dev_create 时设置），mmap 从此取物理页
  start_cluster / dir_start_cluster : uint32_t — FAT32 专属
  dir_entry_index : int — 在目录中的短名 entry index
  hash_next / hash_prev : inode* — hash chain

inode 号 (ino)：FAT32 的 start_cluster 作为 inode 号，自然唯一。根目录 ino = BPB.root_cluster。设备 inode 自动递增分配。

### inode cache

定义：kernel/inode.c

全局 hash 表（INODE_HASH_BITS=6, 64 buckets），全局 inode_hash_lock。操作：inode_lookup(ino) 命中时 ref_count++；inode_create(ino, type, ...)；inode_put(inode) ref_count-- 归零时从 hash 表移除并释放。

生命周期：open → inode_lookup 或 inode_create → ref_count=1；同文件再次 open → lookup 命中 ref_count++；close → inode_put ref_count--；ref_count=0 从 hash 表移除。

### inode 锁模型

- **i_lock**：per-inode spinlock，保护 O_APPEND offset 写入、FAT cluster 分配+修改、文件 size 更新
- **fat_lock**：全局 spinlock，保护 FAT 空闲簇查找 + FAT entry 修改
- **page_cache_lock**：全局 spinlock，保护 page cache hash + LRU
- **锁获取顺序**：i_lock → fat_lock（固定，防死锁）
- **写路径锁序列**：acquire i_lock → 写数据 → acquire fat_lock → FAT 分配/修改 → release fat_lock → 更新目录项 → release i_lock

---

# 二、Page cache

### 设计参数

| 参数 | 值 | 说明 |
|------|-----|------|
| page 粒度 | 4KB | 匹配 x86-64 PAGE_SIZE |
| cache key | (inode*, page_index) | page_index = offset / PAGE_SIZE |
| cache 大小 | 1024 pages (4MB) | 固定大小，LRU 淘汰 |
| 淘汰策略 | LRU | pin_count > 0 或 dirty 时不淘汰 |

### 数据结构

定义：kernel/page_cache.h : cache_page

字段：inode / page_index / data(uint8_t*, kmalloc(4096) 延迟分配) / pin_count / dirty(bool) / lru_next / lru_prev / hash_next

常量：PAGE_CACHE_HASH_BITS=6, PAGE_CACHE_SIZE=1024

### 操作

- page_cache_lookup(inode, page_index) — 命中返回 data ptr，未命中返回 NULL
- page_cache_fill(inode, page_index) — 未命中时从磁盘读 sector(s) 到 page，插入 hash + LRU head
- page_cache_evict() — LRU tail 淘汰（pin_count=0 且 dirty=false 时才可淘汰）
- page_cache_mark_dirty / page_cache_writeback — 标记 dirty / 写回磁盘

**写路径**：fat32_write 在写入后立即调用 page_cache_mark_dirty + page_cache_writeback（即时写回，无延迟写回机制）。

---

# 三、Devtmpfs

### 概述

`/dev/` 是内核维护的内存伪文件系统。驱动启动时通过 dev_create() 注册设备节点，创建 inode（INODE_DEV）。用户 open("/dev/kbd") 时 VFS 通过前缀拦截跳转到 devtmpfs。

### dev_ops（file_operations 等价物）

定义：kernel/devtmpfs.h : dev_ops

字段：
  driver_pid : pid_t — 0=内核设备（内核回调），>0=用户态驱动（IPC 代理）
  device_type : uint32_t — DEV_KMS / DEV_SERIAL / DEV_BLK 等
  回调函数指针（仅 driver_pid==0 时调用）：open / close / ioctl / mmap / read / write / poll

NULL 回调 = 默认行为（ops->read==NULL → sys_read 返回 -EINVAL）。

### 内核设备实例

| 设备 | driver_pid | device_type | 回调 |
|------|-----------|-------------|------|
| KMS | 0 | DEV_KMS | ioctl=display_ioctl, mmap=display_mmap_handler_ioctl |
| Serial | 0 | DEV_SERIAL | open/close/read/write/ioctl(TCGETS)/poll |
| Block | 0 | DEV_BLK | open/close/read/write |

### devtmpfs 操作

- devtmpfs_create(name, dev_type, &ops, shm) — 创建 INODE_DEV inode，i_priv=ops，shm!=NULL 时设置 inode->shm 并 shm_get 增加引用，同步填 isr_driver_pid[dev_type]
- devtmpfs_lookup(name) — 链表线性扫描
- devtmpfs_open(name, flags) — 分配 FD_DEV fd，内核设备调 ops->open()
- devtmpfs_cleanup_pid(pid) — 删除 driver_pid 匹配的所有 inode，清零 isr_driver_pid[]

### VFS 路径拦截

sys_open 中：path 以 /dev/ 开头 → devtmpfs_open(path+5, flags)；否则 → fat32_open(path, flags)。

实现：kernel/vfs.c : sys_open

预留 mount 接口：未来支持 mount("/dev", devtmpfs) 挂载点穿越。

### ISR 唤醒机制

内核维护 isr_driver_pid[DEV_TYPE_MAX] 数组（定义：kernel/devtmpfs.c）。xHCI ISR 调 wake_process(isr_driver_pid[DEV_KBD])，只唤醒不入队。用户态驱动 recv 被唤醒但队列为空时返回 -EINTR，驱动自行检查 SHM ring buffer 处理硬件事件。

---

# 四、sys_ioctl / sys_fstat

### sys_ioctl — syscall #54

签名：long sys_ioctl(int fd, uint32_t cmd, uint64_t arg)

内核从 cmd 位域取 _IOC_DIR(cmd) 和 _IOC_SIZE(cmd)，决定 copy_from_user/copy_to_user 的方向和大小。

| fd 类型 | 行为 |
|---------|------|
| FD_DEV（内核设备） | ops->ioctl(cmd, kbuf)，arg 通过内核缓冲区中转 |
| FD_DEV（用户态驱动） | IPC proxy：cmd + arg 打包到 56B REQ → sys_req → 驱动 recv/resp |
| FD_TTY | pty_ioctl(cmd, arg) |
| 其他 | 返回 -ENOTTY |

ioctl 命令编码：采用 Linux _IOC 宏编码，定义：common/ioctl.h。_IO / _IOW / _IOR / _IOWR 宏，cmd 自带方向、类型、序号、大小。

### sys_fstat — syscall #55

签名：int sys_fstat(int fd, struct kstat *buf)

| fd 类型 | 行为 |
|---------|------|
| FD_REGULAR / FD_DIR | 从 inode 填充 kstat |
| FD_DEV | 从 inode 填充 kstat（mode = S_IFCHR） |
| FD_PIPE | mode = S_IFIFO, size = pipe buffer 数据量 |
| FD_TTY | mode = S_IFCHR |
| FD_SHM | mode = S_IFREG, size = shm->file_size |

---

# 五、FD_DEV 内核 IPC 代理

### read/write 代理

sys_read(fd) 对 FD_DEV：driver_pid==0 且 ops->read 非 NULL → 内核回调 ops->read(current_task, fd, buf, len)；用户态驱动 → IPC proxy（构造 req，sys_req 到 driver_pid）。

sys_write(fd) 同理。

### close 路径

sys_close(fd) 对 FD_DEV：driver_pid==0 且 ops->close 非 NULL → ops->close(current_task, fd)（如 serial 注销 IRQ）；用户态驱动仅 inode_put，不做显式 close 通知。

### poll

sys_poll(fd) 对 FD_DEV：driver_pid==0 且 ops->poll 非 NULL → 内核回调 ops->poll(current_task, events)；用户态驱动 → 读 SHM 状态检查是否有数据。

---

# 六、FAT32 内核模块

### 源文件

kernel/fat32.c/h — FAT32 实现；kernel/vfs.c/h — VFS 层；kernel/devtmpfs.c/h — /dev/ 伪文件系统；kernel/inode.c/h — inode cache；kernel/page_cache.c/h — page cache；kernel/blk_dev.c/h — 块设备抽象（AHCI 接口封装）

### FAT32 锁

| 锁 | 类型 | 保护范围 |
|----|------|---------|
| i_lock | per-inode spinlock | 文件 size 更新、O_APPEND offset 写入、目录项修改 |
| fat_lock | 全局 spinlock | FAT 空闲簇查找 + FAT entry 修改 |
| page_cache_lock | 全局 spinlock | page cache hash + LRU 操作 |

### FAT32 操作

| 操作 | 说明 |
|------|------|
| fat32_open(path, flags) | 路径解析 → 创建 inode → 安装 fd。O_CREAT/O_TRUNC 在内处理 |
| fat32_read(inode, offset, buf, count) | page cache 查找/填充 → 拷贝到用户 buf |
| fat32_write(inode, offset, buf, count) | page cache 查找/填充 → 拷贝到 page → mark dirty → 即时写回 |
| fat32_stat(path) | 路径解析 → 返回 inode 信息 |
| fat32_mkdir(path) | 父目录解析 → 分配簇 → 初始化 . .. → 写目录项 |
| fat32_unlink(path) | 标记目录项 0xE5 → 释放 FAT 簇链 → 失效 page cache |
| fat32_rmdir(path) | 验证目录空 → 标记删除 → 释放簇 |
| fat32_getdents(fd, buf, count) | 目录项枚举，LFN 支持 |

### 磁盘布局

disk.img (64MB, 131072 扇区)：LBA 0=MBR 分区表；MBR 分区1(type=0xDA) LBA 1-300 裸 ELF 存储（init.elf 在 LBA 201-300）；MBR 分区2(type=0x0C) LBA 301-131071 FAT32 文件系统。FAT32 使用 -s 8（4KB/簇）。

### 启动流程

UEFI → kernel_main → FAT32 初始化 → spawn init.elf → init spawn kbd/kms/terminal → shell

---

# 七、VFS syscall 编号

| # | 名称 | 说明 |
|---|------|------|
| 47 | sys_open | open(path, flags, mode) |
| 48 | sys_stat | stat(path, &buf) |
| 49 | sys_mkdir | mkdir(path, mode) |
| 50 | sys_unlink | unlink(path) |
| 51 | sys_rmdir | rmdir(path) |
| 52 | sys_dev_create | dev_create(name, dev_type) — 内核自动填 driver_pid |
| 53 | sys_getdents | getdents(fd, buf, count) |
| 54 | sys_ioctl | ioctl(fd, cmd, arg) |
| 55 | sys_fstat | fstat(fd, &buf) |
| 56 | sys_fdev_pid | 查询 fd 对应的驱动 PID |

---

# 八、与 Linux VFS 对比

| 特性 | Linux | 当前实现 |
|------|-------|--------|
| fd_table | files_struct + 动态 fdtable | files_t + 固定 32 项数组 + ref_count 共享 |
| 文件系统 | ext4/btrfs | FAT32 |
| inode | struct inode + hash cache | struct inode + hash cache |
| page cache | 4KB page, (mapping, index) | 4KB page, (inode, page_index) |
| 设备发现 | devtmpfs + major:minor | devtmpfs + inode + dev_ops 回调 |
| file_operations | 函数指针表 | dev_ops 回调（内核）+ IPC 代理（用户态驱动） |
| pipe | pipe_read/write | ring buffer (4KB) |
| socket lock | per-socket lock | 全局 socket_lock |
| SCM_RIGHTS | unix_inflight ref bump | skb fds[] + lazy 安装 |
| mount | mount_table + 挂载点穿越 | 前缀拦截（预留 mount 接口） |
| ioctl | _IOC 编码 + file_operations | _IOC 编码 + dev_ops / IPC 代理 |
| dentry cache | 路径缓存 | 无（每次 open 读磁盘解析路径） |

---

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| FD_FILE 消除 | FD_FILE(=7) 是 fs_driver IPC 代理遗留类型，消除后 sys_msg/sys_msg_resp 也可废弃 | 高 |
| sys_install_fd 消除 | syscall #29 仅 FD_FILE 使用，FD_FILE 消除后删除 | 高 |
| fd_table 动态扩展 | 固定 32 项 → bitmap + 动态扩展（初始 32，超限 kmalloc），构建 gcc 可能需要 256+ | 中 |
| dentry cache | 无路径缓存，每次 open 读磁盘解析路径；添加 (path, inode) hash + LRU 缓存 | 高 |
| FAT 锁细化 | 全局 fat_lock → per-FAT-sector spinlock | 中 |
| readahead | 顺序检测 + 预取 4 簇 | 中 |
| dirty page 延迟写回 | 当前即时写回，改为 mark dirty → 定时 flush | 中 |
| page cache 动态扩展 | 固定 4MB → 按空闲内存比例 reclaim | 低 |
| pipe 优化 | buffer 从 4KB 扩大到 64KB，byte-by-byte 改为 batch memcpy | 低 |
| socket per-socket lock | 全局 socket_lock → per-socket spinlock | 中 |
| mount 支持 | 前缀拦截 → mount_table + 挂载点穿越 | 低 |
| 通用块设备层 | AHCI 直接调用 → 通用 blk_dev 接口（支持 NVMe 等） | 低 |
| page cache + mmap 共享 | sys_mmap(FD_REGULAR) 映射 page cache 物理页到用户地址空间，同一文件 read + mmap 共享数据 | 中 |
| dev_ops->close 签名妥协 | `file_put` FD_DEV 路径传 `current_task` + `fd=-1` 给 `ops->close`；当前 blk_dev_close 不使用参数，安全。未来新设备驱动 close handler 如需 file 信息，改签名为 `int (*close)(struct file *f)` | 低 |
