# 统一 IPC 机制：sys_recv / sys_req / sys_resp / sys_msg / sys_msg_resp

## 概述

双层同步请求-响应 IPC 机制，所有事件（IRQ/REQ/NOTIFY/MSG）统一通过 per-process recv 队列接收，`sys_recv` 一次调用可多路复用。

| 机制 | 载荷 | 传输方式 | 用途 |
|------|------|---------|------|
| sys_req / sys_resp | ≤56B | 内联，零分配 | 控制信令（bind/unbind） |
| sys_msg / sys_msg_resp | 变长（≤64KB） | 内核 kmalloc 中转拷贝 | 数据传输（文件 I/O） |

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | IPC 模型 | 同步请求-响应 + 统一 recv | 微内核标准模式（seL4/QNX），一次 wait 收 IRQ+REQ+NOTIFY+MSG |
| 2 | sys_wait 处置 | 删除 | sys_recv 完全覆盖其功能 |
| 3 | 内联消息大小 | 64 字节 | 一个缓存行，内核 memcpy 开销可忽略 |
| 4 | 消息格式 | 内核透明（raw buffer） | 微内核原则，内核只搬运不解析 |
| 5 | recv_msg 布局 | `struct recv_msg { type, src, data[56] }` | 结构化头部 + payload；RECV_MSG 用 union 存 kmaddr/len |
| 6 | sys_req 超时 | 不加 | 目标崩溃时 proc_reap 唤醒调用者 |
| 7 | sys_resp 目标 | 内核自动关联 | 严格请求/响应配对，req_caller_pid 自动记录 |
| 8 | recv 队列 | per-process 固定 16 槽环形缓冲区 | 无动态分配，1KB/process，FIFO |
| 9 | sys_notify 语义 | 消息入队替代直接唤醒 | notify 消息进 recv 队列，recv 端统一消费 |
| 10 | 变长消息 | 内核 kmalloc 中转拷贝 | 数据必须跨地址空间，内核拷贝保证隔离；≤64KB 上限防止滥用 |
| 11 | recv 对 RECV_MSG | 扩展签名加 data_buf 参数 | 64B recv_msg 放不下变长数据，双缓冲：msg 头 + data_buf 存变长数据 |

## syscall 接口

### sys_recv(msg, data_buf, data_buf_len, timeout_ms) — syscall #2

```c
int sys_recv(void *msg, void *data_buf, size_t data_buf_len, uint32_t timeout_ms);
```

- 从当前进程的 recv 队列取出一条消息，拷贝 64 字节到 msg（消息头）
- RECV_MSG 时：将变长数据 copy_to_user 到 data_buf（必须 `data_buf_len >= msg.len`，否则返回 -EINVAL），并 kfree 内核缓冲区
- 非 RECV_MSG 消息（IRQ/REQ/NOTIFY）忽略 data_buf 参数，现有调用者传 `NULL, 0`
- 队列为空：设 `WAIT_RECV` 阻塞，设 timeout（0=无限）
- 返回 0=成功，负 errno=-ETIMEDOUT/-EINVAL/-EFAULT

### sys_req(pid, request, reply) — syscall #3

```c
int sys_req(pid_t pid, void *request, void *reply);
```

- 向目标进程发送 56 字节请求（从 request 拷贝到 recv_msg.data），请求入目标 recv 队列（type=RECV_REQ, src=调用者 PID）
- 如果目标正在 `sys_recv` 等待（`WAIT_RECV`），唤醒它
- 调用者设 `WAIT_REQ_REPLY` 阻塞，等待目标 `sys_resp`
- 目标 `sys_resp` 时，56 字节回复拷贝到 reply，唤醒调用者
- 返回 0=成功，负 errno=-ESRCH/-EBUSY/-EFAULT
- 崩溃清理：目标进程 exit 时，`proc_reap` 扫描 `procs[]`，所有 `wait_event==WAIT_REQ_REPLY && req_target==目标PID` 的进程被唤醒，返回 -ESRCH

### sys_resp(reply) — syscall #4

```c
int sys_resp(void *reply);
```

- 将 56 字节回复从 reply 拷贝到当前请求调用者的 reply buffer（需 CR3 切换到调用者地址空间）
- 唤醒调用者（从 `WAIT_REQ_REPLY` → `READY`）
- 清除 `req_caller_pid`
- 返回 0=成功，负 errno=-EINVAL/-ESRCH/-EFAULT

### sys_msg(target_pid, msg_buf, msg_len, reply_buf, reply_len) — syscall #24

```c
int sys_msg(int32_t target_pid, void *msg_buf, size_t msg_len,
            void *reply_buf, size_t reply_len);
```

- 向目标进程发送变长消息（≤64KB），内核 kmalloc 中转拷贝
- 调用者设 `WAIT_MSG_REPLY` 阻塞，等待目标 `sys_msg_resp`
- `msg_len ∈ [1, 65536]`，kmalloc 失败返回 -ENOMEM
- 返回 0=成功，负 errno=-ESRCH/-ENOMEM/-EINVAL/-EFAULT

### sys_msg_resp(resp_buf, resp_len) — syscall #25

```c
int sys_msg_resp(void *resp_buf, size_t resp_len);
```

- 将变长回复拷贝到当前 msg 调用者的 reply buffer（需 CR3 切换到调用者地址空间）
- copy 长度 = `min(resp_len, msg_reply_len)`
- 唤醒调用者（从 `WAIT_MSG_REPLY` → `READY`）
- 清除 `msg_caller_pid`
- 返回 0=成功，负 errno=-EINVAL/-ESRCH/-EFAULT

### sys_notify(pid) — syscall #21

```c
int sys_notify(pid_t pid);
```

- 向目标进程的 recv 队列入一条消息（type=RECV_NOTIFY, src=调用者 PID, data=空）
- 如果目标正在 `sys_recv` 等待（`WAIT_RECV`），唤醒它
- 队列满时返回 -EBUSY

## recv_msg 结构

```c
#define RECV_IRQ    0   // IRQ 通知
#define RECV_REQ    1   // 内联请求（≤56B）
#define RECV_NOTIFY 2   // 异步通知
#define RECV_MSG    3   // 变长消息（≤64KB）

struct recv_msg {
    uint32_t type;       // RECV_IRQ / RECV_REQ / RECV_NOTIFY / RECV_MSG
    uint32_t src;        // IRQ 号 或 发送者 PID
    union {
        uint8_t data[56];       // RECV_IRQ / RECV_REQ / RECV_NOTIFY
        struct {                 // RECV_MSG only（内核内部，用户态不可见）
            void  *kmaddr;      // 内核 kmalloc 缓冲区地址
            size_t len;         // 数据长度
        } msg;
    };
};
```

## 内核数据结构

### proc_t IPC 相关字段

```c
// === recv 队列 ===
uint8_t  recv_buf[16][64];  // 16 槽 × 64 字节 = 1KB 固定缓冲区
uint32_t recv_head;         // 生产者写入位置
uint32_t recv_tail;         // 消费者读取位置
spinlock_t recv_lock;       // 保护 recv_buf/head/tail

// === req 状态（内联载荷，≤56B） ===
pid_t    req_caller_pid;    // 当前正在处理的请求调用者 PID（-1=无）
void    *req_reply_buf;     // 调用者 reply buffer 的用户态地址
int32_t  req_result;        // 请求返回结果（0=成功，负 errno=失败）
pid_t    req_target_pid;    // 请求目标 PID（用于崩溃清理）

// === msg 状态（变长载荷，≤64KB） ===
void    *msg_reply_buf;     // caller's reply buffer user-space address
size_t   msg_reply_len;     // caller's reply buffer size
pid_t    msg_caller_pid;    // server side: who sent the msg (-1 = none)
int32_t  msg_result;        // 0 = success, negative errno on error
pid_t    msg_target_pid;    // caller side: who we're waiting on (crash cleanup)
```

### wait_event_t

```c
enum wait_event_t {
    WAIT_NONE,       // 未等待
    WAIT_RECV,       // sys_recv 等待
    WAIT_REQ_REPLY,  // sys_req 等待回复
    WAIT_MSG_REPLY,  // sys_msg 等待回复
    WAIT_CHILD,      // sys_waitpid 等待子进程
    WAIT_PIPE,       // pipe 读写阻塞
};
```

## 关键实现路径

### sys_req 流程

```
调用者                              内核                              服务端
-------                              ------                              -------
1. sys_req(pid, request, reply)
                                      2. copy_from_user 请求到 recv_msg.data
                                      3. 入队 target->recv_buf (type=RECV_REQ)
                                      4. 唤醒 target (WAIT_RECV → READY)
                                      5. 阻塞调用者 (WAIT_REQ_REPLY)
                                                                          6. 服务端 recv() 返回 RECV_REQ
                                                                          7. 处理请求
                                                                          8. sys_resp(reply)
                                                                              9. CR3 切换 → copy_to_user
                                                                              10. 唤醒调用者 (WAIT_REQ_REPLY → READY)
11. 调用者恢复，返回 0 或负 errno
```

### sys_msg 流程

```
调用者                              内核                              服务端
-------                              ------                              -------
1. sys_msg(pid, msg_buf, msg_len,
           reply_buf, reply_len)
                                      2. 验证 target_pid/msg_len/指针
                                      3. kmalloc(msg_len)，失败→-ENOMEM
                                      4. copy_from_user(kbuf, msg_buf, msg_len)
                                      5. 入队 RECV_MSG(kmaddr=kbuf, len=msg_len)
                                      6. 唤醒 target (WAIT_RECV → READY)
                                      7. 阻塞调用者 (WAIT_MSG_REPLY)
                                                                          8. 服务端 recv() 返回 RECV_MSG
                                                                              内核: copy_to_user(data_buf, kmaddr, len)
                                                                              内核: kfree(kmaddr) ← 请求缓冲区释放
                                                                              内核: msg_caller_pid = src
                                                                          9. 服务端处理请求
                                                                          10. sys_msg_resp(resp_buf, resp_len)
                                                                              11. copy_from_user(kbuf_resp, resp_buf, resp_len)
                                                                              12. CR3 切换 → copy_to_user(msg_reply_buf, ...)
                                                                              13. kfree(kbuf_resp)
                                                                              14. 唤醒调用者 (WAIT_MSG_REPLY → READY)
15. 调用者恢复，返回 0 或负 errno
```

### IRQ 分发

IRQ 到达时向绑定进程的 recv 队列入 RECV_IRQ 消息，然后唤醒（如果进程在 WAIT_RECV）。详见 `kernel/trap.cc` trap_dispatch。

### sys_notify

向目标 recv 队列入 RECV_NOTIFY 消息 + 唤醒 WAIT_RECV 进程。

### wake_process（内核内部，pipe 用）

向 recv 队列入 RECV_NOTIFY 消息 + 唤醒 WAIT_PIPE 进程。pipe 阻塞使用 `WAIT_PIPE`，与 `WAIT_RECV` 隔离。

### proc_reap 崩溃清理

进程退出时清理所有 IPC 状态：

1. **req 调用者**：扫描所有进程，找到 `wait_event==WAIT_REQ_REPLY && req_target_pid==exiting_pid` 的进程，唤醒并设 `req_result=-ESRCH`
2. **msg 调用者**：扫描所有进程，找到 `wait_event==WAIT_MSG_REPLY && msg_target_pid==exiting_pid` 的进程，唤醒并设 `msg_result=-ESRCH`
3. **req 服务端**：`req_caller_pid >= 0` 时清除为 -1
4. **msg 服务端**：`msg_caller_pid >= 0` 时清除为 -1
5. **recv 队列中的 RECV_MSG**：遍历 recv_buf，对 type==RECV_MSG 的项 kfree(kmaddr)

## libc 封装

```c
// user/include/sys/ipc.h
int recv(struct recv_msg *msg, void *data_buf, size_t data_buf_len, uint32_t timeout_ms);
int req(int32_t pid, void *req_buf, void *resp_buf);
int resp(void *resp_buf);
int msg(int32_t pid, void *req_buf, size_t req_len, void *resp_buf, size_t resp_len);
int msg_resp(void *resp_buf, size_t resp_len);
```

高阶封装 `req_call()` 定义在 `user/include/sys.h`。

## libc 文件 I/O

基于 `sys_msg` 实现用户态文件 I/O，使进程可通过 `open()`/`read()`/`write()`/`close()` 访问 FAT32 文件。

### fd 命名空间

libc 统一管理所有 fd，内核只管 pipe fd。

```c
#define MAX_FD  32

enum fd_type_t { FD_NONE = 0, FD_PIPE, FD_FILE };

struct file_fd_entry {
    enum fd_type_t type;    // FD_NONE / FD_PIPE / FD_FILE
    int     flags;          // O_RDONLY / O_WRONLY / O_RDWR
    // FD_FILE 专用：
    int32_t fs_fd;          // fs_driver 返回的本地 fd
    uint64_t offset;        // 当前读写偏移（libc 维护）
};

static struct file_fd_entry fd_table[MAX_FD];
```

**fd 分配规则**：
- 0 = stdin（FD_PIPE），1 = stdout（FD_PIPE），2 = stderr（FD_PIPE）
- 3-31：由 libc 统一分配，pipe 和文件共享此空间
- `pipe()` 和 `open()` 调用时扫描 fd_table 找第一个 FD_NONE 槽位

**read/write/close 分发**：

```c
ssize_t read(int fd, void *buf, size_t count) {
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }
    if (fd_table[fd].type == FD_PIPE)
        return pipe_read(fd, buf, count);   // sys_read
    else
        return file_read(fd, buf, count);   // sys_msg → fs_driver
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }
    if (fd_table[fd].type == FD_PIPE)
        return pipe_write(fd, buf, count);  // sys_write
    else
        return file_write(fd, buf, count);  // sys_msg → fs_driver
}

int close(int fd) {
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }
    if (fd_table[fd].type == FD_PIPE) {
        int r = sys_close(fd);
        if (r < 0) { errno = -r; return -1; }
    } else {
        // sys_msg → fs_driver FILE_CMD_CLOSE
        file_close(fd);
    }
    fd_table[fd].type = FD_NONE;
    return 0;
}
```

### fs_driver 发现

libc lazy 初始化，缓存 fs_driver PID：

```c
static pid_t fs_pid = -1;

static pid_t get_fs_pid() {
    if (fs_pid < 0) {
        fs_pid = device_lookup(DEV_FS);
    }
    return fs_pid;
}
```

容错：如果 `sys_msg` 返回 ESRCH（fs_driver 已退出），清空 `fs_pid = -1`，调用者可重试。

### 消息协议

#### 请求格式

```c
// file_req 放在 sys_msg 的 msg_buf 中
#define FILE_CMD_OPEN      1
#define FILE_CMD_READ      2
#define FILE_CMD_WRITE     3   // stub，返回 ENOSYS
#define FILE_CMD_CLOSE     4
#define FILE_CMD_READDIR   5
#define FILE_CMD_CREATE    6
#define FILE_CMD_MKDIR     7
#define FILE_CMD_RAW_READ  8

struct file_req {
    uint32_t cmd;
    // OPEN/CREATE/MKDIR
    char     path[256];
    uint32_t flags;          // open: O_RDONLY/O_WRONLY/O_RDWR
    // READ/WRITE/CLOSE
    uint32_t fs_fd;          // fs_driver 本地 fd
    uint64_t offset;         // read offset（libc 维护）
    uint32_t count;          // read/write 字节数
    // RAW_READ
    uint32_t lba;
    // READDIR
    uint32_t readdir_offset; // 分页偏移
    uint32_t readdir_count;
};
```

#### 回复格式

```c
// file_resp 放在 sys_msg_resp 的 resp_buf 中
struct file_resp {
    int32_t  status;         // 0=成功, 负errno
    uint32_t fd;             // OPEN 返回 fs_driver 本地 fd
    uint64_t file_size;      // OPEN 返回文件大小
    uint32_t count;          // READ/READDIR/RAW_READ 实际字节数
    uint32_t total;          // READDIR 返回条目数
    uint8_t  data[];         // READ/READDIR/RAW_READ 变长数据
};
```

### fs_driver 多客户端 session

```c
#define MAX_CLIENTS  16

struct client_session {
    pid_t client_pid;
    struct open_file open_files[8];  // 每客户端 8 个 fd
};

static struct client_session sessions[MAX_CLIENTS];

static struct client_session *get_session(pid_t pid) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].client_pid == pid) return &sessions[i];
    }
    // 分配新 session
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].client_pid == -1) {
            sessions[i].client_pid = pid;
            memset(sessions[i].open_files, 0, sizeof(sessions[i].open_files));
            return &sessions[i];
        }
    }
    return NULL;  // ENOMEM
}
```

### pipe 拦截

libc 的 `pipe()` 标记 fd 类型：

```c
int pipe(int fd[2]) {
    int r = sys_pipe(fd);
    if (r < 0) { errno = -r; return -1; }
    fd_table[fd[0]].type = FD_PIPE;
    fd_table[fd[1]].type = FD_PIPE;
    return 0;
}
```

## syscall 编号

| # | 名称 | 说明 |
|---|------|------|
| 0 | sys_getpid | 返回当前 PID |
| 1 | sys_yield | 让出 CPU |
| 2 | sys_recv | 统一事件接收 |
| 3 | sys_req | 同步内联请求（≤56B） |
| 4 | sys_resp | 回复当前 req 调用者 |
| 5 | sys_irq_bind | 绑定 IRQ |
| 6 | sys_exit | 退出进程 |
| 7 | sys_waitpid | 等待子进程 |
| 8 | sys_spawn | 创建子进程 |
| 9 | sys_mmap | 匿名内存映射 |
| 10 | sys_munmap | 解除映射 |
| 11 | sys_serial_write | 串口输出 |
| 12 | sys_fb_info | framebuffer 信息 |
| 13 | sys_shm_create | 创建共享内存 |
| 14 | sys_shm_attach | 附加共享内存 |
| 15 | sys_pipe | 创建管道 |
| 16 | sys_write | 写 fd |
| 17 | sys_read | 读 fd |
| 18 | sys_close | 关闭 fd |
| 19 | sys_load_dev | 注册设备 |
| 20 | sys_lookup_dev | 查询设备 |
| 21 | sys_notify | 异步通知 |
| 22-23 | — | 保留 |
| 24 | sys_msg | 同步变长消息（≤64KB） |
| 25 | sys_msg_resp | 回复当前 msg 调用者 |

NR_SYSCALL = 26。
