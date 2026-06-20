# 微内核实现进度

## 项目目标

| 目标 | 阶段 | 验收标准 |
|------|------|----------|
| **支持简单 ELF 可执行文件的执行** | 短期 | 在宿主机编译 hello world，静态链接 libc.a，在 OS 上执行并输出字符 |
| **支持 Wayland 核心协议** | 中期 | 见下方 [Wayland 验收标准](#wayland-验收标准) |
| **支持构建 gcc** | 远期 | 成功在 OS 上构建出 gcc |

### Wayland 验收标准

在 OS 上运行 Wayland compositor 和至少 2 个客户端进程，客户端通过 wl_shm 共享内存提交像素缓冲区到 surface，compositor 合成所有 surface 并 page-flip 到 framebuffer 显示。键盘输入通过 wl_seat 路由到获得键盘焦点的 surface。

具体验收点：

1. **wl_display + wl_registry**：客户端连接 compositor，获取全局对象列表（wl_compositor、wl_shm、wl_seat、wl_output）
2. **wl_compositor + wl_surface**：客户端创建 surface，提交 damage 区域
3. **wl_shm + wl_buffer**：客户端通过 mmap 创建共享内存池，从中分配 wl_buffer，attach 到 surface 并 commit
4. **wl_seat + wl_keyboard**：compositor 将键盘事件路由到焦点 surface 的客户端
5. **compositor 合成 + page flip**：compositor 读取所有 surface 的 buffer 内容，合成到 framebuffer，双缓冲原子切换显示
6. **多客户端**：至少 2 个客户端同时运行，各自有独立 surface，compositor 正确合成它们的像素内容

验收演示：2 个客户端分别用不同颜色填充矩形区域（如红色和蓝色），compositor 将它们合成到屏幕上，可见两个色块同时显示。按键盘切换焦点，焦点客户端收到键盘事件。

**不在验收范围内**：wl_pointer/鼠标、xdg-shell/window decoration、wl_output 详细模式通告、客户端库封装（验收只需要 test client）

## 已完成（设计文档索引）

| 功能 | 设计文档 |
|------|----------|
| x86-64 迁移 | [x64_migration.md](x64_migration.md) |
| UEFI 引导 | [uefi.md](uefi.md) |
| 多核 SMP — Per-CPU 基础设施 | [smp.md](smp.md) |
| 多核 SMP — APIC 替换 PIC | [smp.md](smp.md) |
| 多核 SMP — AP 启动 | [smp.md](smp.md) |
| SYSCALL/SYSRET 快速系统调用 | [syscall.md](syscall.md) |
| TSS IST 栈 | [tss_ist.md](tss_ist.md) |
| NX 位 | [nx_bit.md](nx_bit.md) |
| 自旋锁原语 | [spinlock.md](spinlock.md) |
| AP 参与调度（idle 进程模型 + pick_cpu） | [schedule.md](schedule.md) |
| 细粒度锁拆分（BKL 移除） | [fine_grained_lock.md](fine_grained_lock.md) |
| FAT32 文件系统 | [fat32.md](fat32.md) |
| 内存管理系统（slab + mmap + 用户态 malloc 重写） | [mem.md](mem.md) |
| 进程生命周期管理（sys_exit/sys_waitpid/sys_spawn） | [process_lifecycle.md](process_lifecycle.md) |
| libc.a 静态库（printf + FILE + _start） | [libc.md](libc.md) |
| KMS 用户态驱动（display 协议 + back buffer flip） | [kms.md](kms.md) |
| TSC 时钟 + sched_clock + udelay 修复 | [driver_workflow.md](driver_workflow.md) |
| 定时等待队列 + sys_wait(timeout_ms) | [driver_workflow.md](driver_workflow.md) |
| sys_fb_info / sys_shm_create / sys_shm_attach | [driver_workflow.md](driver_workflow.md) |
| 共享页重构（KBD/KMS 硬编码→动态 shm） | [driver_workflow.md](driver_workflow.md) |
| Phase 3: 最小 fd + VT100 + Terminal/Shell 拆分 | [terminal_split.md](terminal_split.md) |
| 设备管理器（dev_table + sys_load_dev/sys_open_dev） | [dev_table.md](dev_table.md) |
| 统一 IPC 机制（sys_recv/sys_req/sys_resp + sys_msg/sys_msg_resp） | [rpc.md](rpc.md) |
| 时间子系统（sys_gettime/sys_clock + libc timespec_get/clock） | [time.md](time.md) |
| 文件系统重构（LFN + FHS 目录结构 + Shell 路径执行） | [fs_restructure.md](fs_restructure.md) |
| libc 文件 I/O（open/read/write/close via sys_msg） | [rpc.md](rpc.md) |
| 用户态驱动（3 层 kbd + req bind/unbind + 各驱动工作流） | [user_driver.md](user_driver.md) |
| 构建系统（CMake + mkdisk + mkimg） | [cmake.md](cmake.md) |
| Shell 设计（命令 + FS IPC + 路径执行） | [shell.md](shell.md) |
| Unix domain socket 设计 | [socket.md](socket.md) |
| VFS 统一 I/O 架构（FD_FILE + kernel_msg_send） | [vfs.md](vfs.md) |
| SHM fd 化重构（fd + mmap 模式替代 vaddr + shm_regions） | [shm.md](shm.md) |
| fs_driver 异步事件循环 + 文件写入 | [file_system.md](file_system.md) |
| xHCI + MSI-X（PCIe USB 控制器 + MSI-X 中断分配） | [xhci.md](xhci.md) |
| USB HID 键盘迁移（PS/2 → xHCI Transfer Ring + SHM ring） | [kbd.md](kbd.md) |

## 当前状态

内核已完整运行：UEFI 引导 → 内核初始化 → AP 启动（参与调度）→ 加载 2 个 ELF（fs_driver + init）→ init 从 FAT32 启动 kbd/kms/terminal → terminal 启动 shell → 多进程协作。

**统一 IPC 完成**：sys_recv/sys_req/sys_resp + sys_msg/sys_msg_resp + AF_UNIX SOCK_STREAM socket + SCM_RIGHTS fd 传递 + poll 事件多路复用。sys_recv 统一接收 IRQ/REQ/NOTIFY/MSG 四类消息（per-process 16 slot recv 队列 + 4 参数签名支持变长数据），sys_req 同步内联请求（56 字节载荷），sys_msg 同步变长消息（≤64KB，内核 kmalloc 中转拷贝）。NR_SYSCALL=46（0-45），串口 syscall（SYS_SERIAL_WRITE）已删除。串口镜像下沉到 sys_write FD_PIPE 路径自动完成。内核新增 serial_printf 格式化输出。

**Socket 完成**：AF_UNIX SOCK_STREAM socket（kernel/socket.h + kernel/socket.cc），10 个 syscall（SYS_SOCKET~SYS_POLL，编号 37-46），包括 socket/bind/listen/accept/connect/socketpair/sendmsg/recvmsg/shutdown/poll。SCM_RIGHTS fd 传递（lazy 安装）。sock_write/sock_read 内部辅助（write/read 对 socket fd 的兼容路径）。WAIT_POLL 阻塞状态。

**VFS 完成**：FD_FILE 类型 + kernel_msg_send 内核代理 IPC，sys_install_fd (#36) 注册 FD_FILE，libc open() 统一路径（sys_msg → sys_install_fd → 统一 syscall I/O）。sys_read/sys_write/sys_close/sys_dup2 扩展 FD_FILE 分支。FD_SOCKET 类型 + sock_write/sock_read 内部辅助。libc read()/write()/close() 统一委托内核分发。

**SHM fd 化完成**：sys_shm_create 返回 fd，sys_mmap 6 参支持 MAP_SHARED + fd。struct shm 引用计数管理（fd/vma 各保活）。sys_munmap 识别 shm_obj 并 shm_put。kernel SHM（USB HID）标记 SHM_KERNEL。

**Display 协议完成**：KMS 驱动重构为 display backend（创建 display SHM + back buffer flip），Terminal 作为 compositor 渲染 cell 到 back buffer（display client API），driver/display.h + driver/font.h 提取为共享组件。socket 基础设施就绪后 compositor 可迁移为独立 Wayland compositor 进程。

**文件系统完成**：fs_driver 异步事件循环 + sys_block_async 异步磁盘 I/O。多客户端 session（MAX_CLIENTS=16，每客户端 8 个 fd）。VFAT LFN 读写、FHS 目录结构、Shell 路径执行（替代 `run` 命令）。完整文件写入（write cmd：FAT 链遍历 + 簇分配/扩展 + 数据写入 + 目录项更新）。libc 文件 I/O（user/lib/file.cc）通过 FD_FILE 路径统一管理。

**时间子系统完成**：sys_gettime（全局单调时钟）+ sys_clock（per-process CPU 时间），libc timespec_get/clock 封装。

**设备自注册**：所有驱动通过 device_register(getpid(), DEV_XXX) 自注册（kbd_driver、disk_driver、kms_driver、fs_driver）。

## 多核 SMP — 调度与锁（全部完成）

### 步骤 1: 自旋锁原语 ✅
### 步骤 2: BKL 大内核锁 ✅（已移除，替换为细粒度锁）
### 步骤 3: AP 参与调度 ✅ — [schedule.md](schedule.md)
### 步骤 4: 细粒度锁拆分 ✅ — [fine_grained_lock.md](fine_grained_lock.md)

---

## 短期目标 — 支持简单 ELF 可执行文件的执行 ✅

核心机制已具备，验收流程可走通：宿主机编写 hello.c → gcc 编译 + 静态链接 libc.a → hello.elf 写入 FAT32 → shell `hello.elf` 路径执行 → 输出 "Hello, World!"。

### 进程退出 + 资源回收（sys_exit / sys_waitpid） ✅ — [process_lifecycle.md](process_lifecycle.md)
### 运行时加载新进程（sys_spawn） ✅
### 异常退出路径改造 ✅
### libc.a 静态库 ✅ — [libc.md](libc.md)
### KMS 用户态驱动 ✅ — [kms.md](kms.md)

### FAT32 完善

| 功能 | 说明 | 优先级 |
|------|------|--------|
| ~~文件写入 (write)~~ | ~~fs_driver 支持文件内容写入~~ | ✅ 已完成 |
| 删除 (unlink/rmdir) | 目录项标记 0xE5 + FAT 簇释放 | 中 |
| RTC 时间源 | UEFI 获取初始时间 → sys_gettime syscall → 真实时间戳 | 中 |

---

## 中期目标 — 支持 Wayland 核心协议

需建立 IPC 通道 + mmap + 双缓冲 + compositor 等基础设施。

### Phase 1: 内核基础设施 ✅

#### sys_mmap / sys_munmap ✅

- [x] sys_mmap(size)：匿名私有映射（设计见 [mem.md](mem.md) Phase 3）
- [x] sys_munmap(addr, size)：解除映射 + 归还物理页
- [x] sys_mmap 6 参（addr/size/prot/flags/fd/offset）+ MAP_SHARED + fd 映射
- **后续**: 共享映射（MAP_SHARED + refcount），见 [mem.md](mem.md) Phase 6

#### VFS + fd 抽象 ✅

- [x] 内核 fd 表 + sys_pipe/sys_write/sys_read/sys_close
- [x] `struct file` 添加 FD_FILE 类型 + file_data（fs_pid/fs_fd/offset/file_size/ref_count）
- [x] `kernel_msg_send()` 内部接口 — 内核向 fs_driver 代理 IPC
- [x] sys_install_fd (#36) + libc open() 改造（sys_msg → 注册 FD_FILE → 统一 syscall I/O）
- [x] sys_read/sys_write/sys_close/sys_dup2 扩展 FD_FILE 分支（kernel_msg_send 代理）
- [x] `struct file` 添加 FD_SOCKET 类型 + struct unix_sock *sock
- [x] sys_read/sys_write/sys_close 扩展 FD_SOCKET 分支（sock_write/sock_read/sock_close）
- [x] 统一 read()/write()/close() 在 libc 中不再分类型，全部委托内核 syscall 分发
- [ ] 验证: 用户进程通过 open("hello.txt") → sys_read(fd, buf) 读取 FAT32 文件

#### Unix domain socket（内核态 AF_UNIX SOCK_STREAM）✅

详见 [socket.md](socket.md)。

- [x] skb 链表（struct sk_buff + struct unix_sock）— `kernel/socket.h`, `kernel/socket.cc`
- [x] 全局 socket_lock（spinlock_t，锁顺序：procs_lock → socket_lock → scheduler_lock）
- [x] bind 名字空间表（hash map: sun_path → struct unix_sock*）
- [x] sys_socket(AF_UNIX, SOCK_STREAM, 0) → fd
- [x] sys_bind + sys_listen + sys_accept + sys_connect（命名 socket，路径绑定）
- [x] sys_socketpair（双向字节流 fd pair，可替换 pipe）
- [x] sys_sendmsg + sys_recvmsg（struct msghdr + struct iovec 100% Linux 兼容）
- [x] SOCK_STREAM consumed 部分读 + skb 生命周期管理
- [x] fd passing (SCM_RIGHTS)：sendmsg 验证 fd, recvmsg 时 lazy 安装到接收方
- [x] sys_shutdown(SHUT_RD/SHUT_WR/SHUT_RDWR) + EOF/EPIPE 处理
- [x] sock_write/sock_read 内部辅助（write/read 在 socket fd 上的兼容路径）
- [x] WAIT_POLL 阻塞状态 + wake_process 扩展
- [ ] 验证: 两个进程通过 Unix socket 连接 → sendmsg/recvmsg 收发数据 → SCM_RIGHTS 传递 shm fd

#### poll（统一事件多路复用）✅

- [x] sys_poll(struct pollfd *, nfds, timeout_ms) — 核心实现（kernel/socket.cc）
- [x] 支持 pipe 可读 (POLLIN) / 可写 (POLLOUT) 事件
- [x] 支持 socket 可读 (POLLIN) / 可写 (POLLOUT) / 挂起 (POLLHUP) 事件
- [x] 全部未就绪 → 阻塞（WAIT_POLL + wait_deadline）+ 超时唤醒
- [x] `common/socket.h` — pollfd + POLLIN/POLLOUT/POLLERR/POLLHUP 定义
- [x] `user/include/sys/socket.h` — libc poll() 封装
- [ ] 验证: compositor 用 poll 同时监听 N 个 client socket

#### 信号机制（TTY Ctrl+C 依赖）

**范围：Level 2** — `sys_kill` + `sys_sigaction` + `sys_sigreturn` + pending bits + default action（SIGINT/SIGTERM→exit, SIGCHLD→ignore）。sigprocmask/EINTR/SIGPIPE 预留接口，暂不实现。详见 [signal.md](signal.md)。

- [ ] `sys_kill(pid, sig)` — 设置目标进程 pending bits
- [ ] `sys_sigaction(sig, act, oldact)` — 注册/查询信号 handler
- [ ] `sys_sigreturn()` — 信号 handler 返回后恢复上下文
- [ ] 信号投递：iret 前检查 pending signals → 无 handler 则 default action，有 handler 则 trampoline
- [ ] SIGCHLD：替代现有 RECV_NOTIFY 子进程退出通知（waitpid 不受影响）
- [ ] SIGINT（Ctrl+C）：terminal 通过 kill(pid, SIGINT) 向前台进程发中断信号
- [ ] 验证: shell 前台进程运行时按 Ctrl+C → 进程退出 → shell 返回提示符

**不做**：EINTR 中断阻塞 syscall、SIGPIPE（保持 -EPIPE）、sigprocmask（按需后续加）、作业控制（Ctrl+Z/fg/bg 后续加）。

#### futex（不实现）

**结论：不做。** 评估后确定 futex 在当前系统上需求不存在：
- 无用户态多线程（无 `clone`、无共享地址空间进程）→ 无用户态锁竞争
- 所有进程间同步已有 `sys_recv(timeout)` + `sys_poll()` + `sys_notify()` 覆盖
- Wayland compositor 是单线程事件驱动模型，不需要用户态锁
- futex 的价值 = 用户态原子操作 + 内核 fallback 休眠，前提是有多线程争锁场景
- 未来若引入多线程（pthread/clone），再按需实现

#### shm 改进 ✅

- [x] shm 与 fd 绑定：sys_shm_create 返回 fd（类似 memfd_create），通过 SCM_RIGHTS 传递
- [x] 放宽数量限制：移除 MAX_SHM_PER_PROC=4 限制（shm_regions[] 已删除）
- [x] sys_shm_attach 改为 fd-based（返回 fd，随后 mmap）
- [ ] 对齐 Linux `memfd_create` 签名：新增 `SYS_MEMFD_CREATE（name, flags）+ SYS_FTRUNCATE（fd, size）`，支持 MFD_CLOEXEC + MFD_ALLOW_SEALING。保留 `sys_shm_create` 作为内部快捷方式
- [ ] 验证: 进程 A 创建 shm fd → 通过 Unix socket 传递 → 进程 B mmap 访问

#### 鼠标驱动

- [ ] USB HID 鼠标驱动（xHCI interrupter 1 + SHM mouse sub-ring + get_mouse_event）
- [ ] 验证: 鼠标移动事件写入 mouse_shm，compositor 可读取

#### 像素渲染基础

- [x] Display 协议（display_shm_header + back buffer + generation counter）— [kms.md](kms.md)
- [ ] 合成器进程像素级绘制原语：fill_rect / blit
- [ ] KMS 驱动改造：支持 compositor page flip 请求
- [ ] 验证: 合成器填充红色矩形到 back buffer → flip → 屏幕显示红色

### Phase 2: Wayland 用户态服务

#### Terminal line discipline（用户态）

- [ ] terminal 内建 TTY line discipline（echo、raw/canonical 模式切换）
- [ ] Ctrl+C（SIGINT）→ kill shell 前台进程
- [ ] Ctrl+Z（SIGTSTP）→ 作业停止
- [ ] 行缓冲编辑（退格、Ctrl+U 清行），替代 shell readline
- [ ] 验证: shell 吃原始字节流，terminal 端完成行编辑和信号生成

#### Wayland 协议库（wire format）

- [ ] Wayland wire format 编解码
- [ ] 验证: 编码一条 wl_display.get_registry 请求 → 解码正确

#### wl_display + wl_registry

- [ ] compositor: wl_display + wl_registry
- [ ] 验证: 客户端连接 compositor，获取 registry，绑定 wl_compositor

#### wl_compositor + wl_surface

- [ ] compositor: wl_compositor.create_surface
- [ ] surface: damage/attach/commit/frame
- [ ] 验证: 客户端创建 surface，attach buffer，commit

#### wl_shm + wl_buffer

- [ ] wl_shm.create_pool / wl_shm_pool.create_buffer
- [ ] 验证: 客户端 mmap 创建 pool → 创建 buffer → 填充像素 → attach + commit

#### wl_seat + wl_keyboard

- [ ] wl_seat + wl_keyboard
- [ ] compositor 焦点管理
- [ ] 验证: 键盘事件路由到焦点客户端

#### compositor 合成循环

- [ ] compositor 主循环：poll + 处理请求 + 合成 + page flip
- [ ] 验证: 2 个客户端分别提交红/蓝矩形 → 合成 → 屏幕显示两个色块

#### Test client

- [ ] 简单 test client
- [ ] 验证: test client 运行，屏幕出现彩色矩形

#### 图形终端（Terminal 重写为 Wayland 客户端）

- [ ] terminal 进程改为 Wayland 客户端：`wl_display_connect` → `wl_compositor_create_surface`
- [ ] font 渲染引擎（等宽字体 + 颜色 + Unicode）
- [ ] 通过 wl_shm 创建共享 buffer，渲染 VT100 cell 到 surface
- [ ] 合成器收到 keyboard focus → terminal 获得 wl_keyboard 事件
- [ ] 验证: 图形终端启动，可输入命令、显示输出，外观类似 Ubuntu gnome-terminal

---

## 远期目标 — 支持原生构建 clang/LLVM

> **最低可演示目标：** 在 Xos 上通过 `make`/`ninja` 启动 clang 编译过程，cc1 能处理一个简单 C 文件输出 .o，ld 链接成 ELF。
>
> **完成态目标：** Xos 自举 — 在 Xos 上用 clang 编译 clang 源码自身。

### 🔴 P0：绝对无法绕过

| 能力 | 依赖 | 说明 | 预估 |
|------|------|------|------|
| **fork + execve** | kernel syscall | build system（make/ninja）必须 spawn 子进程（编译→汇编→链接）。没有 fork/exec 构建系统不能运行 | sprint 3-4 |
| **信号机制（基础）** | kernel syscall + libc | `SIGCHLD`（waitpid 依赖它知道子进程退出）、`signal()`/`sigaction()`/`kill()`、信号投递（用户态返回前检查 pending signals） | sprint 3-4 |
| **FPU/SSE 上下文切换** | kernel context switch | clang/LLVM **强制使用 SSE/SSE2**。当前 kernel context switch 不保存 xmm 寄存器，编译 flags `-mno-sse -mno-sse2` 必须移除。内核 context switch 需 fxsave/fxrstor 或 xsave/xrstor 保存 FPU/SSE 状态 | sprint 4-5 |
| **environ + getenv** | libc | 构建系统用环境变量传 PATH/CC/CFLAGS/LDFLAGS。无环境变量连 `which cc` 都跑不了。需内核每进程 environ 区 + execve 继承 | sprint 5 |
| **/dev/null** | dev_table | 构建脚本大量使用 `>/dev/null 2>&1`。最简单的新增设备 | sprint 5 |
| **磁盘空间** | mkdisk.sh + FAT32 | LLVM 源码 ~500MB，构建产物 ~5GB。当前 disk.img 64MB 需 ≥ 8GB。另需解决 FAT32 4GB 单文件上限 | sprint 5-6 |

### 🟡 P1：快速构建必须

| 能力 | 依赖 | 说明 | 预估 |
|------|------|------|------|
| **execvp / PATH 搜索** | libc | 构建系统需要根据 PATH 搜索 cc/ld/as 等可执行文件 | sprint 5 |
| **sys_mmap 文件映射** | kernel mmap | mmap 文件 fd + MAP_SHARED。LLVM 通过 llvm::MemoryBuffer 大量使用 mmap 文件映射，当前只支持匿名映射 | sprint 5-6 |
| **pthread (基本)** | kernel + libc | LLVM 编译不强制多线程，但 ninja 需要做并行构建。无 pthread 只能单核串行编译。需 `clone` syscall + TLS（FS_BASE） | sprint 7-9 |
| **getrlimit/setrlimit** | kernel syscall | 构建工具检查资源限制（open files, stack size） | sprint 6 |
| **waitpid WNOHANG** | kernel proc | 非阻塞等待子进程，ninja 依赖此实现并行调度 | sprint 3-4 |

### 🔵 P2：clang 运行时需要

| 能力 | 依赖 | 说明 |
|------|------|------|
| **termios** | libc/驱动 | tcsetattr/tcgetattr，部分构建脚本/shell 需要 |
| **symlink** | fs_driver | 构建系统创建链接。FAT32 不支持，需考虑其他 FS |
| **/dev/zero** | dev_table | 一些工具需要 |
| **Shebang `#!` 支持** | sys_exec | 脚本执行（`./configure` 等） |
| **sigprocmask** | kernel signal | 信号阻塞掩码 |
| **setpgid/getpgid** | kernel proc | 进程组，作业控制 |
| **alarm/setitimer** | kernel timer | 超时控制 |

### 里程碑路径

```
sprint 1-2: POSIX 扩展波（libc 完善 + lseek + dirent + unlink/fstat 等）
              ↓
sprint 3-4: fork + execve + 信号机制（基础）
              ↓
sprint 4-5: FPU/SSE 上下文切换 + -mno-sse 移除 + 用户态 SSE 使能
              ↓
sprint 5-6: 环境变量 + /dev/null + disk.img 扩容 + 文件 mmap + PATH 搜索
              ↓
sprint 7-9: pthread + TLS（FS_BASE）+ 初步构建验证
              ↓
sprint 10+: 交叉编译 clang for Xos → 在 Xos 上测试 cc1 → 自举
```

### 替代路径：交叉编译

如果仅需在 Xos 上**运行 clang**（而非在 Xos 上原生编译 clang），可跳过 SSE 和 pthread 的最困难依赖：

1. 宿主机配置 `--target=x86_64-xos` 交叉编译 clang
2. Xos 只需 fork/exec + 信号 + 环境变量 + /dev/null
3. clang 在 Xos 上运行 cc1/as/ld 处理 C 文件

这约等于 sprint 3-5 即可完成，跳过了 sprint 7-9。

### 现有相关 TODO 迁移

- [ ] sys_fork + sys_exec（含 ELF loader 复用）
- [ ] sys_sigaction / sys_sigprocmask / sys_kill / sys_rt_sigreturn
- [ ] 信号投递：进程返回用户态前检查 pending signals（trapret 路径）
- [ ] SIGCHLD：子进程 exit 时向父进程投递
- [ ] SIGINT（Ctrl+C）：terminal 通过 kill(pid, SIGINT) 向前台进程发中断信号
- [ ] 验证: shell 前台进程运行时按 Ctrl+C → 进程退出 → shell 返回提示符
- [ ] FPU/SSE lazy context switch（fxsave/ fxrstor, CR0.TS + device_not_available #NM 处理）
- [ ] 环境变量区（process_create 扩展 environ page）+ copy on exec
- [ ] 验证: getenv("PATH") == "/bin:/usr/bin"
- [ ] /dev/null 设备（DEV_NULL = 6，读写丢弃/返回 EOF）
- [ ] disk.img 扩容至 8GB + FAT32 大分区
- [ ] sys_mmap 文件映射：MAP_SHARED file-backed mmap（在 mmap 中识别 fd→FD_FILE→kernel_msg_send 读 fs_driver）
- [ ] 验证: mmap 文件 → 读内容 → 写回 → munmap
- [ ] 单核验证: make 编译单文件 C 程序
- [ ] 预置 clang-for-Xos 交叉编译器到 FAT32
- [ ] 验证: exec clang → cc1 输出 .o → ld 链接 → 运行产物

### Sprint 1-2: POSIX 扩展波（详细计划见 [[posix.md]]）

- [ ] syscall 清理：重排编号 + DEV_MSG 移除 + BLOCK_READ/WRITE 合并 + SYS_LSEEK 新增
- [ ] fs_driver: UNLINK/RMDIR/FSTAT/OPENDIR/DIRENT/CLOSEDIR/SEEK/ACCESS
- [ ] libc: sprinft/snprintf/strtol/ctype/assert/getcwd/sleep/uname
- [ ] libc: fopen/fclose/fread/fwrite/fseek/ftell/rewind
- [ ] libc: opendir/readdir/closedir + struct stat 扩展
- [ ] libc: lseek/mkdir/unlink/rmdir/access/isatty
- [ ] libc: memcmp/strstr/strtok/strtok_r/strerror/qsort/rand/abs

---

## 已知 Bug 与技术债务

### 高（SMP 竞态 / 内存安全）

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 9 | 进程创建失败内存泄漏 | `kernel/proc.cc` | `process_create_elf` 后续 `alloc_page` 失败时，前面已分配的页面未释放 |

### 中（逻辑错误 / 健壮性）

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 16 | 用户栈仅 4KB 无 guard page | `kernel/proc.cc` | 栈溢出触发 #PF 被 kill |
| 17 | 内核栈仅 8KB | `kernel/proc.cc` | 深层调用路径偏紧 |
| 20 | `pick_cpu` 总倾向 CPU 0 | `kernel/proc.cc` | 所有 CPU run_count 相同时返回 0 |
| 21 | `pid` 未校验上界 | `kernel/trap.cc` | `procs[pid]` 未检查 `pid >= MAX_PROC` |
| 22 | `malloc free` 不验证指针合法性 | `user/lib/malloc.cc` | 任意指针损坏空闲链表 |
| 30 | socket 全局锁粒度 | `kernel/socket.cc` | 全局 socket_lock 在大量连接时成为瓶颈，应升级为 per-socket spinlock + per-hash-bucket spinlock |

### 低（构建 / 代码质量）

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 25 | 缺少 `-mcmodel=kernel` | `CMakeLists.txt` | C 用 `-fno-pie` 绝对寻址错误 |
| 26 | 链接脚本缺 section 对齐 | `build_script/linker.ld` | 无法设置不同页权限 |
| 28 | `boot_info` 复制硬编码 128B | `arch/x64/start.S` | 结构体扩展会截断 |

---

## Q35 + AHCI 后续

| 功能 | 优先级 | 状态 | 说明 |
|------|--------|------|------|
| ~~异步块设备~~ | ~~中~~ | ✅ 已完成 | `sys_block_async` + RECV_NOTIFY 完成回调，fs_driver 事件循环已使用 |
| USB 键盘迁移 | 中 | ✅ 已完成 | xHCI Transfer Ring + HID Boot Protocol + SHM ring → 替代 PS/2 键盘，见 [kbd.md](kbd.md) |
| xHCI 热插拔 | 低 | 待做 | 需内核工作队列机制，Port Status Change 事件处理 + 异步枚举状态机 |
| NVMe 驱动 | 低 | 待做 | PCIe + 多队列 + PRP/SGL |
| 块设备文件系统 `/dev/sda` | 低 | 待做 | fs_driver 通过 open + read/write 访问磁盘，而非专用 syscall |
| USB 键盘端到端验证 | 中 | 待做 | xHCI 枚举 + Transfer Ring + ISR → SHM → kbd_driver → terminal 需串口调试验证 |
| `-vga none` | 低 | 已有基础设施 | GPU 原生 mode setting 后可移除 VGA 仿真 |

---

## 其他扩展

| 功能 | 依赖 | 优先级 | 说明 |
|------|------|--------|------|
| malloc/free 加锁 | — | 中 | MALLOC_LOCK/MALLOC_UNLOCK 占位宏替换为实际锁 |
| 串口打印统一 NDEBUG 控制 | — | 低 | 全项目串口输出用宏包装 |
| 删除 sys_write | — | 远 | stdout 改为 SHM ring + sys_notify 零拷贝路径替代 pipe，完全绕过内核，libc 直接写 terminal。对应删除 sys_write/sys_read/sys_pipe/sys_close 等 pipe 相关 syscall。见 [libc.md](libc.md) |
| printf %f 浮点格式化 | 用户态 SSE/FPU | 中 | 需 FPU 上下文保存 + 浮点转换算法 |
| 动态库加载（.so 支持） | — | 远 | PIC + 动态链接器 + PLT/GOT |
| 运行时 IPI | LAPIC | 低 | reschedule / TLB shootdown |
| KMS PAT/write-combining | KMS | 低 | framebuffer 页映射加 PCD/PAT 标记 |
| KMS huge page 映射 | KMS | 低 | 减少 TLB miss |
| ~~MSI / MSI-X~~ | ~~APIC~~ | ~~低~~ | ✅ 已完成，PCIe 设备 MSI-X 中断，见 [xhci.md](xhci.md) |
| 用户态 SSE/FPU | 无 | 中 | lazy FPU restore，gcc 构建依赖 |
| FSINFO 空闲簇提示 | FAT32 | 低 | 加速空闲簇查找 |
| ~~多客户端 fs_driver 并发~~ | ~~fs_driver 事件循环~~ | ~~高~~ | ✅ 已完成，见 [file_system.md](file_system.md) |
| ~~fs_driver 事件循环~~ | — | ~~中~~ | ✅ 已完成，异步事件循环 + disk_io/pending_op |
| 回调链 → 协程迁移 | GCC ≥ 12 + 栈回溯支持 | 中 | 见 [file_system.md](file_system.md) 搁置项 |
| 多线程客户端 session 并发安全 | 多线程进程 | 中 | 需 per-fd 粒度锁，见 [file_system.md](file_system.md) 搁置项 |
| 磁盘队列优先级调度 | 多客户端延迟敏感 | 低 | 见 [file_system.md](file_system.md) 搁置项 |
| ~~VFS 层~~ | 无 | 低 | ✅ 已完成 — fd_table 统一 FD_FILE + 内核代理 IPC + fs_driver 纯 FAT32，设计见 [vfs.md](vfs.md) |
| ~~Unix domain socket~~ | — | 高 | ✅ 已完成 — AF_UNIX SOCK_STREAM + SCM_RIGHTS + poll，见 [socket.md](socket.md) |
| ~~SHM fd 化~~ | — | 高 | ✅ 已完成 — sys_shm_create 返回 fd + mmap(MAP_SHARED) 映射，见 [shm.md](shm.md) |
| 页面换出 (swap) | FAT32 write | 低 | BFC 不足时换出到磁盘 |
| 启动流程改造 | kbd 重构完成 | ✅ | init + FAT32 启动用户态服务，见 [boot.md](boot.md) |
| POSIX API 封装 | — | 中 | unistd.h/sys/wait.h/sys/mman.h 等，见 [sys_api.md](sys_api.md) |
