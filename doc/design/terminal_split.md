# Phase 3: 最小 fd + VT100 + Terminal/Shell 拆分

## 目标

将当前"所有进程直写 KMS ring"的扁平 I/O 模型重构为 fd + pipe + terminal 分层模型：

- **子进程 stdout 可路由**：hello.elf 等子进程通过 `sys_write(1, ...)` 输出，经 pipe 路由到 terminal 进程，不再直写 KMS ring
- **stdin 可达**：shell 和子进程通过 `sys_read(0, ...)` 从 stdin pipe 读键盘输入
- **Terminal 进程**：管理 VT100 状态机 + cell 缓冲区，负责屏幕渲染和键盘输入分发
- **Shell 瘦身**：shell 不再 attach kbd/kms shm，仅通过 fd 0/1 与 terminal 通信

## 设计决策摘要

| 决策 | 选择 | 理由 |
|------|------|------|
| fd 表位置 | 内核 `proc_t` 内 | spawn 时 fd 继承需内核参与；VFS 自然对接 |
| fd 表存储 | 固定数组 `[32]` | 简单 O(1) 查找，与 shm_regions 风格一致 |
| fd 范围 | 仅 fd 0 (stdin) 和 fd 1 (stdout) | Phase 3 最小集，无 stderr |
| fd 类型 | 统一 pipe（内部 ring buffer） | pipe 是 Unix 标准抽象，内部用环形缓冲区实现 |
| pipe 缓冲区 | 单页 4KB ring buffer | 文本 I/O 足够，写满阻塞 |
| pipe 同步 | 复用 sys_wait/sys_notify | 不引入新同步原语 |
| Terminal/KMS 关系 | Terminal 写扩展 KMS ring | Terminal 负责 VT100 状态机，KMS 只做像素渲染 |
| 键盘输入路径 | Terminal 读 kbd_ring → pipe → shell | Terminal 拥有输入分发控制权 |
| echo 责任 | Shell 做 echo（Unix 标准） | Shell 可控，支持未来 echo off |
| sys_spawn fd 继承 | 默认继承 fd 0/1（ref_count++） | 简单，run 命令子进程自动获得同样 pipe |
| VT100 子集 | 6 条必须指令 | 清屏/光标/颜色/重置 |
| Terminal 刷新 | 脏区域增量刷新到 KMS ring | 只刷新变化行，效率高 |
| 会话模型 | 单会话 | Phase 3 交付最小集，多会话增量加 |
| sys_pipe 返回 | 用户指针 `int fd[2]` | 类 Linux pipe()，规范 |

## 1. 内核 fd 机制

### 1.1 数据结构

```c
// kernel/proc.h

#define MAX_FD  32

struct pipe {
    uint8_t *buf;        // 4KB ring buffer (kmalloc)
    uint32_t head;       // write position
    uint32_t tail;       // read position
    pid_t read_pid;      // reader process PID
    pid_t write_pid;     // writer process PID
    int ref_count;       // open fd count
};

#define FD_NONE   0
#define FD_PIPE   1

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2

struct file {
    int type;            // FD_NONE / FD_PIPE
    int flags;           // O_RDONLY / O_WRONLY / O_RDWR
    struct pipe *pipe;   // if type == FD_PIPE
};
```

### 1.2 proc_t 扩展

```c
struct proc_t {
    // ... 现有字段 ...
    struct file fd_table[MAX_FD];  // per-process fd table
};
```

初始化：`process_create_elf` 中所有 fd_table 条目初始化为 `type = FD_NONE`。

### 1.3 sys_pipe(fd_ptr)

```c
// syscall #17
uint64_t sys_pipe(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    int *fd_ptr = (int *)arg1;
    // 校验用户指针：非空、不在内核地址空间
    // 找两个空闲 fd 槽位
    // kmalloc 1 页作为 pipe ring buffer
    // 创建 pipe 结构体，ref_count=2
    // fd_table[read_fd] = {FD_PIPE, O_RDONLY, pipe}
    // fd_table[write_fd] = {FD_PIPE, O_WRONLY, pipe}
    // 写 fd_ptr[0]=read_fd, fd_ptr[1]=write_fd
    return 0;  // 成功
}
```

### 1.4 sys_write(fd, buf, len)

```c
// syscall #18
uint64_t sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t) {
    int fd = (int)arg1;
    const char *buf = (const char *)arg2;
    size_t len = (size_t)arg3;
    // 校验 fd 范围 + fd_table[fd].type != FD_NONE + flags 含 O_WRONLY 或 O_RDWR
    // 校验用户 buf 指针范围
    struct pipe *p = current_proc->fd_table[fd].pipe;
    // 逐字节写入 ring buffer：
    //   head == (tail - 1 + PAGE_SIZE) % PAGE_SIZE → 满，阻塞
    //   写满时：设 write_pid = current_pid，sys_wait(0)
    //   读端消费后 notify write_pid
    // 返回实际写入字节数
}
```

### 1.5 sys_read(fd, buf, len)

```c
// syscall #19
uint64_t sys_read(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t, uint64_t) {
    int fd = (int)arg1;
    char *buf = (char *)arg2;
    size_t len = (size_t)arg3;
    // 校验 fd 范围 + fd_table[fd].type != FD_NONE + flags 含 O_RDONLY 或 O_RDWR
    // 校验用户 buf 指针范围
    struct pipe *p = current_proc->fd_table[fd].pipe;
    // head == tail → 空，阻塞
    // 阻塞时：设 read_pid = current_pid，sys_wait(0)
    // 写端写入后 notify read_pid
    // 返回实际读取字节数
}
```

### 1.6 sys_close(fd)

```c
// syscall #20
uint64_t sys_close(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    int fd = (int)arg1;
    // 校验 fd 范围 + fd_table[fd].type != FD_NONE
    struct pipe *p = current_proc->fd_table[fd].pipe;
    p->ref_count--;
    if (p->ref_count == 0) {
        kfree(p->buf);
        kfree(p);
    } else {
        // 通知对端：对端读/写阻塞时需要唤醒
        // 如果关闭的是写端，notify read_pid（read 会返回 0 = EOF）
        // 如果关闭的是读端，notify write_pid（write 会返回 -EPIPE）
    }
    current_proc->fd_table[fd].type = FD_NONE;
    return 0;
}
```

### 1.7 sys_spawn fd 继承

修改 `sys_spawn`：创建子进程后，复制父进程的 fd 0 和 fd 1 到子进程 fd_table，并对 pipe 做 `ref_count++`。

```c
// sys_spawn 中，process_create_elf 之后：
for (int fd = 0; fd <= 1; fd++) {
    if (current_proc->fd_table[fd].type != FD_NONE) {
        child->fd_table[fd] = current_proc->fd_table[fd];
        if (child->fd_table[fd].type == FD_PIPE) {
            child->fd_table[fd].pipe->ref_count++;
        }
    }
}
```

### 1.8 proc_reap fd 清理

`proc_reap` 中遍历 fd_table，对每个 `type != FD_NONE` 的 fd 执行与 `sys_close` 相同的清理逻辑（ref_count--，归零则释放）。

### 1.9 内核启动时 terminal 的 fd 设置

内核在 `kernel_main` 中加载 terminal.elf 时，不为其设置 fd 0/1（terminal 直接操作 kbd_ring 和 KMS ring，不走 pipe）。加载 shell.elf 时，内核创建两对 pipe：

```
pipe_stdin:  terminal fd_table[1]（写端）→ shell fd_table[0]（读端）
pipe_stdout: shell fd_table[1]（写端）→ terminal fd_table[0]（读端）
```

这样 shell 一启动就有 fd 0 (stdin) 和 fd 1 (stdout)。

## 2. Terminal 进程

### 2.1 职责

- 从 kbd_ring 读按键，写入 shell 的 stdin pipe（fd 1 = stdin 写端）
- 从 shell 的 stdout pipe 读数据（fd 0 = stdout 读端），解析 VT100，维护 cell 缓冲区
- 将脏区域刷新到 KMS ring

### 2.2 初始化

```c
void terminal_main() {
    // 1. attach kbd_driver shm（读键盘输入）
    shm_addr = sys_shm_attach(KBD_DRIVER_PID);
    shm_hdr = (driver_shm_header *)shm_addr;
    kbd = (kbd_ring *)(shm_addr + KBD_RING_OFFSET);
    kms = (kms_ring *)(shm_addr + KMS_RING_OFFSET);

    // 2. fd 0 和 fd 1 已由内核设置（stdin 读端和 stdout 写端的 pipe）

    // 3. 获取 framebuffer 信息
    sys_fb_info(&fb_info);

    // 4. 初始化 cell 缓冲区
    init_cell_buffer();

    // 5. 清屏
    fb_clear();
}
```

### 2.3 主循环

```c
while (1) {
    // 1. 读 kbd_ring → 写 stdin pipe（fd 1）
    while (kbd->head != kbd->tail) {
        char ch = kbd->msgs[kbd->tail].ch;
        kbd->tail = (kbd->tail + 1) % KBD_RING_SIZE;
        sys_write(1, &ch, 1);  // 写入 shell 的 stdin pipe
    }

    // 2. 读 stdout pipe（fd 0）→ VT100 解析 → 更新 cell 缓冲区
    char buf[256];
    int n = sys_read(0, buf, sizeof(buf));  // 非阻塞或超时读取
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            vt100_feed(buf[i]);  // 更新 cell 缓冲区 + 脏标记
        }
    }

    // 3. 刷新脏区域到 KMS ring
    flush_dirty_cells();

    // 4. 无工作时睡眠
    // 两个 pipe 都无数据时 sys_wait(poll_timeout)
}
```

### 2.4 VT100 状态机

Terminal 维护 VT100 状态：

```c
struct vt100_state {
    int cursor_x;       // 列 (0..cols-1)
    int cursor_y;       // 行 (0..rows-1)
    int cols;           // 终端列数 (fb_width / 8)
    int rows;           // 终端行数 (fb_height / 16)
    uint32_t fg_color;  // 前景颜色
    uint32_t bg_color;  // 背景颜色
    int escape_state;   // 0=normal, 1=ESC, 2=CSI
    int csi_params[8];  // CSI 参数
    int csi_param_count;
};
```

支持的 VT100 指令（Phase 3 最小子集）：

| 指令 | 作用 |
|------|------|
| `\033[H` | 光标归位（1,1） |
| `\033[%d;%dH` | 光标定位（row, col） |
| `\033[2J` | 清屏 |
| `\033[K` | 清行（光标到行尾） |
| `\033[%dm` | 设置前景/背景色（30-37=fg, 40-47=bg, 0=reset） |
| `\033[m` | 重置所有属性 |

`vt100_feed(char c)` 状态转换：

```
NORMAL → 收到 ESC → ESC_STATE
ESC_STATE → 收到 '[' → CSI_STATE（清空参数缓冲区）
ESC_STATE → 其他 → 处理为普通字符，回 NORMAL
CSI_STATE → 收到数字 → 累积参数
CSI_STATE → 收到字母（最终字节）→ 执行指令，回 NORMAL
CSI_STATE → 收到 ';' → 参数分隔
```

### 2.5 Cell 缓冲区

```c
struct cell {
    uint8_t ch;          // ASCII 字符
    uint32_t fg_color;   // 前景颜色
    uint32_t bg_color;   // 背景颜色
};

struct cell *cells;  // kmalloc(rows * cols * sizeof(struct cell))
int dirty_row_start; // 脏区域起始行
int dirty_row_end;   // 脏区域结束行（不含）
```

vt100_feed 处理普通字符时：写入 `cells[cursor_y * cols + cursor_x]`，标记脏行，移动光标。换行时滚动（首行上移，末行清空）。

### 2.6 脏区域刷新

```c
void flush_dirty_cells() {
    if (dirty_row_start >= dirty_row_end) return;
    for (int row = dirty_row_start; row < dirty_row_end; row++) {
        // 光标移到行首
        kms_msg cursor_msg = {KMS_CMD_CURSOR_MOVE, 0, row, 0};
        write_kms_ring(&cursor_msg);
        // 逐字符写入该行所有 cell
        for (int col = 0; col < cols; col++) {
            struct cell *c = &cells[row * cols + col];
            kms_msg msg = {KMS_CMD_PUTC, c->ch, c->fg_color, 0};
            write_kms_ring(&msg);
        }
    }
    dirty_row_start = rows;
    dirty_row_end = 0;
    // notify KMS driver if sleeping
}
```

## 3. Shell 适配

### 3.1 移除 shm attach

Shell 不再需要 `sys_shm_attach(KBD_DRIVER_PID)`。fd 0 和 fd 1 已由内核设置。

### 3.2 输入改造

```c
// 旧：直接从 kbd_ring 读
char getc() { ... kbd->msgs[kbd->tail].ch ... }

// 新：从 stdin pipe 读
char getc() {
    char ch;
    while (sys_read(0, &ch, 1) != 1) {
        // 阻塞在 pipe 读端
    }
    return ch;
}
```

### 3.3 输出改造

stdout 的 `write_fn` 从 `kms_write_flush` 改为 `sys_write_flush`：

```c
// user/lib/stdio.cc
static void sys_write_flush(FILE *f, const char *data, int len) {
    sys_write(f->fd, data, len);
}
```

stdout 和 stderr 的 `write_fn` 均设为 `sys_write_flush`。

### 3.4 kms_flush 移除

Shell 不再需要 `kms_flush()`（不再直接写 KMS ring）。删除 shell.cc 中的 kms 相关代码。

### 3.5 FS IPC 适配

Shell 的 FS IPC 仍使用硬编码共享页（FS_REQ/FS_RESP），不变。但 `fs_request()` 中的 `kms_flush()` 调用改为 `fflush(stdout)` 或直接移除（sys_write 会自动刷新 pipe）。

## 4. libc 适配

### 4.1 stdin

新增 `FILE *stdin`：

```c
// user/lib/stdio.cc
static FILE stdin_file = { 0, nullptr, 0, 0, _IONBF, _F_READ, sys_read_fill };
FILE *stdin = &stdin_file;
```

`sys_read_fill` 从 fd 0 读数据：

```c
static void sys_read_fill(FILE *f, char *buf, int len) {
    // 实际读取量由 sys_read 返回值决定
    int n = sys_read(f->fd, buf, len);
    // 处理返回值...
}
```

**注意**：当前 FILE 结构体的 `write_fn` 回调是写端抽象。读端需要新增 `read_fn` 回调，或者简化为：stdin 只支持 `fgetc`（内部调 `sys_read(0, &ch, 1)`），不支持缓冲读取。Phase 3 取简化方案。

### 4.2 stdout/stderr

```c
static FILE stdout_file = { 1, nullptr, 0, 0, _IONBF, _F_WRITE, sys_write_flush };
static FILE stderr_file = { 2, nullptr, 0, 0, _IONBF, _F_WRITE, sys_write_flush };
```

`kms_write_flush` 和 `kms_shm_init` 保留但仅 terminal 进程使用。Shell 和子进程的 libc 不再调用它们。

### 4.3 libc 条件编译

Terminal 进程需要直接操作 KMS ring（kms_write_flush），普通进程不需要。两种处理方式：

- **A) terminal 不链接 libc.a**：terminal 用自己的 I/O 函数，直接调 syscall + shm 操作。
- **B) libc.a 保留两套**：`kms_write_flush` 仍在 libc.a 中，普通进程的 stdout 初始化时检查 fd 是否可用，如果 fd 1 是 pipe 则用 `sys_write_flush`，否则 fallback `kms_write_flush`。

推荐 **A**。Terminal 是特殊进程（直接操作 kbd/kms shm），不需要 libc 的 printf 抽象。它的 I/O 直接用 syscall 原语，代码量不大。

## 5. KMS 驱动

KMS 驱动**不变**。仍然：
- 从 KMS ring 读命令（PUTC/CLEAR/SCROLL/CURSOR_MOVE）
- 执行像素渲染到 framebuffer
- 空闲时 kms_sleeping + sys_wait(16)

Terminal 是 KMS ring 的唯一生产者（之前是所有进程都能写），KMS 驱动无需感知此变化。

## 6. 进程启动顺序与磁盘布局

### 6.1 新启动顺序

```
disk_driver → kbd_driver → kms_driver → terminal → shell → fs_driver
```

### 6.2 磁盘布局

| LBA | 内容 | PID |
|-----|------|-----|
| 1-100 | disk_driver.elf | 2 |
| 101-200 | kbd_driver.elf | 3 |
| 201-300 | kms_driver.elf | 4 |
| 301-400 | terminal.elf | 5 |
| 401-500 | shell.elf | 6 |
| 501-600 | fs_driver.elf | 7 |
| 601+ | FAT32 分区 | — |

### 6.3 PID 调整

```c
// common/pid.h
#define DISK_DRIVER_PID   2
#define KBD_DRIVER_PID    3
#define KMS_DRIVER_PID    4
#define TERMINAL_PID      5
#define SHELL_PID         6
#define FS_DRIVER_PID     7
```

### 6.4 kernel_main 修改

```c
// 加载顺序调整
load_elf("disk_driver", LBA 1, IOPL=3, map_fb=false);
load_elf("kbd_driver", LBA 101, IOPL=3, map_fb=false);
load_elf("kms_driver", LBA 201, IOPL=0, map_fb=true);
load_elf("terminal", LBA 301, IOPL=0, map_fb=false);  // 新增
load_elf("shell", LBA 401, IOPL=0, map_fb=false);      // LBA 从 301→401
load_elf("fs_driver", LBA 501, IOPL=0, map_fb=false);  // LBA 从 401→501

// terminal 和 shell 之间的 pipe 在加载 shell 时创建：
//   pipe_stdin:  terminal fd_table[1](W) → shell fd_table[0](R)
//   pipe_stdout: shell fd_table[1](W) → terminal fd_table[0](R)
```

## 7. Syscall 汇总

| # | 名称 | 签名 | 说明 |
|---|------|------|------|
| 17 | SYS_PIPE | `sys_pipe(fd_ptr)` | 创建 pipe，写 [read_fd, write_fd] 到用户指针 |
| 18 | SYS_WRITE | `sys_write(fd, buf, len)` | 向 fd 写入数据 |
| 19 | SYS_READ | `sys_read(fd, buf, len)` | 从 fd 读数据，阻塞直到有数据 |
| 20 | SYS_CLOSE | `sys_close(fd)` | 关闭 fd，pipe ref_count-- |

NR_SYSCALL 从 17 增至 21。

## 8. 实施步骤

### 8.1 内核 fd 基础设施

- [x] `kernel/proc.h`：新增 `struct pipe`、`struct file`、`MAX_FD`、fd 标志常量
- [x] `kernel/proc.h`：`proc_t` 新增 `struct file fd_table[MAX_FD]`
- [x] `kernel/proc.cc`：`process_create_elf` 初始化 fd_table 全为 FD_NONE
- [x] `kernel/proc.cc`：`proc_reap` 遍历 fd_table 清理 pipe ref_count

验证: 编译通过，QEMU 启动正常

### 8.2 sys_pipe + sys_close

- [x] `common/syscall.h`：新增 `SYS_PIPE=17`、`SYS_CLOSE=20` 封装
- [x] `kernel/trap.cc`：实现 `sys_pipe` — 校验指针 + 找空闲 fd + kmalloc pipe + 写用户指针
- [x] `kernel/trap.cc`：实现 `sys_close` — ref_count-- + 归零释放 + notify 对端
- [x] NR_SYSCALL 增至 21

验证: 两个进程通过 sys_pipe + sys_shm_attach 共享 pipe 通信

### 8.3 sys_write + sys_read

- [x] `common/syscall.h`：新增 `SYS_WRITE=18`、`SYS_READ=19` 封装
- [x] `kernel/trap.cc`：实现 `sys_write` — 校验 fd + 写 pipe ring buffer + 满时阻塞 + notify 读端
- [x] `kernel/trap.cc`：实现 `sys_read` — 校验 fd + 读 pipe ring buffer + 空时阻塞 + notify 写端

验证: 进程 A sys_write → pipe → 进程 B sys_read 数据正确

### 8.4 sys_spawn fd 继承

- [x] `kernel/trap.cc`：`sys_spawn` 中复制 fd 0/1 到子进程 + pipe ref_count++

验证: shell run hello.elf，子进程可通过 sys_write(1, ...) 输出

### 8.5 libc 适配

- [x] `user/lib/stdio.cc`：新增 `sys_write_flush`，stdout/stderr write_fn 改为 `sys_write_flush`
- [x] `user/lib/stdio.cc`：新增 stdin FILE（read_fn 调用 sys_read(0, ...)）
- [x] `user/include/stdio.h`：声明 `FILE *stdin`
- [x] `user/lib/start.cc`：移除 `kms_shm_init` 调用（fd 由内核设置，不需要初始化 KMS ring）

验证: hello.elf 通过 printf → sys_write(1, ...) 输出

### 8.6 PID + 磁盘布局调整

- [x] `common/pid.h`：新增 `TERMINAL_PID=5`，shell 改为 6，fs_driver 改为 7
- [x] `kernel/kernel.cc`：调整 ELF 加载顺序和 LBA
- [x] `build.sh`：新增 terminal.elf 编译 + dd 写入 LBA 301，调整 shell/fs_driver LBA + MBR 分区表

验证: 编译通过，QEMU 启动正常（shell 此时无输出，因为还没有 terminal）

### 8.7 Terminal 进程

- [x] 新建 `driver/terminal.cc`：
  - attach kbd_driver shm
  - 初始化 VT100 状态机 + cell 缓冲区
  - 主循环：读 kbd_ring → sys_write(1, ...) 到 stdin pipe；sys_read(0, ...) 从 stdout pipe → vt100_feed → flush_dirty_cells
- [x] kernel_main 中创建 terminal→shell 的 pipe 对并设置 fd_table

验证: shell 通过 terminal 输出文本，键盘输入正确路由

### 8.8 Shell 适配

- [x] `shell/shell.cc`：移除 kbd/kms shm attach 代码
- [x] `shell/shell.cc`：getc() 改为 sys_read(0, ...)
- [x] `shell/shell.cc`：移除 kms_flush() 调用
- [x] `shell/shell.cc`：FS IPC 中移除 kms_flush，改为 fflush(stdout) 或直接移除

验证: shell 交互正常，所有命令正常工作

### 8.9 VT100 验证

- [x] 测试 `\033[H`（光标归位）、`\033[2J`（清屏）、`\033[K`（清行）
- [x] 测试 `\033[31m`（红色文本）、`\033[0m`（重置）
- [x] 测试 `\033[10;5H`（光标定位）
- [x] 子进程（hello.elf）输出正确路由到 terminal

验证: VT100 escape sequence 正确渲染，子进程输出正常

## 9. 数据流总览

```
按键输入:
  keyboard → IRQ33 → kbd_driver → kbd_ring → terminal(sys_read kbd_ring)
    → sys_write(stdin_pipe) → shell(sys_read fd 0) → readline

文本输出:
  shell printf → sys_write(fd 1) → stdout_pipe → terminal(sys_read fd 0)
    → vt100_feed → cell 缓冲区 → flush_dirty_cells → KMS ring
    → kms_driver → framebuffer

子进程输出:
  hello.elf printf → sys_write(fd 1, 继承自 shell) → stdout_pipe
    → terminal → vt100_feed → cell → KMS ring → kms_driver → framebuffer
```

## 10. 后续扩展（不在 Phase 3 范围内）

| 功能 | 依赖 | 说明 |
|------|------|------|
| 多 shell 会话 | Phase 3 完成 | terminal 维护 sessions 数组，Ctrl+Fn 切换 |
| stderr (fd 2) | Phase 3 完成 | 新增 pipe 或与 stdout 共享 |
| sys_dup2 | VFS | 重定向 (`run > file`) |
| 管道 (`ls \| cat`) | sys_dup2 | shell 创建 pipe + dup2 重定向子进程 fd |
| 通用 IPC channel | Wayland | sys_channel_create/connect/send/recv |
| sys_poll / sys_epoll | Wayland | compositor 事件多路复用 |
