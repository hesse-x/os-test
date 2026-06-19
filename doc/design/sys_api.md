# 用户态系统调用 POSIX 封装设计

## 背景

当前所有用户态代码（driver/\*、shell/shell.cc、user/lib/\*）直接调用 `common/syscall.h` 中的底层 `sys_*` 函数（如 `sys_exit`、`sys_getpid`、`sys_write`、`sys_read` 等）。这些底层封装暴露了原始 syscall 编号和 `__syscallN` 内联汇编细节，且参数类型使用 `int64_t` 等底层类型，不够规范。

目标：在 `user/include/` 下提供 POSIX 风格的标准头文件（`unistd.h`、`fcntl.h`、`sys/wait.h` 等），声明符合 POSIX 签名的标准函数（`getpid`、`exit`、`read`、`write`、`close`、`pipe`、`waitpid` 等），实现文件放在 `user/lib/` 下，内部委托 `sys_*`。这样用户态代码使用标准 API，不再直接依赖 `common/syscall.h` 的底层细节。

## 封装清单

### 第一类：POSIX 标准函数（通用，所有用户进程可用）

| POSIX 函数 | 底层 sys_* | 目标头文件 | 备注 |
|-----------|-----------|-----------|------|
| `pid_t getpid(void)` | `sys_getpid()` | `unistd.h` | 返回值类型 pid_t |
| `void _exit(int status)` | `sys_exit(status)` | `unistd.h` | POSIX 低级退出，不刷缓冲区 |
| `ssize_t read(int fd, void *buf, size_t count)` | `sys_read(fd, buf, count)` | `unistd.h` | |
| `ssize_t write(int fd, const void *buf, size_t count)` | `sys_write(fd, buf, count)` | `unistd.h` | |
| `int close(int fd)` | `sys_close(fd)` | `unistd.h` | |
| `int pipe(int fd[2])` | `sys_pipe(fd)` | `unistd.h` | |
| `void _exit(int status)` | `sys_exit(status)` | `unistd.h` | 与 stdlib.h 的 exit() 区别：不刷缓冲区 |
| `pid_t waitpid(pid_t pid, int *status, int options)` | `sys_waitpid(pid, status)` | `sys/wait.h` | options 暂未使用，传 0 |
| `int fcntl(int fd, int cmd, ...)` | — | `fcntl.h` | 暂不实现，预留头文件 |
| `void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)` | `sys_mmap(length)` | `sys/mman.h` | 忽略 addr/prot/flags/fd/offset，仅传 length |
| `int munmap(void *addr, size_t length)` | `sys_munmap(addr, length)` | `sys/mman.h` | |

### 第二类：OS 扩展函数（驱动/特殊场景）

| 函数 | 底层 sys_* | 目标头文件 | 备注 |
|------|-----------|-----------|------|
| `int shm_create(size_t size, void **addr)` | `sys_shm_create(size)` | `sys/shm.h` | 返回 0 成功 / -1 失败，addr 为输出参数 |
| `int shm_attach(pid_t target, void **addr)` | `sys_shm_attach(target)` | `sys/shm.h` | 同上 |
| `int irq_bind(int irq)` | `sys_irq_bind(irq)` | `sys/irq.h` | 驱动专用 |
| `int device_register(pid_t pid, int dev_type)` | `sys_load_dev(pid, dev_type)` | `sys/device.h` | |
| `int msg_fd(int fd, void *msg_buf, size_t msg_len, void *reply_buf, size_t reply_len)` | `sys_dev_msg(fd, ...)` | `sys/ipc.h` | fd-based 变长消息 |
| `int notify(pid_t pid)` | `sys_notify(pid)` | `sys/ipc.h` | |
| `int recv(struct recv_msg *msg, uint32_t timeout_ms)` | `sys_recv(msg, timeout_ms)` | `sys/ipc.h` | |
| `int rpc(pid_t pid, void *req, void *resp)` | `sys_rpc(pid, req, resp)` | `sys/ipc.h` | |
| `int rpc_reply(void *resp)` | `sys_reply(resp)` | `sys/ipc.h` | |
| `int serial_write(const char *buf, size_t len)` | `sys_serial_write(buf, len)` | `sys/serial.h` | |
| `int fb_info(void *buf)` | `sys_fb_info(buf)` | `sys/fb.h` | |
| `pid_t spawn(const void *elf, size_t size, int iopl)` | `sys_spawn(elf, size, iopl)` | `sys/process.h` | |

### 已有封装（stdlib.h / stdio.h）

| 函数 | 底层 | 头文件 | 状态 |
|------|------|--------|------|
| `exit(int)` | `_start` 中调用 `sys_exit` | `stdlib.h` | ✅ 已有 |
| `malloc/free/calloc/realloc` | `sys_mmap/sys_munmap` | `stdlib.h` | ✅ 已有 |

## 新增文件清单

### 头文件（user/include/）

```
user/include/unistd.h          — getpid, _exit, read, write, close, pipe
user/include/sys/wait.h        — waitpid
user/include/sys/mman.h        — mmap, munmap
user/include/sys/shm.h         — shm_create, shm_attach
user/include/sys/irq.h         — irq_bind
user/include/sys/device.h      — device_register
user/include/sys/ipc.h         — notify, recv, rpc, rpc_reply
user/include/sys/serial.h      — serial_write
user/include/sys/fb.h          — fb_info
user/include/sys/process.h     — spawn
```

### 实现文件（user/lib/）

```
user/lib/unistd.cc             — getpid, _exit, read, write, close, pipe
user/lib/sys_wait.cc           — waitpid
user/lib/sys_mman.cc           — mmap, munmap
user/lib/sys_shm.cc            — shm_create, shm_attach
user/lib/sys_irq.cc            — irq_bind
user/lib/sys_device.cc         — device_register
user/lib/sys_ipc.cc            — notify, recv, rpc, rpc_reply
user/lib/sys_serial.cc         — serial_write
user/lib/sys_fb.cc             — fb_info
user/lib/sys_process.cc        — spawn
```

## 类型定义

在 `user/include/sys/types.h` 中定义（新建）：

```c
typedef int pid_t;
typedef long ssize_t;
typedef long off_t;
```

## errno 处理

当前 syscall 返回负 errno（`-ENOMEM` 等）。POSIX 函数约定：返回 -1 并设置 `errno`。

封装层统一模式：
```c
int64_t r = sys_xxx(args);
if (r < 0) {
    errno = (int)(-r);
    return -1;
}
return (int)r;
```

需要在 `user/include/errno.h` 中定义 `errno` 变量（或 thread-local），并从 `common/errno.h` re-export 错误码常量。当前阶段为单线程，用全局变量即可。

## 迁移策略

1. **新建头文件 + 实现文件**，CMake 中加入 libc
2. **逐步替换**各模块中的 `sys_*` 调用为 POSIX 函数：
   - shell/shell.cc：`sys_read` → `read`，`sys_write` → `write`，`sys_getpid` → `getpid`，`sys_spawn` → `spawn`，`sys_waitpid` → `waitpid`，`sys_shm_attach` → `shm_attach`，`sys_notify` → `notify`，`sys_recv` → `recv`，`sys_serial_write` → `serial_write`
   - driver/terminal.cc：`sys_read` → `read`，`sys_write` → `write`，`sys_shm_attach` → `shm_attach`，`sys_rpc` → `rpc`，`sys_getpid` → `getpid`，`sys_mmap` → `mmap`，`sys_fb_info` → `fb_info`，`sys_notify` → `notify`，`sys_yield` → `sched_yield`，`sys_recv` → `recv`
   - driver/kbd_driver.cc：`sys_irq_bind` → `irq_bind`，`sys_getpid` → `getpid`，`sys_load_dev` → `device_register`，`sys_recv` → `recv`，`sys_notify` → `notify`，`sys_shm_create` → `shm_create`，`sys_reply` → `rpc_reply`
   - driver/disk_driver.cc：`sys_shm_create` → `shm_create`，`sys_recv` → `recv`，`sys_notify` → `notify`
   - driver/kms_driver.cc：`sys_shm_attach` → `shm_attach`，`sys_fb_info` → `fb_info`，`sys_recv` → `recv`
   - driver/fs_driver.cc：`sys_shm_create` → `shm_create`，`sys_shm_attach` → `shm_attach`，`sys_getpid` → `getpid`，`sys_load_dev` → `device_register`，`sys_recv` → `recv`，`sys_notify` → `notify`，`sys_serial_write` → `serial_write`
3. **替换完成后**，用户态代码不再直接 `#include "common/syscall.h"`，改为使用对应 POSIX 头文件
4. **libc 内部**（`stdio.cc`、`malloc.cc`、`start.cc`）可保留 `sys_*` 调用，因为它们是 libc 自身实现，需要直接访问底层 syscall

## 不封装的 syscall

以下 syscall 仅在极特殊的内核交互场景使用，不封装为 POSIX 函数：

- `SYS_YIELD`：用户态极少直接调用（仅 terminal.cc 用了一次），可用 `sched_yield()` 封装（`unistd.h`）
- `SYS_SERIAL_WRITE`：已删除，串口镜像下沉到 sys_write FD_PIPE 路径

## CMake 变更

在 `user/CMakeLists.txt` 的 libc target `c` 中添加新的 .cc 源文件：

```cmake
add_user_lib(c SOURCES
    lib/stdio.cc
    lib/string.cc
    lib/start.cc
    lib/malloc.cc
    lib/sys.cc
    lib/unistd.cc
    lib/sys_wait.cc
    lib/sys_mman.cc
    lib/sys_shm.cc
    lib/sys_irq.cc
    lib/sys_device.cc
    lib/sys_ipc.cc
    lib/sys_serial.cc
    lib/sys_fb.cc
    lib/sys_process.cc
)
```

## 替换示例

替换前（shell/shell.cc）：
```cpp
#include "common/syscall.h"
// ...
freq->client_pid = sys_getpid();
	sys_notify(sys_lookup_dev(DEV_FS));
{ struct recv_msg m; sys_recv(&m, 0); }
int64_t child_pid = sys_spawn((const void *)elf_buf, (uint64_t)file_size, 0);
int64_t result = sys_waitpid((int32_t)child_pid, &exit_code);
```

替换后：
```cpp
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/device.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <fcntl.h>
// ...
freq->client_pid = getpid();
int fs_fd = open("/dev/fs", O_RDWR);
msg_fd(fs_fd, &freq, sizeof(freq), &fresp, sizeof(fresp));
{ struct recv_msg m; recv(&m, 0); }
pid_t child_pid = spawn((const void *)elf_buf, (size_t)file_size, 0);
pid_t result = waitpid((pid_t)child_pid, &exit_code, 0);
```
