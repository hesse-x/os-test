# VFS 设计：微内核统一 I/O 架构

> 内核扩展 fd_table（新增 `FD_FILE` 类型），为 pipe、设备、文件提供统一的 `sys_read/sys_write/sys_close` 接口。文件系统实现（FAT32）保持在用户态 fs_driver 中，内核仅做 fd 元信息管理和 IPC 代理转发。pipe 保持内核直通路径不变。

## 1. 动机

当前架构中用户态 I/O 有三个互不统一的分支，fd 解析权分裂于内核和 libc：

| I/O 类型 | 内核 fd_table | libc fd_table | 数据路径 |
|----------|:------------:|:------------:|---------|
| Pipe (terminal↔shell) | FD_PIPE | FD_PIPE | `sys_read/sys_write` 直操内核 ring buffer |
| 文件 (FAT32) | **FD_NONE（内核不知）** | FD_FILE | libc `sys_msg` → fs_driver → `sys_block_read` |
| 设备 (/dev/kbd) | FD_DEV | FD_DEV | libc `sys_dev_msg` → target driver |

关键问题：

1. **fd 解析分叉**：内核 fd_table 不知文件 fd 的存在，`sys_read/close/dup2` 对文件 fd 返回 `EBADF` 或无法处理
2. **libc 分派开销**：`read(fd)` 需要 libc 检查 fd 类型再走不同路径，违背"一切皆文件"原则
3. **进程生命周期断层**：`sys_spawn` 继承 fd、`proc_reap` 清理 fd 均无法覆盖文件 fd
4. **POSIX 原语缺失**：`dup2` 对文件 fd 无效，`lseek` 不存在

## 2. 设计原则

- **微内核第一**：文件系统实现（FAT32 簇链遍历、目录操作）仍在用户态 fs_driver，内核不做文件内容缓存和路径解析
- **一切皆文件**：所有 fd 类型在内核 fd_table 中有统一表示，`sys_read(fd)` 在内核内部做 dispatch
- **最小改动原则**：pipe 保持内核直通路径不变；FD_DEV 保持现有 `sys_open_dev` 路径不变
- **fd 解析权归内核**：fd 号与资源映射由内核统一管理，libc 仅在 `open()` 时参与建连

## 3. 整体架构

```
用户进程
  │
  ├─ read(fd, buf, n) = sys_read(fd, buf, n)         ← 统一入口
  ├─ write(fd, buf, n) = sys_write(fd, buf, n)       ← 统一入口
  ├─ close(fd) = sys_close(fd)                        ← 统一入口
  ├─ open("/path") → libc 处理方法:
  │   ├─ /dev/xxx → sys_open_dev(dev_type) → FD_DEV
  │   └─ /xxx     → msg → fs_driver → sys_install_fd → FD_FILE
  └─ dup2(old, new) = sys_dup2(old, new)             ← 统一入口
       │
       ▼
  内核 syscall dispatch
       │
       ├─ FD_PIPE:  直读 kernel ring buffer     (保持)
       ├─ FD_DEV:   sys_msg → target driver     (保持)
       └─ FD_FILE:  kernel_msg_send → fs_driver (新增)
                     等待回复 → copy 到用户 buf
                     更新 file.offset
```

## 4. 内核数据结构变更

### 4.1 `struct file` 扩展 FD_FILE 类型

```c
// kernel/proc.h

#define FD_NONE   0
#define FD_PIPE   1
#define FD_SHM    2
#define FD_DEV    3
#define FD_FILE   4   // 新增

struct file {
    int type;            // FD_NONE / FD_PIPE / FD_SHM / FD_DEV / FD_FILE
    int flags;           // O_RDONLY / O_WRONLY / O_RDWR / O_NONBLOCK
    union {
        struct pipe *pipe;    // if FD_PIPE
        struct shm  *shm;     // if FD_SHM
        pid_t       target_pid;    // if FD_DEV (driver PID)
        struct {                    // if FD_FILE — 新增
            pid_t   fs_pid;         // fs_driver 进程 PID
            int32_t fs_fd;          // fs_driver 侧会话 fd
            uint64_t offset;        // 当前文件偏移（内核维护）
            uint64_t file_size;     // 文件大小（open 时获取）
            int      ref_count;     // 引用计数（dup2 共享）
        } file;
    };
};
```

### 4.2 `struct proc_t` 无需变更

`fd_table[MAX_FD]` 已是 `struct file` 数组，新增 `FD_FILE` 类型只需扩展 union，不涉及 PCB 结构调整。

## 5. Syscall 协议

### 5.1 新增 syscall：`sys_install_fd` (#36)

从用户态向进程 fd_table 注册一个 FD_FILE 条目。

| 参数 | 含义 |
|------|------|
| arg1 = fs_pid | fs_driver 进程 PID |
| arg2 = fs_fd | fs_driver 侧的会话 fd |
| arg3 = offset | 初始文件偏移（通常 0） |
| arg4 = flags | O_RDONLY / O_WRONLY / O_RDWR |
| arg5 = file_size | 文件大小（来自 fs_driver open 回复） |

**返回值**：分配的 fd 号（≥3），负数为 errno

**行为**：
1. 在当前进程 fd_table 中查找空闲 fd slot（从 3 开始）
2. 填充 type=FD_FILE、file.fs_pid、file.fs_fd、file.offset、file.file_size、file.ref_count=1
3. 若返回值后调用方不继续使用（open 路径等），需自行 close

**典型调用序列（open 路径）：**

```
open("/path"):
  ① libc sys_msg → fs_driver:  FILE_CMD_OPEN(path, flags)
  ② fs_driver → sys_msg_resp:  {status=0, fs_fd, file_size}
  ③ libc sys_install_fd(fs_pid, fs_fd, 0, flags, file_size)
  ④ 返回 fd
  // 之后 read/write/close 全部走统一 syscall
```

### 5.2 现有 syscall 扩展：`sys_read` (#17) FD_FILE 分支

```c
sys_read(fd, buf, len):
    if fd_table[fd].type == FD_FILE:
        file = &fd_table[fd].file
        // 构造 READ 请求
        req = {cmd=READ, fs_fd=file->fs_fd,
               offset=file->offset, count=len}
        // 内核内部向 fs_driver 发 msg，阻塞等待回复
        // (复用 sys_msg 机制的内部入口)
        n = kernel_msg_send(file->fs_pid, &req, sizeof(req),
                            reply_buf, sizeof(reply_buf))
        if n < 0: return n
        // 从回复包提取数据，copy 到用户 buf
        copy_to_user(buf, reply_buf.data, reply_buf.count)
        // 更新内核维护的 offset
        file->offset += reply_buf.count
        return reply_buf.count
    // FD_PIPE: 保持现有路径不变
    // FD_DEV:  保持现有路径不变 (或ENOSYS)
```

### 5.3 现有 syscall 扩展：`sys_write` (#16) FD_FILE 分支

```
sys_write(fd, buf, len):
    if fd_table[fd].type == FD_FILE:
        file = &fd_table[fd].file
        // 构造 WRITE 请求
        req = {cmd=WRITE, fs_fd=file->fs_fd,
               offset=file->offset, data=buf[0..len]}
        // 内核向 fs_driver 发 msg，阻塞等待
        n = kernel_msg_send(file->fs_pid, &req, sizeof(req)+len,
                            reply, sizeof(reply))
        if n < 0: return n
        file->offset += reply.count
        return reply.count
    // FD_PIPE: 保持现有路径不变
```

### 5.4 现有 syscall 扩展：`sys_close` (#18) FD_FILE 分支

```
sys_close(fd):
    if fd_table[fd].type == FD_FILE:
        file = &fd_table[fd].file
        file->ref_count--
        if file->ref_count == 0:
            // 同步通知 fs_driver 释放会话 fd
            req = {cmd=CLOSE, fs_fd=file->fs_fd}
            kernel_msg_send(file->fs_pid, &req, sizeof(req),
                            NULL, 0)
            // 回复后 fs_driver 端已清理
        fd_table[fd].type = FD_NONE
        return 0
    // FD_PIPE: 保持现有路径不变 (ref_count + kfree)
    // FD_DEV:  保持现有路径不变
    // FD_SHM:  保持现有路径不变
```

### 5.5 现有 syscall 扩展：`sys_dup2` (#27) FD_FILE 分支

```
sys_dup2(old_fd, new_fd):
    if fd_table[old_fd].type == FD_FILE:
        if fd_table[new_fd].type != FD_NONE:
            // 关闭现有 new_fd（同 sys_close 逻辑）
        // 拷贝 fd_table 条目
        fd_table[new_fd] = fd_table[old_fd]
        fd_table[new_fd].file.ref_count++
        return new_fd
    // FD_PIPE/D_DEV: 保持现有路径不变
```

POSIX 语义：dup2 后两个 fd 共享同一 `fs_fd`（同一个文件会话），offset 也共享（因为内核 `file.file.offset` 是同一个）。

### 5.6 内部接口：`kernel_msg_send()`

```c
// kernel/trap.cc — 新增内部接口
// 用途：内核自身向用户态进程发送 sync msg 并等待回复
// 参数：target_pid, req, req_len, resp, resp_len
// 返回值：正数 = 回复数据长度，负数 = errno
int kernel_msg_send(pid_t target_pid, const void *req,
                    size_t req_len, void *resp, size_t resp_len);
```

复用 `sys_msg` 的核心路由逻辑（`sys_msg_to`），绕过用户态的 page boundary check 和 copy_from_user，直接在内核空间构造 msg。调用者进程阻塞于 `WAIT_MSG_REPLY`，目标进程回复后唤醒。

### 5.7 `proc_reap` FD_FILE 清理

```c
// kernel/proc.cc — proc_reap 新增分支
if (proc->fd_table[fd].type == FD_FILE) {
    file = &proc->fd_table[fd].file;
    file->ref_count--;
    if (file->ref_count == 0) {
        // 同步通知 fs_driver 释放
        req = {cmd=CLOSE, fs_fd=file->fs_fd};
        kernel_msg_send(file->fs_pid, &req, sizeof(req), NULL, 0);
    }
    proc->fd_table[fd].type = FD_NONE;
}
```

## 6. libc 变更

### 6.1 移除 libc fd_table 中的文件 I/O 逻辑

当前 libc `read()` 在 `FD_PIPE` / `FD_DEV` / `FD_FILE` 间分派。引入 FD_FILE 后，`read/write/close` 全部走统一 syscall，libc 中不再需要 `file_read()` / `file_write()` / `file_close()`：

```c
// user/lib/file.cc (改造后)

ssize_t read(int fd, void *buf, size_t count) {
    fd_table_init();
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }
    // 所有类型统一走 sys_read，内核 dispatch
    int64_t n = sys_read(fd, buf, count);
    if (n < 0) { errno = (int)(-n); return -1; }
    return (ssize_t)n;
}

ssize_t write(int fd, const void *buf, size_t count) {
    fd_table_init();
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }
    int64_t n = sys_write(fd, buf, count);
    if (n < 0) { errno = (int)(-n); return -1; }
    return (ssize_t)n;
}

int close(int fd) {
    fd_table_init();
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }
    int64_t n = sys_close(fd);
    if (n < 0) { errno = (int)(-n); return -1; }
    fd_table[fd].type = FD_NONE;  // libc 侧同步清理
    return 0;
}
```

### 6.2 改造 `open()` 路径

```c
// user/lib/file.cc — open() 改造后

int open(const char *path, int flags, ...) {
    fd_table_init();

    // /dev/xxx 路径不变
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e'
        && path[3] == 'v' && path[4] == '/') {
        // ... 同现有逻辑：sys_open_dev → FD_DEV
    }

    // 普通文件：
    int fs_fd = get_fs_dev_fd();    // open("/dev/fs") 获取 fs_driver FD_DEV fd
    if (fs_fd < 0) { errno = ENOENT; return -1; }

    // ① 向 fs_driver 发送 OPEN 请求
    struct file_req req = { .cmd = FILE_CMD_OPEN, .flags = flags };
    strncpy(req.path, path, 255);
    uint8_t reply_buf[8192];
    int r = msg_fd(fs_fd, &req, sizeof(req), reply_buf, sizeof(reply_buf));
    if (r < 0) { errno = -r; return -1; }

    struct file_resp *resp = (struct file_resp *)reply_buf;
    if (resp->status < 0) { errno = -resp->status; return -1; }

    // ② 向内核注册 FD_FILE fd
    int fd = sys_install_fd(get_fs_pid(), resp->fd, 0, flags, resp->file_size);
    if (fd < 0) {
        // 注册失败，通知 fs_driver 关闭
        struct file_req close_req = { .cmd = FILE_CMD_CLOSE, .fs_fd = resp->fd };
        msg_fd(fs_fd, &close_req, sizeof(close_req), NULL, 0);
        errno = -fd;
        return -1;
    }

    // ③ libc fd_table 同步记录
    fd_table[fd].type = FD_FILE;
    fd_table[fd].flags = flags;
    return fd;
}
```

### 6.3 移除用户态文件 I/O 辅助函数

- `file_read()` / `file_write()` / `file_close()` — 不再需要
- `msg_fd(fs_fd, ...)` — fs_driver IPC 只在 `open()` 中使用
- libc fd_table 的 `FD_FILE` 类型仍保留但仅用于 `open()` 和 `close()` 后同步清理

### 6.4 工作目录管理

cwd（当前工作目录）由 libc 全局变量维护，`chdir()` 纯 libc 实现：

```c
// user/lib/file.cc
static char cwd_path[256] = "/";   // 每个进程独立

int chdir(const char *path) {
    // 验证路径存在性（msg → fs_driver → stat）
    // 成功则更新 cwd_path
    // 失败返回 -1
}

// open() 中处理相对路径
int open(const char *path, int flags, ...) {
    char abs_path[256];
    if (path[0] != '/') {
        // 相对路径：拼接 cwd + path
        snprintf(abs_path, sizeof(abs_path), "%s/%s", cwd_path, path);
        path = abs_path;
    }
    // ... 同上
}
```

`sys_spawn` 继承 cwd 由 libc 侧处理（spawn 前序列化 cwd 到目标 ELF 参数区，_start 中恢复）。

## 7. fs_driver 变更

### 7.1 新增请求者身份识别

内核代理 IPC 时，`kernel_msg_send` 发出的请求需要让 fs_driver 知道**哪个内核 fd 会话**在操作。建议方案：

fs_driver 收到 READ/WRITE/CLOSE 请求时，通过 `sys_msg` 的 `caller_pid`（即内核的特殊 PID，如 `KERNEL_PID=0`）来识别是来自内核代理而非用户态直接调用。也可以在请求结构中增加 `caller_pid` 字段：

```c
// driver/fs_driver.cc — 请求结构扩展
struct fs_msg_header {
    pid_t  caller_pid;    // 谁发的请求（对 FD_FILE 场景为内核代理 PID？或用户进程 PID？）
    int32_t fs_fd;        // fs_driver 侧会话 fd
    uint32_t cmd;         // FILE_CMD_READ / FILE_CMD_WRITE / FILE_CMD_CLOSE
    uint64_t offset;      // 读写偏移
    uint32_t count;       // 读写长度
    uint8_t  data[];      // 变长数据（WRITE 时带数据，READ 时回复带数据）
};
```

**设计选择**：`caller_pid` 应该传递用户进程 PID 还是内核 PID？

推荐传递 **用户进程 PID**。理由：fs_driver 的权限检查、会话隔离、日志都以 PID 为 key。内核代理 fd_table 时，`caller_pid` 记录的是发起 syscall 的进程 PID——内核只是中转，不改变身份。

### 7.2 fs_driver 的 READ/WRITE 处理

```c
// driver/fs_driver.cc — READ 处理
case FILE_CMD_READ: {
    pid_t caller_pid = req->caller_pid;
    int32_t session_fd = req->fs_fd;
    uint64_t offset = req->offset;
    uint32_t count = req->count;
    session_t *s = find_session_by_pid(caller_pid);
    file_t *f = find_open_file(s, session_fd);
    // 读 FAT32 簇链（现有逻辑不变）
    count = min(count, f->file_size - offset);
    uint64_t lba = fat_cluster_to_lba(f->start_cluster, offset);
    sys_block_read(lba, count / 512 + 1, data_buf);
    // 回复
    resp->status = 0;
    resp->count = count;
    memcpy(resp->data, data_buf, count);
    break;
}
```

**注意**：fs_driver 仍通过 `sys_block_read`（syscall #32）访问磁盘，数据流方向不变：

```
sys_read(fd) → 内核 FD_FILE dispatch → kernel_msg_send → fs_driver
  → fs_driver 解析 FAT32 → sys_block_read → AHCI → 磁盘
  → fs_driver 回复数据 → kernel 收到 → copy 到用户 buf → 返回
```

## 8. 启动流程与 fd 继承

### 8.1 `kernel_main` 创建 terminal/shell 的 pipe 拓扑

**不变**。`kernel_main` 仍创建两对 pipe 连接 terminal 和 shell：

```
pipe_stdin: terminal fd[1](WO) → shell fd[0](RO)
pipe_stdout: shell fd[1](WO) → terminal fd[0](RO)
```

`sys_spawn` 继承 fd 0/1 的逻辑不变（FD_PIPE 类型，pipe->ref_count++）。

### 8.2 Terminal 打开显示设备

terminal 通过 `open("/dev/kms")` 获取 FD_DEV fd（不变）。fs_driver 不参与 /dev 路径的解析。

### 8.3 shell 执行 ELF

`sys_spawn` 继承 fd 0/1（pipe）。ELF 路径解析（查找 FAT32 文件）走 fs_driver 是通过 `sys_msg` 直接通信（不经过 FD_FILE 路径），因为在 spawn 时还没有对应的 fd——这是 `open()` 路径之外 fs_driver 的直接 IPC 场景。

### 8.4 用户程序继承文件 fd

`sys_spawn` 扩展：在 fd 0/1 继承之外，增加对所有非 FD_NONE fd 的继承判断。对于 `FD_FILE`：

```c
// sys_spawn 中
for (int fd = 0; fd < MAX_FD; fd++) {
    if (current_proc->fd_table[fd].type != FD_NONE) {
        // FD_PIPE: 保持现有 ref_count++
        if (current_proc->fd_table[fd].type == FD_FILE) {
            // 继承 FD_FILE: 拷贝 + ref_count++
            child->fd_table[fd] = current_proc->fd_table[fd];
            child->fd_table[fd].file.ref_count++;
        }
    }
}
```

## 9. Pipe 保持现状

本次重构**不移出 pipe**，原因：

| 维度 | 说明 |
|------|------|
| 性能 | `printf → pipe → terminal` 是最频繁的 I/O 路径，保持内核直通 |
| 架构 | pipe 是匿名通信通道，不需文件命名空间管理 |
| 统一性 | `sys_read/sys_write` 对 FD_PIPE 和 FD_FILE 共用入口，libc 无分叉 |
| 与 Linux 对比 | Linux pipe 也位于内核，通过 VFS `file_operations` 路由 |

后续若需将 pipe 移出到用户态 pipe_driver，只需：
1. 新增 FD_PIPE_VIRTUAL 类型（或统一用 FD_DEV）
2. pipe_driver 注册为 DEV_PIPE
3. `sys_pipe` 改为向 pipe_driver 发请求
4. 内核 fd_table 中 FD_PIPE 路径改为 `kernel_msg_send → pipe_driver`

## 10. Socket 未来接入

socket 遵循与 FD_FILE 相同的接入模式：

```
sys_socket(domain, type, protocol):
  → 内核 alloc FD_SOCK fd
  → struct file.sock = {net_pid, sock_fd, ref_count}

sys_read(sock_fd, buf, n):
  → FD_SOCK → kernel_msg_send(net_pid, READ_REQ, ...)
  → 等待回复 → copy 到用户 buf → 返回

sys_close(sock_fd):
  → FD_SOCK → ref_count-- → 0 → kernel_msg_send(net_pid, CLOSE_REQ)
```

`open("/dev/tcp/...")` 路径也经 sys_install_fd 注册 FD_SOCK fd。

## 11. Syscall 表变更

| # | 名称 | 功能 | 状态 |
|---|------|------|:----:|
| 15 | sys_pipe | 创建 pipe | 不变 |
| 16 | sys_write | 写 fd（PIPE/FILE/DEV dispatch） | **扩展 FD_FILE** |
| 17 | sys_read | 读 fd（PIPE/FILE/DEV dispatch） | **扩展 FD_FILE** |
| 18 | sys_close | 关闭 fd | **扩展 FD_FILE** |
| 27 | sys_dup2 | 复制 fd | **扩展 FD_FILE** |
| 28 | sys_fcntl | fd 控制 | 不变 |
| **36** | **sys_install_fd** | **注册 FD_FILE fd** | **新增** |

## 12. 实现计划

### Phase 1：内核基础设施

1. `kernel/proc.h` — `struct file` 新增 `FD_FILE` 类型 + `struct file.file` 子结构
2. `kernel/trap.cc` — 新增 `kernel_msg_send()` 内部接口
3. `kernel/trap.cc` — 新增 `sys_install_fd()` syscall (#36)
4. `kernel/trap.cc` — `sys_read/sys_write/sys_close/sys_dup2` 各增 FD_FILE 分支
5. `kernel/proc.cc` — `proc_reap` 增 FD_FILE 清理 + dev_table 中 fs_driver 自清理
6. `common/syscall.h` — 新增 `SYS_INSTALL_FD` #36 和 wrapper

### Phase 2：libc 改造

1. `user/lib/file.cc` — `read/write/close` 移除类型分派，统一走 syscall
2. `user/lib/file.cc` — `open()` 新增 msg_fd → sys_install_fd 路径
3. `user/lib/file.cc` — 移除 `file_read/file_write/file_close`
4. `user/lib/file.cc` — 新增 `chdir()` + cwd 全局变量

### Phase 3：fs_driver 适配

1. `driver/fs_driver.cc` — 确保 READ/WRITE/CLOSE 命令处理的 caller_pid 正确传递
2. `driver/fs_driver.cc` — 回复中包含 count/file_size 等 kernel 需要的字段

### Phase 4：测试

1. `hello.c` — 验证 `open("/hello.elf")` → FD_FILE → `read/close` 路径
2. Terminal ↔ shell pipe 路径回归
3. `dup2(FILE_fd, ...)` 场景验证
4. 进程退出时 FD_FILE 清理验证

## 13. 与 Linux VFS 的关系

| 特性 | Linux | 本设计 |
|------|-------|--------|
| fd_table 位置 | 内核（`struct files_struct`） | 内核（`proc_t::fd_table`） |
| 文件系统实现 | 内核 ext4/btrfs 等 | 用户态 fs_driver |
| 数据流 | `vfs_read → ext4_read → block layer` | `sys_read → kernel_msg_send → fs_driver → sys_block_read` |
| page cache | 内核（page cache） | 无 |
| `file_operations` | 内核函数指针表 | fd_table type dispatch + kernel_msg_send |
| open 路径分割 | 内核统一 | libc 发 msg 到 fs_driver → sys_install_fd |
| pipe | 内核 `pipe_read/write` in VFS | 内核直通 pipe ring buffer |

## 14. 局限

- **数据拷贝开销**：当前 `sys_msg` 使用内核 kmalloc 中转（≤64KB），FD_FILE 的 read/write 额外多一次内核 copy（fs_driver → kernel msg buf → 用户 buf）。后续可优化为 SHM 直通（sys_mmap 共享数据页）
- **无 page cache**：频繁读写的文件每次 IPC 到 fs_driver，不支持内核缓存，对高频小 I/O 场景性能较弱
- **无 `lseek`**：offset 在内核维护，暂未暴露 `lseek` syscall。后续可通过新增 `sys_lseek(fd, offset, whence)` + FD_FILE 分支支持
- **无 `select/poll` 的 FD_FILE 支持**：后续需扩展 `sys_poll` 以支持查询文件 fd 的可读状态
- **单 fs_driver 实例**：当前 dev_table 每类型单实例，不支持多文件系统同时挂载（如 tmpfs + FAT32 并存）

## 15. 设计文档参考

| 文档 | 内容 |
|------|------|
| `file_system.md` | FAT32 文件系统驱动架构 |
| `fat32.md` | FAT32 磁盘布局 |
| `dev_table.md` | 设备注册表（`/dev/xxx` → PID） |
| `rpc.md` | REQ/RESP 同步 IPC 协议 |
| `sys_api.md` | 系统调用 API 参考 |
| `libc.md` | 用户态 libc 设计 |
