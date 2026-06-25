# 设备 VFS 集成 — POSIX file_operations 对齐

将设备访问从专用 syscall（sys_open_dev/sys_dev_req）迁移到 VFS 统一路径：`open("/dev/xxx") → ioctl(fd, cmd, arg)`。内核 `dev_ops` 演进为 Linux `file_operations` 等价物。

## 设计动机

当前设备访问有两条并行路径：
- `/dev/` 路径：libc 硬编码字符串匹配 → `sys_open_dev(DEV_XXX)` → 查 `dev_table[]` → 返回 packed fd
- VFS 路径：`sys_open("/dev/kms")` → `devtmpfs_open()` → 返回 FD_DEV fd

两条路径做同样的事，但 sys_open_dev 额外维护了一个全局 `dev_table[DEV_TYPE_MAX]`，和 devtmpfs 的 `inode->i_priv (dev_ops*)` 信息冗余。且内核对每种设备硬编码 `if (device_type == DEV_KMS)` 分发，加新设备必须改 trap.c 多处。

核心思路：**驱动自己注册到 /dev/，上层从 /dev/ 读取所需驱动**。对齐 Linux：VFS 是唯一入口，file_operations 是唯一分发机制。

## 架构决策

### 1. dev_ops → file_operations

`dev_ops` 从 `(driver_pid, device_type)` 扩展为完整操作向量，对齐 Linux `struct file_operations`：

```c
// kernel/devtmpfs.h
struct dev_ops {
    pid_t    driver_pid;     // 0 = kernel device, >0 = user-space driver
    uint32_t device_type;    // DEV_KMS, DEV_SERIAL, etc.

    // VFS 回调（仅 driver_pid == 0 时调用）
    int      (*open)(proc_t *proc, int fd);            // .open
    int      (*close)(proc_t *proc, int fd);           // .release
    long     (*ioctl)(uint32_t cmd, void __user *arg); // .unlocked_ioctl
    int      (*mmap)(proc_t *proc, uint64_t size);     // .mmap
    ssize_t  (*read)(proc_t *proc, int fd,
                     void __user *buf, size_t count);   // .read
    ssize_t  (*write)(proc_t *proc, int fd,
                      const void __user *buf, size_t count); // .write
    __poll_t (*poll)(proc_t *proc, int events);        // .poll
};
```

NULL 回调 = 默认行为（和 Linux 一致）：
- `ops->read == NULL` → `sys_read` 对该设备返回 `-EINVAL`
- `ops->open == NULL` → `devtmpfs_open` 只分配 fd，不做设备特定初始化

内核设备实例：

```c
// KMS — 现有功能
static struct dev_ops kms_ops = {
    .driver_pid  = 0,
    .device_type = DEV_KMS,
    .ioctl       = display_ioctl,   // 替代 display_req_handler
    .mmap        = display_mmap,    // 已有 display_mmap_handler
};

// Serial — 需要特殊初始化
static struct dev_ops serial_ops = {
    .driver_pid  = 0,
    .device_type = DEV_SERIAL,
    .open        = serial_open,     // 注册 COM1 IRQ + 追踪 fd 数量
    .close       = serial_close,    // 注销 IRQ
    .read        = serial_read,     // COM1 读取
    .write       = serial_write,    // COM1 写入
    .poll        = serial_poll,     // 数据可读事件
};
```

用户态驱动 `dev_ops`：回调指针均为 NULL（驱动在用户态，内核不调它的函数指针）。`sys_dev_create` 只从用户空间拷贝 `driver_pid` + `device_type`，不拷贝函数指针。

### 2. 内核分发：dev_ops 回调 vs IPC proxy

```c
// sys_ioctl dispatch:
struct dev_ops *ops = inode->i_priv;
if (ops->driver_pid == 0) {
    if (ops->ioctl) return ops->ioctl(cmd, arg);  // 内核回调
    return -ENOTTY;
} else {
    // IPC proxy: cmd + arg 打包到 req_buf → sys_req → driver recv/resp
    ...
}
```

**所有硬编码 `if (device_type == DEV_KMS)` 消除**，trap.c 不再需要知道任何具体设备。

### 3. fd 类型精简

FD_SERIAL 合入 FD_DEV，FD_FILE 改名 FD_REGULAR：

```c
enum { FD_NONE, FD_PIPE, FD_REGULAR, FD_DEV, FD_DIR, FD_SOCKET };
```

6 种类型，每种有明确 VFS 分发语义：

| type | sys_read | sys_write | sys_ioctl | sys_mmap |
|------|----------|-----------|-----------|----------|
| FD_REGULAR | FAT32 page_cache | FAT32 write | -ENOTTY | -ENODEV |
| FD_DEV | ops->read / IPC | ops->write / IPC | ops->ioctl / IPC | ops->mmap / SHM |
| FD_PIPE | pipe_read | pipe_write | -ENOTTY | -ENODEV |
| FD_DIR | getdents | -EISDIR | -ENOTTY | -ENODEV |
| FD_SOCKET | sock_recvmsg | sock_sendmsg | sock_ioctl | -ENODEV |

### 4. libc fd_table 消除

内核 `proc->fd_table[fd]` 是 fd 元数据唯一权威来源。libc 不维护冗余拷贝。

影响：
- `open()` 统一走 `sys_open()`，不区分 /dev/ vs 普通文件
- `read/write/close/lseek` 直接走 syscall，不做 libc 层 fd_type 检查
- `fcntl()` 全走 `sys_fcntl(fd, cmd, arg)`（补 F_GETFL）
- `fstat()` → 新增 `sys_fstat` syscall
- `isatty()` → `ioctl(fd, TCGETS)`
- `req_fd(fd, req, resp)` → `ioctl(fd, cmd, arg)`
- `notify_fd(fd)` → `ioctl(fd, NOTIFY_CMD)`
- `msg_fd(fd, ...)` → 搬家到 `<driver/ipc.h>`（driver-only）
- `__fd_dev_target_pid()` → 消除（内核自己查 fd→pid）

### 5. ioctl 命令编码

采用 Linux `_IOC` 宏编码，cmd 自带方向、类型、序号、大小：

```c
#define _IOC(dir, type, nr, size) \
    (((dir)<<30) | ((type)<<8) | ((nr)<<0) | ((size)<<16))

#define _IO(type, nr)      _IOC(0, type, nr, 0)     // no arg
#define _IOW(type, nr, sz) _IOC(1, type, nr, sizeof(sz)) // write only
#define _IOR(type, nr, sz) _IOC(2, type, nr, sizeof(sz)) // read only
#define _IOWR(type, nr, sz) _IOC(3, type, nr, sizeof(sz)) // read+write

// KMS example:
#define KMS_IOCTL_CREATE_BUF  _IOWR('K', 1, struct display_create_buf_req)
#define KMS_IOCTL_FLIP        _IO('K', 2)
```

内核从 cmd 位域取 `_IOC_DIR(cmd)` 和 `_IOC_SIZE(cmd)`，决定 copy_from_user/copy_to_user 的方向和大小。不需要查表。

### 6. ioctl IPC proxy（用户态驱动）

当前阶段：arg ≤ 48B，走 56B req 快路径。

```c
// sys_ioctl IPC proxy:
uint8_t req_buf[56];
*(uint32_t *)req_buf = cmd;                    // cmd 替代 req_type
if (_IOC_DIR(cmd) & _IOC_WRITE)
    copy_from_user(req_buf + 4, arg, _IOC_SIZE(cmd));

return sys_req(target_pid, req_buf, resp_buf);
```

驱动 recv 拿到 RECV_REQ，req_type = ioctl cmd，payload = arg data。

resp 方向：
```c
// _IOC_READ: driver resp data → copy_to_user(arg)
```

未来需要大 arg (>48B) 时，加 msg 分支：`sys_msg(target_pid, req_buf, 4+_IOC_SIZE(cmd), resp_buf, resp_len)`。

### 7. open("/dev/") 统一路径

消除 `sys_open_dev`（syscall 32）。所有设备 open 走 `sys_open("/dev/xxx")` → `devtmpfs_open("xxx")`。

devtmpfs_open 流程：
```c
uint64_t devtmpfs_open(const char *name, int flags) {
    struct inode *ip = devtmpfs_lookup(name);
    // 分配 fd, 设 FD_DEV, inode_get(ip) ...
    struct dev_ops *ops = ip->i_priv;
    if (ops->driver_pid == 0 && ops->open)
        ops->open(proc, fd);   // serial: 注册 IRQ
    return fd;
}
```

新增 /dev/serial devtmpfs 节点（kernel boot 时 `serial_dev_register()` 创建）。

### 8. sys_dev_create 简化

消除 `sys_load_dev`（syscall 18）。驱动注册只走 `sys_dev_create(name, dev_type)`，内核自动填 `driver_pid = current_proc->pid`。

```c
// sys_dev_create: 不再从用户空间拷贝 dev_ops 函数指针
uint64_t sys_dev_create(uint64_t arg1_name, uint64_t arg2_dev_type, ...) {
    struct dev_ops kops = {
        .driver_pid = current_proc->pid,   // 内核自动填
        .device_type = dev_type,
        // 回调指针全为 NULL（用户态驱动）
    };
    return devtmpfs_create(name, dev_type, &kops);
}
```

libc `device_register(pid, dev_type)` → `sys_dev_create("kbd", DEV_KBD)`（不再需要传 pid）。

### 9. dev_table 消除

消除 `dev_table[DEV_TYPE_MAX]`、`register_dev()`、`sys_load_dev()`、`dev_table_cleanup()`、`lookup_dev()`。

ISR 无锁查 driver_pid：devtmpfs_create 时同步填 `isr_driver_pid[dev_type]`，devtmpfs_cleanup_pid 时清零。

```c
// kernel/devtmpfs.c
static pid_t isr_driver_pid[DEV_TYPE_MAX];

// xHCI ISR:
wake_process(isr_driver_pid[DEV_KBD]);  // 只唤醒，不入队 RECV_NOTIFY
```

### 10. ISR 唤醒机制

ISR 只 `wake_process()`，不操作 recv 队列。`recv` 被唤醒但队列为空时返回 `-EINTR`。

```c
// recv 改造:
// 被 wake_process 唤醒但 recv_head == recv_tail → return -EINTR

// 驱动逻辑:
while (1) {
    int rc = recv(&msg, NULL, 0, -1);
    if (rc == -EINTR) {
        process_kbd_events();   // 硬件事件 → 读 SHM ring buffer
        continue;
    }
    handle_ioctl(&msg);         // 客户端 ioctl → resp
    resp(&reply);
}
```

EINTR 是 POSIX 标准行为（和 Linux recv 遇到信号返回 EINTR 一致）。

### 11. recv/resp 降级到 driver-only

`recv/resp/req/msg/msg_resp` 不从公共 libc 头 `<sys/ipc.h>` 导出。搬到 `<driver/ipc.h>`（驱动内部接口）。

公共 libc 头只剩 POSIX 标准接口：
- `<fcntl.h>`: open, fcntl
- `<sys/stat.h>`: stat, fstat
- `<sys/ioctl.h>`: ioctl
- `<unistd.h>`: read, write, close, lseek, isatty
- `<sys/poll.h>`: poll

### 12. 新增 syscall

| syscall | 编号 | 功能 |
|---------|------|------|
| sys_ioctl | (待分配) | ioctl(fd, cmd, arg) |
| sys_fstat | (待分配) | fstat(fd, &buf) |
| sys_fcntl F_GETFL | 26 (现有) | 补 F_GETFL 路径 |

### 13. 废弃 syscall

| syscall | 编号 | 替代 |
|---------|------|------|
| sys_open_dev | 32 | sys_open → devtmpfs |
| sys_load_dev | 18 | sys_dev_create |
| sys_dev_req | 57 | sys_ioctl |
| sys_install_fd | 33 | (文件系统 proxy，待定) |

## 和现有文档的关系

- `dev_table.md` → 旧方案（本文替代其核心设计）
- `vfs.md` → VFS 层设计（本文的 open/ioctl/mmap dispatch 补充其中）
- `rpc.md` → req/resp IPC 协议（本文的 ioctl IPC proxy 基于 req 路径）
- `libc.md` → libc 设计（本文消除 fd_table，公共 API 缩减到 POSIX）
- `user_driver.md` → 用户态驱动流程（本文简化为 sys_dev_create + irq_bind）
