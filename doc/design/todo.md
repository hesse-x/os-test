# 项目路线图

## 项目目标

| 目标 | 阶段 | 验收标准 |
|------|------|----------|
| **支持简单 ELF 可执行文件的执行** | 短期 ✅ | 在宿主机编译 hello world，静态链接 libc.a，在 OS 上执行并输出字符 |
| **支持 Wayland 核心协议** | 中期 | 见下方 Wayland 验收标准 |
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

## virtio-gpu + DRM/KMS 迁移（阶段 1 完成）

内核侧设计见 [kernel/drm.md](kernel/drm.md) + [kernel/virtio.md](kernel/virtio.md) + [kernel/virtio_gpu.md](kernel/virtio_gpu.md)。

- [x] Phase 0: devtmpfs 子目录支持 + `common/drm.h`
- [x] Phase 1: virtio-pci modern transport + split virtqueue
- [x] Phase 2: virtio-gpu 命令层（CREATE_RESOURCE_2D / ATTACH_BACKING / TRANSFER_TO_HOST_2D / SET_SCANOUT / FLUSH）
- [x] Phase 3: DRM/KMS ioctl + `drm_test`
- [x] Phase 4: 删除 bochs-display + terminal 改造为 DRM backend + QEMU 配置 + 文档

## 阶段 2 待办（mesa 接入）

- [ ] GEM / PRIME fd 跨进程 buffer 共享
- [ ] atomic commit（替代当前 SETCRTC + PAGE_FLIP 的非原子序列）
- [ ] EDID 协商 + 多分辨率（当前硬编码 800×600@60）
- [ ] libdrm 二进制接入（当前用户态直连 DRM ioctl，未走 libdrm）
- [ ] `modetest`/`tinywl`（wlroots example compositor）验证

## 阶段 3 待办（Vulkan Wayland）

- [ ] syncobj / explicit fence
- [ ] atomic commit（完整）
- [ ] Venus（Vulkan over virtio-gpu）
- [ ] Wayland 合成器

## 中期目标 — Wayland 核心协议

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
- [ ] 验证: 图形终端启动，可输入命令、显示输出，外观类似 gnome-terminal

## 远期目标 — 支持构建 clang/LLVM

> **最低可演示目标：** 在 Xos 上通过 `make`/`ninja` 启动 clang 编译过程，cc1 能处理一个简单 C 文件输出 .o，ld 链接成 ELF。
>
> **完成态目标：** Xos 自举 — 在 Xos 上用 clang 编译 clang 源码自身。

### P0：绝对无法绕过

| 能力 | 依赖 | 说明 |
|------|------|------|
| **fork + execve** ✅ | kernel syscall | build system 必须 spawn 子进程 |
| **信号机制（基础）** ✅ | kernel syscall + libc | SIGCHLD、sigaction、kill、信号投递 |
| **FPU/SSE 上下文切换** | kernel context switch | clang 强制 SSE/SSE2，当前不保存 xmm；需 fxsave/fxrstor |
| **environ + getenv** | libc | 构建系统用环境变量传 PATH/CC/CFLAGS；需内核 environ 区 + execve 继承 |
| **/dev/null** | devtmpfs | 构建脚本大量使用 `>/dev/null 2>&1` |
| **磁盘空间** | mkdisk + FAT32 | LLVM ~500MB 源码 + ~5GB 产物；当前 64MB 需 ≥8GB；FAT32 4GB 单文件上限 |

### P1：快速构建必须

| 能力 | 依赖 | 说明 |
|------|------|------|
| **execvp / PATH 搜索** | libc | 构建系统需根据 PATH 搜索可执行文件 |
| **sys_mmap 文件映射** | kernel mmap | LLVM 大量使用 mmap 文件映射，当前只支持匿名映射 |
| **pthread (基本)** | kernel + libc | ninja 需并行构建；需 clone syscall + TLS（FS_BASE） |
| **getrlimit/setrlimit** | kernel syscall | 构建工具检查资源限制 |
| ~~**waitpid WNOHANG**~~ | ~~kernel proc~~ | ~~非阻塞等待子进程~~ | 已实现：`options` 三层打通（libc inline `__syscall3` + wrapper 转发 + 内核读 `options`），两条等待路径按 `WNOHANG` 短路返回 0；`wait.h` 补齐 `WIFEXITED` 等宏。停止态上报（`WUNTRACED`/`WCONTINUED`）见 [proc.md](kernel/proc.md) |

### P2：clang 运行时需要

| 能力 | 依赖 | 说明 |
|------|------|------|
| **termios** | libc/驱动 | tcsetattr/tcgetattr，部分构建脚本/shell 需要 |
| **symlink** | FAT32 | FAT32 不支持，需考虑其他 FS |
| **/dev/zero** | devtmpfs | 一些工具需要 |
| **Shebang `#!` 支持** | sys_exec | 脚本执行（`./configure` 等） |
| **sigprocmask** | kernel signal | 信号阻塞掩码 |
| **setpgid/getpgid** | kernel proc | 进程组，作业控制 |
| **alarm/setitimer** | kernel timer | 超时控制 |

### 里程碑路径

```
sprint 1-2: POSIX 扩展波（libc 完善 + lseek + dirent + unlink/fstat 等）
              ↓
sprint 3-4: fork + execve + 信号机制（基础） ✅
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

## VFS i_op 重构 ✅（R1–R5）

四 fs i_op 就绪、path_walk 逐组件遍历、硬编码消除、fstype 仅保留 mount_root/getdents。设计见 [kernel/vfs.md](kernel/vfs.md) i_op 层节。

- [x] R1: `struct inode_operations` 定义 + 四 fs i_op 表 + mount_root iget 出口
- [x] R2: fat32 回归 + sys_open/sys_stat/sys_truncate 等 i_op 分发
- [x] R3: sysfs/devtmpfs 回归 + devtmpfs ino 唯一化（next_dev_ino++）
- [x] R4: `test_vfs_dispatch` + `test_inode_refcount` 测试 ELF
- [x] R5-前置: i_op NULL errno 修正（EINVAL→EPERM, ENOSYS→EPERM，对齐 Linux）
- [x] R5: tmpfs i_op 实现（`tmpfs_dir_iop`/`tmpfs_file_iop` 全套 + /run 挂载 + AF_UNIX bind 落 VFS socket inode + sys_mknod），见 [kernel/vfs.md](kernel/vfs.md) tmpfs / AF_UNIX socket inode 节

### i_op NULL errno 修正（R5 前置，对齐 Linux）

当前 `kernel/bsd/vfs.c` 中 i_op NULL 回调返回的 errno 与 Linux 不一致：

| 回调 | 当前 errno | Linux errno | Linux 源码 | 位置 |
|------|-----------|-------------|-----------|------|
| setattr NULL | EINVAL | **EPERM** | `notify_change()` | vfs.c:238,376 |
| mkdir NULL | ENOSYS | **EPERM** | `vfs_mkdir()` | vfs.c:443 |
| unlink NULL | ENOSYS | **EPERM** | `vfs_unlink()` | vfs.c:472 |
| rmdir NULL | ENOSYS | **EPERM** | `vfs_rmdir()` | vfs.c:501 |
| create NULL | EACCES | EACCES | `vfs_create()` | vfs.c:254 ✓ |

ENOSYS 表示"syscall 内核层不存在"，EPERM 表示"syscall 存在但此 fs 不支持该操作"。用户态程序（如 glibc）会据此做完全不同的回退策略。修正为 EPERM 对齐 Linux 语义。

## 键盘 repeat 机制

- [ ] evdev 层实现键盘 repeat：记录最后按下键 + 时间戳，用软定时器（timerfd 或 sys_gettime 轮询）在按住超过延迟后周期性重发 KEY_DOWN
- [ ] 验证: 长按一个键持续输出字符（对齐 Linux EV_REP 行为）

## 已知 Bug 与技术债务

| # | 问题 | 位置 | 说明 | 归属文档 |
|---|------|------|------|----------|
| 9 | 进程创建失败内存泄漏 | kernel/proc.cc | process_create_elf 后续 alloc_page 失败时，前面已分配页面未释放 | [mem.md](mem.md) |
| 16 | 用户栈仅 4KB 无 guard page | kernel/proc.cc | 栈溢出触发 #PF 被 kill | [proc.md](proc.md) |
| 17 | 内核栈仅 8KB | kernel/proc.cc | 深层调用路径偏紧 | [proc.md](proc.md) |
| ~~21~~ | ~~pid 未校验上界~~ | ~~kernel/trap.cc~~ | 已修复：`task_get(pid)` helper 编码 `pid >= 0 && pid < MAX_PROC` 守卫到运行时（debug ASSERT / release 零开销），所有 `tasks[pid]` 访问点统一走该 helper | [proc.md](proc.md) |
| 22 | malloc free 不验证指针合法性 | user/lib/malloc.cc | 任意指针损坏空闲链表 | [mem.md](mem.md) |
| 25 | 缺少 -mcmodel=kernel | CMakeLists.txt | C 用 -fno-pie 绝对寻址错误 | [cmake.md](cmake.md) |
| 26 | 链接脚本缺 section 对齐 | build_script/linker.ld | 无法设置不同页权限 | [cmake.md](cmake.md) |
| 28 | boot_info 复制硬编码 128B | arch/x64/start.S | 结构体扩展会截断 | [uefi.md](uefi.md) |
| 29 | evdev ioctl 大 getter 数据截断(B3) | kernel/bsd/syscall.c FD_DEV req 段 | 客户端回填守卫硬限 `_IOC_SIZE<=48`、`req_reply_len=56`,完整 KEY 位图 96B 等 >48B 查询会静默截断。本轮 B1(test 传 ≤48B buf)够桩用,**接 libinput 前必须做 B3**:ioctl 段按 `_IOC_SIZE>48` 分流到 `sys_msg_to`/`sys_msg_resp` 变长通道,复用已有 MSG 机制。**evdev broker 重构落地后由 broker 转发路径（既有 RECV_IOCTL 变长通道）自动消解，见 [../../refact_evdev.md](../../refact_evdev.md) §6.3 + [../../plan_reface_evdev.md](../../plan_reface_evdev.md) Stage D** | [../../evdev_design.md](../../evdev_design.md) |
| 31 | evdev grab 僵尸锁 | kernel/bsd/proc.c `file_put` FD_DEV 段 | `file_put` 对 user-driver(`driver_pid>0`)不调 close,客户端崩溃/关 fd 后 `grab_client` 残留 → eventN 永久 EBUSY。FD_FILE 已有 `kernel_msg_send` close 通知范式(proc.c:220)可复用:给 user-driver close 时 `kernel_msg_send(driver, &close_req, ...)` → evdev 收 RECV_MSG 释放 grab。但改全局 close 语义(所有 user-driver 的 close 阻塞等驱动)作用域溢出 evdev 查询接口本轮,留接真实多客户端前。**evdev broker 重构落地后 grab 下沉 evdev、consumer `f_op->close` 自动清理 client，僵尸锁不复存在，见 [../../refact_evdev.md](../../refact_evdev.md) §7.1 + [../../plan_reface_evdev.md](../../plan_reface_evdev.md) Stage C** | [../../evdev_design.md](../../evdev_design.md) |
| ~~32~~ | ~~VFS 路径解析为字符串前缀匹配,非逐组件遍历~~ | ~~kernel/bsd/vfs.c~~ | 已修复：实现 `path_walk`/`path_walk_parent` 逐组件遍历（`inode->i_op->lookup`），无 symlink 场景与 Linux 等价。引入 symlink 后需加 `follow_symlink` + `follow_dotdot` 穿越挂载边界。见 [vfs.md](kernel/vfs.md) i_op 层节 | [mount.md](kernel/mount.md) |
| 33 | pipe read_pid/write_pid 滞后引用致 wake_process ASSERT panic | kernel/bsd/syscall.c `pipe_write`/`pipe_read` | 读/写端 `schedule()` 返回后才清 `read_pid`/`write_pid = -1`,目标线程可能已切换到 `WAIT_FUTEX` 等非 PIPE 等待状态;另一核 `pipe_write`/`pipe_read` 仍看到旧 pid,调 `wake_process` 触发 `ev==WAIT_PIPE` ASSERT( ipc.c:648)。多核偶发。**修复**:`wake_process(p->read_pid)` → `wake_with_event(task_get(p->read_pid), WAIT_PIPE)`,内部持 `scheduler_lock` 校验 `wait_event==expected`,不匹配 no-op。write 端同理。 | [ipc.md](ipc.md) |
| 34 | evdev HID 中断投递借 sys_recv 的 WAIT_RECV/EINTR 通道(双源共占) | kernel/driver/xhci.c `xhci_isr` `wake_process(kbd_openers[])` + user/driver/evdev.cc `recv` 主循环 | evdev 进程单一 `WAIT_RECV` 等待点被两源共用:硬件 HID 中断(内核 ISR `wake_process` 伪造 EINTR)+ 下游 IPC(`sys_notify`/`sys_req` 真消息)。与 bug.md 的 pid5 双身份结构同构,仅因 evdev 专用 driver 未撞车。`wake_process` 是唯一 ISR 安全的唤醒(`sys_notify` 不能在 ISR 调:裸 `recv_lock` 非 irqsave + 读 `current_task->pid` 做 src)。**正规化走路径 3**:引入 irqfd(eventfd + ISR 安全 `eventfd_signal_isr`)+ ipcfd(evdev 下游 IPC 定制 socket fd,自带 read 出队)+ evdev 主循环改 epoll,两源物理分离。落地后 `recv_intr`/`kbd_openers[]` 即可删除;`wake_process` 函数删除需 **路径 3 + design0.6**(pipe `pty.h:202/208`)都完成。design0 唤醒收敛阶段保留 `wake_process` 仅供 evdev。完整方案见 [../../evdev_refact.md](../../evdev_refact.md) | [../../evdev_refact.md](../../evdev_refact.md) |
| 35 | `xtask` 无通用引用计数 | kernel/xcore/sched.c `xtask` | 本内核 `xtask` 是 `MAX_PROC` 静态表 + pid 判活(reap 时 `pid=-1`,`sched.c:721`),无 `get_task_struct`/`put_task_struct` 式 refcount。ipcfd(路径 3)暂用 pid + `task_get` 存活校验达成"fd 不 UAF"的 Linux 同款不变量。长远对齐 Linux(fd 持 task 引用)需给 `xtask` 加 refcount + 改 `sched_task_reap`/`schedule` 时序——属调度核心重构,独立立项,不在路径 3 内。详见 [../../evdev_refact.md](../../evdev_refact.md) §9 | [../../evdev_refact.md](../../evdev_refact.md) |
| 36 | fd close 不自动从 epoll 摘除 | kernel/bsd/proc.c `file_put` + kernel/bsd/types.h `struct file` | 本内核 `file_put`(`proc.c:180`)无 epoll 摘除钩子、`struct file`(`types.h:64`)无 `f_ep_links`,fd 关闭不自动从挂它的 epfd 注销(Linux 有 `file_close` 遍历 `f_ep_links` 调 `ep_remove`)。evdev(路径 3)靠"不主动 close + `ep_insert:144` 的 `file_get` 防 UAF"过渡。根治应补 `f_ep_links` + `file_put` 遍历 `ep_remove`,对所有 epoll 用户(含 terminal)受益——属 design0.5 的 eventpoll.c 重写范围,挂 design0.5。详见 [../../evdev_refact.md](../../evdev_refact.md) §9 | [../../evdev_refact.md](../../evdev_refact.md) |
| 37 | 通用 `irqfd_bind` syscall(概率极低) | kernel/driver/xhci.c `usb_hid_ops.ioctl` `HID_BIND_IRQFD` | 路径 3 用设备 ioctl `HID_BIND_IRQFD` 绑 irqfd(绑设备用设备 ioctl 是 Linux 形状,非 KVM_IRQFD syscall)。仅当出现第二个**独立用户态中断驱动进程**(非 evdev)时,把绑定逻辑抽成通用 `irqfd_bind(dev_fd, irqfd_fd)`。当前单消费者(evdev,且多 HID 设备 kbd/mouse/gamepad/touchpad 共用同一 ISR+irqfd),无需抽通用层(YAGNI)。详见 [../../evdev_refact.md](../../evdev_refact.md) §9 | [../../evdev_refact.md](../../evdev_refact.md) |
| 38 | SA_RESTART 未实现(S02 缓做) | kernel/bsd/signal.c `check_pending_signals` + arch/x64/trap.h `trapframe` | 慢调用被信号中断后不自动重启(全返 EINTR)。S02 设计已豁免缓做:trapframe(arch/x64/trap.h:26-39)无 `orig_rax` 字段,加它会扰动 `__alltraps`/`__trapret`/`switch_to` 偏移,回归风险高。落地需:trapframe 加 `orig_rax` + 慢调用(sys_waitpid/sys_read/sys_write/sys_recvmsg/sys_accept/sys_poll/sys_pause/futex)返 `-ERESTARTSYS` + `check_pending_signals` 末尾按 `SA_RESTART` 决策 `tf->rip-=2; tf->rax=orig_rax`(重启)或转 `-EINTR`。ERESTARTSYS 不得泄漏成用户 errno。见 [../../refact_syscall/S02_death_encoding_sa_flags_restorer.md](../../refact_syscall/S02_death_encoding_sa_flags_restorer.md) §4 | [kernel/ipc.md](kernel/ipc.md) |
| 39 | SA_NOCLDWAIT 未实现(S02 缓做) | kernel/bsd/syscall.c `do_exit_with_code`(SIGCHLD 通知段,~:208-219) | 父进程装了 SA_NOCLDWAIT 时,子退出仍通知 SIGCHLD 并留 zombie(应自动 reap 不留 zombie)。S02 设计已豁免近似("不自动 reap,留 zombie,记 todo")。落地需:`do_exit` 通知父 SIGCHLD 前查 `parent->signal->action[SIGCHLD].sa_flags & SA_NOCLDWAIT`,置位则跳过 SIGCHLD + 直接 reap(不进 zombie)。父仍可 wait4(但无 zombie 可收,语义近似 Linux)。见 [../../refact_syscall/S02_death_encoding_sa_flags_restorer.md](../../refact_syscall/S02_death_encoding_sa_flags_restorer.md) §5 | [kernel/ipc.md](kernel/ipc.md) |
| 40 | CLONE_VFORK 父阻塞语义未实现(降级 no-block) | kernel/bsd/proc.c `sys_clone` | S07 修正了 `CLONE_PARENT`/`NEWUTS`/`NEWIPC` 与 Linux 规范值的冲突(原 0x80/0x08000000/0x10000000 互相占位),新增 `CLONE_NEWTIME` 入拒绝掩码,实现 `CLONE_CLEAR_SIGHAND`(子进程信号 handler 复位 SIG_DFL,与 `CLONE_SIGHAND` 互斥),并把 `CLONE_PTRACE`/`CLONE_SYSVSEM` 从静默忽略改为 `-EINVAL`(无 ptrace/SysV IPC 子系统,拒绝而非撒谎)。`CLONE_VFORK`(0x4000)接受但**降级为父不阻塞**:本内核无 completion 原语,v1 不实现父挂起直到子 exec/exit。落地需 xtask 增 vfork 完成通道 + `sys_execve`/`do_exit_with_code` 唤醒点;补 `SYS_VFORK`(号 58=`clone(VFORK|VM|SIGCHLD)`)待 vfork 同步完整后做。见 [../../refact_syscall/07_clone_remaining_flags.md](../../refact_syscall/07_clone_remaining_flags.md) | [posix.md](posix.md) |
| ~~41~~ | ~~fork/clone 路径未统一 do_fork~~ ✅已消除 | kernel/bsd/proc.c `sys_fork`(:910-934) | `sys_fork` 已委派 `sys_clone(SIGCHLD,0,0,0,0)`,两路径合一,fork 自动复用 clone 的 COW/fd/signal/identity 填充及 deferred_files RCU 死锁修复。未抽独立 `do_fork` helper —— 委派即等价统一,且无重复代码。核实等价性:`stack==0` 无 CLONE_VM 走父 trapframe rsp;`exit_signal=SIGCHLD`;`clear_tid_addr=NULL`(无 CLONE_CHILD_CLEARTID,与 Linux 一致:fork 子不继承父 clear 点)。唯一 delta:CPU 选配从亲和父核变全局最闲(仅放置启发式,非正确性/ABI)。见 [../../refact_syscall/03_fork_settid_cleartid.md](../../refact_syscall/03_fork_settid_cleartid.md) 方案 B | [posix.md](posix.md) |
| 42 | tmpfs/devtmpfs/sysfs 不可 exec + sys_read 硬编 fat32_read | kernel/bsd/vfs.c `vfs_read_kernel` + kernel/bsd/syscall.c `sys_read` FD_REGULAR | S19 §7 新增 `vfs_read_kernel` 内核态 inode-read helper 并改 execve 两处 `fat32_read` 调用;tmpfs 降级返 `-ENOEXEC`(tmpfs inode-read 需从 `tmpfs_inode_info->data` 读,见 tmpfs.c:561 `tmpfs_read`)。另 `sys_read` 的 `FD_REGULAR` 分支(syscall.c)仍硬编 `fat32_read`,改它需同时给 fat32 加 `f_op` 或走 `vfs_read_kernel` 分发,范围另议。记此为"tmpfs 二进制可 exec + sys_read 通用化"长期项。见 [../../refact_syscall/S19_process_cluster.md](../../refact_syscall/S19_process_cluster.md) §7 | [vfs.md](kernel/vfs.md) |
| 43 | rusage 不区分 user/sys time | kernel/bsd/syscall.c `fill_rusage` | S19 §5.2 把子 `cpu_time_ns` 全填 `ru_utime`,`ru_stime=0`。本 OS 无 tick accounting(user/sys time 采样),需 `account_*_time` 风格采样才能区分。记此为长期项。见 [../../refact_syscall/S19_process_cluster.md](../../refact_syscall/S19_process_cluster.md) §5 | [posix.md](posix.md) |

各 Bug 详细说明见归属文档的待完成项。已修复的 Bug（#30 socket 锁粒度）不再列出。

## udev / udevd 进度

udevd 设备数据库 + 规则引擎 + coldplug **已落地**（对齐 Linux `input_id` builtin + `udevadm trigger`）：udevd 维护 `/run/udev/data/<key>` db（tmp+rename 原子写，依赖 `SYS_RENAME`(98) + tmpfs rename），规则引擎 `input_id_compute` 开 /dev 跑 `EVIOCGBIT` 合成键盘类 `ID_INPUT_*`；sysfs `uevent` 属性可写，coldplug 写 `add` 重广播走 netlink；init settled gate 保证 db 就绪再起 terminal。详见 [udev.md](udev.md)。

延后项（均记 [udev.md](udev.md) 待完成项）：
- B6 ID_INPUT_MOUSE/TOUCHPAD/SEAT 全类——依赖 evdev 真实多设备 caps，做合成器（Wayland）时连同真实多输入设备一起做
- shim 枚举路径对齐 db（`device_is_keyboard` 改走 db 查 property）、uevent 可读 + 接受 remove/change、coldplug 扫 /sys/devices 全树

> monitor 管道**已落地**（commit `221c373`）：shim `udev_monitor_*` 全真实（`enable_receiving` AF_UNIX connect+SCM_RIGHTS 收 pipe rd fd / `get_fd` / `receive_device` 解析 KV），唯一 stub=`filter_add_match_subsystem_devtype`。udevd 完整 daemon（accept→pipe→SCM_RIGHTS 握手 + device 补全 + 广播 + coldplug + settled 门）。终端消费 monitor + crash 重连见根目录 `use_udev_design.md`。

延后项（终端 crash 自愈增强）：
- terminal respawn 门控（use_udev_design.md §3.4 路径C 备选）：当前 init 收尸循环只 respawn udevd，**不 respawn terminal**（`init.c:138-165` 只比对 `udevd_pid`）。故 U3 终端 monitor 重连耗尽后取 degraded hold（保 pty/shell 活、输入断、5s 低频探活自愈）而非 exit→respawn。若将来要"自愈到全新可用终端"，扩 init 给 terminal 加与 udevd 同款 `crash_count` respawn + burst 上限 + **仅当 udevd 存活时才 respawn terminal** 的门控（避免 udevd 过 `START_LIMIT_BURST=5` 后 terminal respawn 风暴）。非 use_udev 本轮范围。

延后项（use_udev U4 测试收尾，**极低优先级，非验收必需**——基建已全落地，terminal 切 udev 后端 + monitor 重连已可手动验收，下列仅为测试补全，都差不多了不如顺手做完）：
- test_udevd 热插拔 add 测点（`plan_use_udev.md` U4-T2）：桩设备走两步注册（`device_register_shm`+`device_set_meta` 触发 `nl_uevent_broadcast("add")`，`devtmpfs.c:776`，对齐 `test_sysfs.c:register_event1()`）→ udevd → client pipe，验证 monitor 端到端（非 coldplug 快照路径）。落地即 +1 测点（5→6）。
- test_udevd crash respawn 重连测点（`plan_use_udev.md` U4 推迟项）：**前置基建**——procfs 已上线（[procfs.md](procfs.md)），可扫 `/proc/[pid]/comm` 找 udevd（pid 发现不再缺）；但 `udevd_pid` 仅存 `init.c:116` 栈变量、netlink 广播 `nlmsg_pid=0` 的现状未变，`test_udevd` 仍难直接发现其 pid。落地可走 procfs 扫 pid+comm 定位 udevd（替代 pid 文件），或保留 `init.c` spawn udevd 后写 `/run/udev/udevd.pid`（respawn 刷新）方案。crash 重连本身已由 U3 终端 spin→`suspend`/`resume` 人工驱动验收（use_udev_design.md §4.2 层2）。

## 内核限制与已知缺陷（静默截断 / 固定上限类）

均为既有现状，非任一在制方案引入，但会阻塞大缓冲/大设备场景。按修复成本排序：

- `devtmpfs_getdents` 不支持分批续传（`kernel/bsd/devtmpfs.c:556`）：`ctx->pos != 0` 即返 0（`:559`），且末尾无条件 `ctx->pos = (uint64_t)-1` 标 EOF（`:642`）。`dir_emit` buffer 满 break 时直落 EOF 覆盖，故"装得下则一次返回全部、装不下则静默丢失剩余条目"。libc `readdir` 用 4096 buffer 分批拉（`user/lib/file.cc:626`），`/dev` 条目总 reclen 超 4096 即漏设备。**根因**：pos 当 EOF 标记而非"上次发到第几个节点"的游标。**修复**：pos 改节点序号游标，emit 成功 pos++、buffer 满则 break 存回 pos、下次从 pos 续传。**障碍**：root `/dev` 是两段遍历（先 `dir_list` 发子目录 `:588`、再 `dev_list` 发顶层设备 `:598`），跨段续传需统一编号（dir 段 [0,dir_count) + dev 段 [dir_count,...)），但 dev_count/dir_count 已随 devtmpfs 动态化删除——改用节点指针当 pos 或独立段计数。work2_design §3.6 明令 getdents 零改动，此为独立改进。发现于 devtmpfs 动态化 `test_dev_vfs_dynamic`（90 条 ≈ 3600 字节逼近 4096 边界）。
- `sys_read`/`sys_write` 单次上限 65536（`kernel/bsd/syscall.c:1306` read / `:960` write）：`if (len > 65536) len = 65536;` 静默截断，用户态不检查返回值会以为读/写全。阻塞大 uevent、大 props、大文件单次 I/O 场景。**修复**：移除截断改为循环 I/O 或按调用方需求放宽（对齐 Linux 无单次上限）。注意 socket/msg 已有同源 64KB 限制（`kernel/bsd/socket.h:17 MAX_SOCKET_DATA`、`kernel/xcore/ipc.c:844/:928`），属 IPC 层独立约束，不与此处 read/write 同步改。
- fd 表固定上限 `MAX_FD`（`kernel/bsd/types.h:26` `#define MAX_FD 1024`，进程 `files->fd_table[MAX_FD]` 固定数组 `:114`）：S20 由 128 提升到 1024（对齐 Linux `RLIMIT_NOFILE` soft 默认），`struct files` 增到 ~8KB/进程（堆分配，`files_create` kmalloc），proc.c 三处 fd-collect-then-put 快照（`proc_reap`/`files_put`/execve cloexec）改 `kmalloc` 避免撑爆 16KB 内核栈。仍为固定上限——fd 数超 1024 即 `EMFILE`，无法运行时扩张。对齐 Linux 动态 fd 表（`files_struct` + `fdtable` 可扩容 `krealloc` + `RLIMIT_NOFILE` 软硬限）作为独立改进。**修复**：fd_table 改 `krealloc` 动态数组 + 容量翻倍（tmpfs `ti->data`/`ti->cap` 模式可参考，`kernel/bsd/tmpfs.c:161`）。与 devtmpfs 动态化同类（静态数组→动态），但属进程 fd 子系统独立改进。见 [../../refact_syscall/S20_misc_finish.md](../../refact_syscall/S20_misc_finish.md) §5

### S20 杂项收尾延后项

S20（[../../refact_syscall/S20_misc_finish.md](../../refact_syscall/S20_misc_finish.md)）以最小代价对齐 Linux x86-64 语义；下列属"需要更大子系统才能真实生效"的项，S20 仅接常量/退化语义或显式拒绝，真实语义延后：

- **mount 权限校验**（`kernel/bsd/mount.c` `sys_mount`）：已加 `capable(CAP_SYS_ADMIN)` 门（今天等价 `euid==0`，与 `kill_permitted`/setuid 阶梯同一收口）。单用户 root-default 系统下为 no-op，仅当未来引入非 root euid 时生效；引入真实 capability 模型后只改 `capable()` 实现。`MS_RDONLY`/`MS_NOSUID`/`MS_NODEV`/`MS_NOEXEC` 已存入 `mount_entry.m_flags`——`MS_NOSUID` 被 execve 消费（跳过 setuid/setgid 位，见 [proc.md](kernel/proc.md) execve 段），其余三 flag 存而未强制。`MS_REMOUNT`/`MS_BIND` 显式 `-ENOSYS`（未实现，优于静默吞）。`data`(arg5) 透传未做——无 fstype 消费，等真实 mount 选项需求出现时连同 `mount_root` 回调签名一起加。
- **`MS_RDONLY`/`NODEV`/`NOEXEC` 真实生效**：本 FS（FAT32/tmpfs/devtmpfs）无权限位与执行位语义，三 flag 存 `m_flags` 但不强制（`MS_NOSUID` 已生效，见上）。需权限 FS + 执行位模型。
- **`MAP_GROWSDOWN` 栈自动扩展**（`include/uapi/xos/mman.h`）：S20 补常量 `0x100`，接受为 no-op（本 OS 栈固定）。真实 growsdown 栈扩展需 fault handler 在栈 guard 页 fault 时向下扩展 VMA，属 S10 范围。
- **`PROT_GROWSDOWN` 栈 guard 扩展**（`kernel/bsd/syscall.c` `sys_mprotect`）：S20 接受该位（mask 掉不报错），但无栈自动扩展语义。同 `MAP_GROWSDOWN`，属 S10。
- **sparse 文件真实 hole 跟踪**（`kernel/bsd/syscall.c` `sys_lseek` `SEEK_DATA`/`SEEK_HOLE`）：S20 补 `SEEK_DATA 3`/`SEEK_HOLE 4`，退化到 Linux "无洞文件"语义（全文件=data，尾部=hole，`offset>=size` 返 `ENXIO`）。本 FS 无 sparse/punch，真实 hole 报告随 sparse FS。
- **`statfs`/`fstatfs` 容量字段**（`SYS_STATFS 137`/`SYS_FSTATFS 138`，`kernel/bsd/syscall.c` `sys_statfs`/`sys_fstatfs`）：已实现，但 `f_blocks`/`f_bfree`/`f_bavail`/`f_files`/`f_ffree` 返回 0——FAT32 不维护 free-cluster 计数器，且 llvm-libc 的 `pathconf()` 只读 `f_type`/`f_bsize`/`f_frsize`/`f_namelen` 四字段（本任务仅为打通 llvm-libc unistd/time 模块）。`f_type` 按 mount fstype 名选 magic：fat32→`MSDOS_SUPER_MAGIC`（诚实反映 FAT 不支持符号链接，与 `_PC_2_SYMLINKS=0` 一致）、tmpfs/devtmpfs→`TMPFS_MAGIC`、sysfs→`SYSFS_MAGIC`。**修复**：未来 df/磁盘工具需要真实容量时，给 fat32 加 `fat32_total_clusters()`（导出 `static total_data_clusters`）+ `fat32_free_clusters()`（新增 `used_clusters` 计数器在 `fat32_allocate_cluster`/`fat32_free_chain` 维护，或慢 FAT 扫描），再在 `fill_statfs` 填入 `f_blocks`/`f_bfree`/`f_bavail`。
- **`CLONE_VFORK` 父阻塞语义**：见技术债 #40（S19 已记），需 wait 通道。

## 调度与唤醒子系统重构

bug.md 的 virtio-gpu 卡死根因(跨源误唤醒 → run_node 二次 enqueue → 环损坏)经审查定位为**唤醒模型抽象问题**,非 schedule 模型问题。拆为两阶段:

- **design0 — 唤醒模型收敛 + wait_event 语义降级**(前置,解 bug 根因):把 socket/pty/netlink/virtio-gpu 的**资源唤醒**从 pid-targeted `wake_process(pid)` 收敛到 wq 队列身份制(`add_wait_queue` + `__wake_up`);`wait_event` 降级为调试标签。不碰调度核心。详见 [../../schedule_refact_design0.md](../../schedule_refact_design0.md)。
- **design1 — 调度锁模型 + run_queue 不变式**(后置,依赖 design0):`run_node` 单一归属的结构化保证(入队前断言 `list_empty`、封装 push/pop)、`scheduler_lock` 边界与锁序定稿、幂等防御降级为 debug 断言。详见 [../../schedule_refact_design1.md](../../schedule_refact_design1.md)。

evdev 中断投递正规化(技术债 #34)独立于本重构,走路径 3,见 [../../evdev_refact.md](../../evdev_refact.md)。现已落地:`wake_process`/`recv_intr`/`kbd_openers[]` 彻底删除,design1 锁序总表的 evdev 例外行撤销,唤醒模型完全统一到 wq 队列身份制。

## Sprint 1-2: POSIX 扩展波

详细计划见 [posix.md](posix.md)。

- [ ] syscall 清理：重排编号 + DEV_MSG 移除 + BLOCK_READ/WRITE 合并
- [ ] libc: sprintf/snprintf/strtol/ctype/assert/getcwd/sleep/uname
- [ ] libc: fopen/fclose/fread/fwrite/fseek/ftell/rewind
- [ ] libc: opendir/readdir/closedir + struct stat 扩展
- [ ] libc: lseek/mkdir/unlink/rmdir/access/isatty
- [ ] libc: memcmp/strstr/strtok/strtok_r/strerror/qsort/rand/abs
- [ ] `open()` mode 管线接通：`O_CREAT` 时第三个 `mode_t` 参数当前三层全断——wrapper `user/lib/file.cc` 不取 va_arg、libc inline `sys_open` 用 `__syscall2`、内核 `sys_open` 第 3 参命名 `_u1` 忽略。FAT32 无权限位故 mode 本就无意义（与 `mkdir` 同样 `(void)mode`），属合理技术妥协；未来支持权限的 FS 上线时三层接通（wrapper 取 va_arg → inline 改 `__syscall3` 传 mode → 内核用 arg3）。详见 [vfs.md](kernel/vfs.md) 待完成项
- [x] **O_DIRECTORY 强制**（已落地，`kernel/bsd/vfs.c`）：`sys_open` 与 `sys_openat` 相对路径分支在 EISDIR 门控后、SOCKET/ENXIO 前补 `O_DIRECTORY` 校验——非目录返 `-ENOTDIR`（对齐 Linux 原子语义）。`O_CREAT|O_DIRECTORY` 新建的普通文件 `type==INODE_REGULAR` 命中 `!= INODE_DIR` → ENOTDIR，符合 Linux。**未覆盖**：① `sys_open` 对 devtmpfs 设备文件的委派分支（`vfs.c:356-359` 在校验前 `return devtmpfs_open(...)`）——`/dev` 下设备节点 `INODE_DEV` 用 O_DIRECTORY 理论应 ENOTDIR，本内核不拦截（场景极少，留作后续微调）；② `O_NOFOLLOW`/`O_DIRECT` 维持 no-op（无 symlink 子系统、无 direct-IO 旁路路径，`mount.c:179` 明注无 symlink 处理，全树 0 引用），待对应子系统上线再激活。测试 `test_openat_dirfd.c::test_open_odirectory`。
- [ ] link/symlink/readlink：FAT32 不支持硬链接与符号链接。三条路：① 暂报 `ENOSYS`/`EPERM`（推荐短期，最干净，不污染路径解析）；② 伪符号链接——用 Windows 式 `.SYMLINK` 伪文件存目标路径，`lstat` 识别 `S_IFLNK`，路径解析时跟随（工作量中等，污染纯路径解析，违背 FAT 语义）；③ 换文件系统（ext2/TAR 等，长期）。`lstat` 在无 symlink 前提下语义等价 `stat`，可直接别名。详见 [vfs.md](kernel/vfs.md) 待完成项

### chmod/chown + capable() 收口后续

`chmod/fchmod/fchmodat/chown/fchown/fchownat` 6 个 syscall 已填实（落盘仅内存，setuid 位清除规则见 [vfs.md](kernel/vfs.md)），`capable()` 单一收口已落地（今天等价 euid==0，CAP_* 编号对齐 Linux，见 `include/uapi/xos/capability.h`）。setuid/setgid 凭证阶梯 6 个函数（setuid/setgid/setresuid/setresgid/setreuid/setregid）的特权 gate 统一经 `capable(CAP_SETUID/CAP_SETGID)`（对齐 Linux setuid(2) 判 CAP_SETUID，非裸 euid==0）；未来 bitmap 实化只改 `capable()`。下一步：

- [x] **execve S_ISUID/S_ISGID 处理（sudo 真正使能点）**：已实现（`kernel/bsd/proc.c` sys_execve）。close 前捕获 `ip->mode/uid/gid` + `mount_of_inode(ip)->m_flags`，ELF 校验后**预计算** new_euid/egid（不写 proc，对齐 Linux prepare_binfmt），auxv 的 `AT_EUID/AT_EGID/AT_SECURE` 用新值构建（AT_SECURE 非零 → musl 进 secure-execution 模式 drop LD_*），point-of-no-return 后才**提交**（commit_creds，对齐 Linux，零失败路径凭证泄露窗口）。`S_ISREG && (mode & S_ISUID)` → euid/suid=inode->uid；S_ISGID → egid/sgid=inode->gid；`MS_NOSUID` 跳过。real uid/gid 保持调用者（setuid 程序能 setuid(getuid()) 永久 drop 特权的前提）。测试 `test_setuid_exec`（helper ELF 复制到 tmpfs /run 使 path chmod 持久，再 drop root execve 断 euid 切换）。配套扩了 `vfs_read_kernel` 支持 tmpfs（见下）。
- [x] **`vfs_read_kernel` 跨 fs（execve 不再绑死 fat32）**：`kernel/bsd/vfs.c` 按 `ip->mount->fs->name` 分发——tmpfs→`tmpfs_read_kern`（`kernel/bsd/tmpfs.c`，__memcpy 到内核 buf），其余→`fat32_read`。解锁 execve 从 tmpfs 执行（此前 tmpfs 普通文件只能经 f_op 的 `sys_read`，execve 的内核态读路径读不了）。`MS_NOSUID` 亦因此可达测试。
- [ ] **capability bitmap 实化**：`capable()` 现仅判 euid==0（`(void)cap` 预留分流钩子）。需按 cap 分流时给 proc 加 `cap_effective/inheritable/permitted` bitmap（届时更新 `proc.h` 的 `STATIC_ASSERT(sizeof(proc)==520)` + `kernel/driver/bsd_types.h` 镜像）+ secbits（SECBIT_NOROOT 等）。各调用点（inode_permission/kill_permitted/sys_mount/clock_settime/chmod/chown/setuid 阶梯）届时零改动，只改 `capable()` 实现。注意：bitmap 落地后 setgid 阶梯的 `CAP_SETGID` 应独立于 euid（今天 capable 只看 euid 是粗糙近似；bitmap 真实化后 CAP_SETGID 与 euid 解耦）。
- [ ] **inode_permission 拆 CAP_DAC_OVERRIDE vs CAP_DAC_READ_SEARCH**：现 root 的 R/W/X 全走 `CAP_DAC_OVERRIDE`。Linux 拆分：W/X 走 DAC_OVERRIDE，R 走 DAC_READ_SEARCH（目录 +x 也走后者）。本轮统一不拆（零行为变化），拆分留此。
- [ ] **chown 复杂规则**：现 chown 简化为 `capable(CAP_CHOWN)`（root-only）。Linux 允许"属主把文件 chown 到自己所在的 group"（不需 CAP_CHOWN）。引入多 group 概念后放宽到此规则；届时 `apply_chown` 的非特权清 setuid 位路径才有真实触发点（本轮 chown root-only，非 root 得 EPERM 不执行清位，故 `test_chown_root_keeps_setuid` 验证的是 root CAP_FSETID 保留语义）。

### 剩余未迁移接口（按阻断原因分组）                                                                                                                                                                                                        
                                                                                                                                                                                                                                            
| 分组 | 接口 | 阻断原因 | 解锁路径 |                                                                                                                                                                                                       
|------|------|----------|----------|                                                                                                                                                                                                       
| **[已落地 2026-07-28]** M0.4 内核 `*at` AT_FDCWD→cwd 修 | `chdir`/`getcwd`/`renameat`/`unlinkat`/`openat`/`rename` | ~~AT_FDCWD→cwd 内核债~~ 已修：`resolve_dirfd_start(AT_FDCWD)` 解析 `bp->cwd` 为 inode（`kernel/bsd/vfs.c`，非 root）；plain syscall 走 `vfs_resolve_user` 已解析 `bp->cwd`。`bp->cwd` 成 cwd 唯一真相 | 已下掉：repo `chdir`/`getcwd`/`unlinkat`/`renameat`/`openat`/`rename` + `cwd_path`/`resolve_at_path`/`__get_cwd`；接入 musl `openat.c`（fcntl）+ `chdir.c`/`getcwd.c`/`renameat.c`/`unlinkat.c`（unistd）+ `src/stdio/rename.c`；删 `user/include/xos/fcntl_ext.h`。`do_stat_path`/`fstatat`/`mkdir`/`mkdirat`/`opendir` 的 cwd 前缀拼装一并下掉（相对路径直送内核）。`rename` 缺 `LIBC_EXPORT` 在 libc.so 隐藏（libc.map 列了但不导出）的潜在 bug 由强制重编暴露、改采 musl `rename` 修复 |
| | `faccessat` | AT_EACCESS 路径要 `__block_all_sigs`/`__restore_sigs`/`__clone` clone shim | 补 clone shim 后接入 musl `faccessat.c` |                                                                                                    
| | `isatty` | musl 探 `TIOCGWINSZ`；串口（`driver/serial.c:211`）只答 `TCGETS`，余返 `-ENOTTY` | 串口补 `TIOCGWINSZ`（或 repo `isatty` 改用 `TCGETS`，已保留） |                                                                           
| | `sleep`/`usleep` | musl 遇 `EINTR` 返剩余秒，repo 语义是 EINTR 后重试睡满 | time 已迁 musl（见下「time 全量迁移」），`sleep`/`usleep` 仍在 `time.cc` 故意保留 EINTR-resume 语义（musl `sleep.c`/`usleep.c` 仍排除于 `MUSL_UNISTD_EXCLUDE`）；仅当确认无消费者依赖"睡满"语义且想统一 POSIX 返回值时再换 musl 版 |                                                                                                     
| | `ttyname`/`ttyname_r` | musl `readlink("/proc/self/fd/N")` + `stat`/`fstat` dev/ino 交叉校验，硬依赖 procfs 魔幻 symlink | ✅ procfs 已上线（`/proc/self/fd/N`→`/dev/ptsN` 或 `/dev/ttyS0`，见 [procfs.md](procfs.md) §3.4）。解锁条件具备，但 musl `ttyname_r` 的 `stat`/`fstat` dev+ino 交叉校验要求 `/dev/ptsN` 路径解析到的 devtmpfs inode 与 tty fd 的 `fstat` 命中同一 (dev,ino)——当前 repo 版走 `TIOCGPTN` 拼 `/dev/ptsN` 已够 PTY 用，切 musl 上游版前需先验证该交叉校验精度，避免 ENODEV 回退。已登记 procfs TODO（见文末） |
| | `gethostname` | musl 返 `uname().nodename`（"（none）"）；repo 走 `sys_gethostname`（sethostname 回环） | `uname` nodename 正确填充后可换 musl 版 |                                                                                     
| **musl 声明但内核无 syscall/无实现（欠账）** | `getgroups`/`setgroups` | 内核无 groups 数组 | proc 加 `groups[]` + syscall |                                                                                                              
| | `getresuid`/`getresgid`/`setresuid`/`setresgid` | 内核无 saved-set 查询/三参 res 版本 | proc 加 suid/sgid + `sys_setresuid/setresgid`（凭证已进程级） |                                                                                 
| | `nice` | 依赖 `getpriority`/`setpriority`（musl 已编进）+ 调度器 priority | 接 `sys_getpriority`/`sys_setpriority` + 调度优先级 |                                                                                                       
| | `acct` | 依赖 acct syscall + 记账写盘 | 上 acct syscall（非常后期） |                                                                                                                                                                   
| | `chroot`/`vhangup` | 无对应 syscall | 上 `sys_chroot`/`sys_vhangup` |                                                                                                                                                                   
| | `brk`/`sbrk` | malloc 走静态 freelist，不用 brk | 返 `-ENOSYS` 或 mmap 近似（非必须） |                                                                                                                                                 
| | `getdtablesize`/`issetugid`/`ctermid`/`get_current_dir_name`/`syncfs` | 边缘/可用户态包装 | 逐项用户态 shim 或 edge syscall |                                                                                                           
| | `euidaccess`/`eaccess` | 依赖 `faccessat(AT_EACCESS)` | 随 `faccessat` 解锁 |                                                                                                                                                           
| | `vfork`/`fexecve` | `vfork` 需 CLONE_VFORK-ish；`fexecve` 需 fd-as-exec | clone 扩展 + `execveat` |                                                                                                                                     
| | `crypt`/`encrypt`/`gethostid`/`sethostid`/`getdomainname`/`setdomainname` | 边缘 legacy | 用户态 stub 或 edge syscall |                                                                                                                 
| | `getlogin`/`getlogin_r` | 依赖 utmp | 上 utmp（非常后期） |                                                                                                                                                                             
| | `setpgrp` | = `setpgid(0,0)` | 一行 shim |                                                                                                                                                                                              
| | `pathconf`/`fpathconf`/`confstr` | musl 在 `src/misc`，本批只拉了 setpriority/getpriority | 补编 `src/misc/pathconf.c`/`confstr.c` + 内核 `_PC_*`/`_CS_*` 取值 |                                                                        
| | `daemon`/`getpass`/`getusershell`/`setusershell`/`endusershell` | 边缘功能 | 用户态包装或忽略 |                                                                                                                                         
| **机制缝（已 shim 降级，非 musl 真实现）** | `__setxid`（set*id 系列） | musl 原版走 `__synccall` 跨线程广播；本内核凭证进程级 | 多线程凭证同步需求出现时再做（当前降级为单次 syscall 等价，见 `user/lib/musl_shim/syscall_cp.c`） |
| | ~~`__syscall_cp`（cancellable 系列底层）~~ | ~~musl 原版走 `__syscall_cp_asm` + pthread_cancel；自研 pthread 无 cancel~~ | ✅ 已落地：pthread 切到 musl 真实现后 `__syscall_cp` 来自 `musl_pthread`（`src/thread/__syscall_cp.c` + `x86_64/syscall_cp.s`），取消点已 live；`musl_shim/syscall_cp.c` 不再定义 `__syscall_cp`（防与 musl_pthread 重复符号），仅留 `__setxid` |                                                                                       
                                                                                                                                                                                                                                            
**剩余债（M0.4 之后）**：~~① AT_FDCWD→cwd 内核修后下掉 `chdir`/`getcwd`/`renameat`/`unlinkat` repo 版与 `cwd_path`~~ ✅ 已落地（2026-07-28，见上表 M0.4 行）；~~② procfs 上线后下掉 `ttyname`/`ttyname_r`~~ ✅ procfs 已上线（[procfs.md](procfs.md)），`/proc/self/fd/N` 魔幻 symlink 就位；切 musl 上游 `ttyname_r` 待 dev+ino 交叉校验精度验证（见 procfs TODO）；③ 串口补 `TIOCGWINSZ` 后可换 `isatty` 到 musl；④ `i
t_interval` 重复 alarm（`alarm_check` 到期重 arm）+ ITIMER_VIRTUAL/PROF；⑤ shim 目录 `user/lib/musl_shim/` 仍未 `git add`（untracked），errno 双包装修复在 diff 盲区。

### stdlib 全量迁移到 musl（含启动/env/exit 链，退役 shim）

stdlib 模块（纯计算 + 启动/env/exit 链）切到 musl 上游（`musl_stdlib_objs`，`add_musl_lib` 单 -fPIC OBJECT lib 同时喂 libc.a + libc.so，同 string/math 模式）。源清单：`src/stdlib/{abs,labs,llabs,imaxabs,imaxdiv,div,ldiv,lldiv,atoi,atol,atoll,strtol,strtod,atof,bsearch,qsort}.c` + `src/prng/{rand,rand_r}.c` + `src/internal/{intscan,shgetc,floatscan}.c` + `src/env/__libc_start_main.c` + `src/env/{__environ,getenv,setenv,putenv,unsetenv,clearenv}.c` + `src/exit/{exit,atexit,abort,quick_exit,at_quick_exit}.c`。

退役 shim：删 `user/lib/start_main.cc`（`__libc_start_main`/atexit/`__libc_run_*`/environ 全家/`__libc_env_init`/`decode_auxv`/`__libc_fini_array_trampoline`）、`user/lib/musl_startup.c`（`musl_libc_init_aux` 冗余——musl `__init_libc` 自设 `libc.page_size`/`libc.auxv`）、`user/lib/strtol.c`（musl strtol.c/strtod.c/atoi/atol/atoll 覆盖全 strto*/ato* 家族）。`stdlib_misc.c` 精简至暂留子集；`signal.cc` 删 `abort`（musl `src/exit/abort.c` 接管）。当时 `__stdio_exit` 由新增 `user/lib/stdio_exit.c`（fflush stdout+stderr）覆盖 musl `exit.c` 的 weak dummy（repo FILE 不在 musl ofl 链）；该文件已在后续 stdio 全量迁移（见下节）物理删除，由 musl `__stdio_exit.c` 接管。

源码核实（两条曾担心的大问题都不成立）：(1) TLS 重复初始化不成立——dynlink.c 的 `__init_tls` 是空 no-op（loader 在 `__dls3` 自做 TLS），musl `__libc_start_main→__init_libc→__init_tls` 动态路径等于空操作，静态路径用 `src/env` 的真 `__init_tls`（已在 `musl_pthread`）；(2) env 握手不成立——musl `__putenv` 第一次 `setenv` 时懒拷贝栈 `envp` 到堆，替代仓库 `__libc_env_init` 预拷贝；loader 在 `__libc_start_main` 前 `__environ=envp`+`getenv("LD_LIBRARY_PATH")` 的时序由 musl `__environ.c`(`weak_alias(__environ,environ)`) + `getenv.c` 直读 `__environ` 满足。内核 auxv（`kernel/bsd/proc.c` execve / `proc_create.c`）提供 `AT_PHDR/PHENT/PHNUM/ENTRY/PAGESZ/RANDOM/EXECFN` + `AT_UID==AT_EUID==AT_GID==AT_EGID==0`/`AT_SECURE=0`，故 `__init_libc` 的 secure-check 早返（不开 `/dev/null`）。`crt1.c` 6 参调用 `__libc_start_main` 与 musl 3 参定义在 x86-64 ABI 兼容（多余参在寄存器被忽略）。`_Exit.c` 不在此批（已在 `musl_unistd_objs`，编入会 multi-def）。`__uflow` 当时由 `user/lib/musl_shim/scan_uflow_stub.c`（返回 EOF，运行时不可达——strtol/strtod 把 `shend` 设为巨大哨兵，`rpos` 永不到 `shend`）满足 musl 扫描器链的 link-time 引用。该 stub 在后续 stdio 全量迁移（见下节）删除——musl `src/stdio/__uflow.c`（`musl_stdio_objs` glob）提供真 `__uflow`，同时满足扫描链引用与 stdio 运行期，stub 再留会 multi-define。

**暂留子集（musl 无法替换，待后续模块迁移解锁）**：
- `realpath`（`stdlib_misc.c`）：musl `src/misc/realpath.c` 需 `__procfdname` + `readlink("/proc/self/fd/N")` + `O_PATH`。✅ procfs 已上线（[procfs.md](procfs.md)），`/proc/self/fd/N` + readlink 子系统就位；`O_PATH` 内核尚未支持是剩余阻断，补 `O_PATH` 后切 musl `realpath`。
- `mkstemp`/`mktemp`（`stdlib_misc.c`）：musl `src/temp/__randname.c` 调 `__clock_gettime(CLOCK_REALTIME)`。~~解锁：time 模块迁移到 musl 后切 `__randname`/`__clock_gettime`~~ time 已迁 musl（见下「time 全量迁移」，`__clock_gettime` 现由 musl `clock_gettime.c` 提供），阻断已消除；切 musl `mkstemp` 为 stdlib 收尾子任务。
- ~~`getline`/`fscanf`/`scanf`（`stdlib_misc.c`，当前 ENOSYS stub）：musl 把它们放 `src/stdio/`，依赖整套 musl stdio 的 `__uflow`/shgetc 扫描链；repo stdio 是自写 `stdio.cc`（非 musl FILE），硬塞接不上。解锁：stdio 整体迁 musl 时一起上。~~ ✅ 已落地（stdio 全量迁 musl，见下「stdio 全量迁移」节）：musl `src/stdio` 真实现接管，stub 删除。
- `sysconf`/`getpagesize`（`stdlib_misc.c`）：musl `src/conf/sysconf.c`（217 行）改 `_SC_NPROCESSORS_ONLN` 语义，repo 走 `sys_sysconf` syscall。解锁：对齐 ncpu 取值语义后切 musl 版（连带 `src/legacy/getpagesize.c`）。
- `mknod`/`chmod`/~~`remove`~~（`stdlib_misc.c`）：薄 syscall 封装，musl 版走同一 syscall 号但拖 `src/misc/sysm.c` 机制，收益≈0；~~`remove` 在 musl 属 stdio~~ ✅ `remove` 已迁——musl `src/stdio/remove.c` 接管（stdio 全量迁移）。`mknod`/`chmod` 低优先级，仍留。
- **`getrandom`/`getentropy`（`user/lib/getrandom.c`）——独立里程碑**：musl 有（`src/linux/getrandom.c`），但属 syscall wrapper（`src/linux/`），不在 stdlib 迁移范围（只迁 `src/stdlib/`）。解锁：作为「`src/linux/` syscall wrapper 批次迁移」里程碑接入。
- `arc4random_buf`/`arc4random_uniform`（`user/lib/getrandom.c`）：**永久留 repo**——musl 无此源文件（BSD 扩展，musl 不实现 arc4random）。已在 `user/include/sys/random.h` 用 `LIBC_EXPORT` 声明导出。
- `abort`（曾留 `signal.cc`）：已迁——musl `src/exit/abort.c` 接管，`signal.cc` 删旧实现。                                

### time 全量迁移到 musl

time 模块（日历换算 + strftime/strptime + TZ + clock/nanosleep 包装）切到 musl 上游（`musl_time_objs`，`add_musl_lib` 单 -fPIC OBJECT lib 同时喂 libc.a + libc.so，同 string/math/stdlib 模式）。源清单：`src/time/{difftime,__year_to_secs,__month_to_secs,__tm_to_secs,__secs_to_tm,__tz,__map_file,gmtime,gmtime_r,localtime,localtime_r,mktime,timegm,asctime,asctime_r,__asctime,ctime,ctime_r,strftime,strptime,clock,clock_gettime,clock_settime,clock_getres,clock_getcpuclockid,clock_nanosleep,nanosleep,gettimeofday,time,timespec_get,times,utime,ftime,getdate}.c` + `src/locale/langinfo.c`（`__nl_langinfo_l`/`nl_langinfo` C-locale 提供者，喂 `__asctime.c`/`strftime.c`/`strptime.c` 的 weekday/month 名）。

退役 shim：`user/lib/time.cc` 精简至**仅** `sleep`/`usleep`（EINTR-resume 故意保留，见上表 sleep/usleep 行）；删 `timespec_to_tm`（仓库非标准，0 调用者，musl 无对应，`libc.map`/`time.h` 同步删）。`user/include/time.h` 改转发 musl `include/time.h`（同 `sys/time.h` shim），拿到带 `__tm_gmtoff`/`__tm_zone` 的 `struct tm`（musl `gmtime_r`/`mktime`/`strftime` 写这些字段）。

`user/lib/musl_glue.c` 删 `__clock_gettime` forwarder（musl `clock_gettime.c` 现 strong 定义 `__clock_gettime` + weak `clock_gettime`，会 multi-def；`__timedwait.c`/`__randname.c` 改用 musl 版）；新增 `__vdsosym` 桩返回 NULL（`arch/x86_64/syscall_arch.h` 定义 `VDSO_CGT_SYM` 故 `clock_gettime.c` 编进 vdso 探测，本 OS 无 vdso → 首调缓存 `vdso_func=NULL` 回落 `__syscall`，不编 `src/internal/vdso.c`）。

源码核实：(1) 内核已实现 `SYS_clock_gettime(228)`/`clock_settime(227)`/`clock_nanosleep(230)`/`nanosleep(35)`/`gettimeofday(96)`/`utimensat(280)`；`clock_getres`/`times` 内核无 impl，musl 走 `__syscall` 返 `-ENOSYS`（0 调用者，可接受）。(2) `__tz.c` 走 `getenv("TZ")`+`__map_file("/etc/localtime")`（absent → 默认 UTC，等价旧 repo UTC-only 桩但支持 TZ），依赖 `__munmap`/`__mmap`（musl_glue）、`__sys_open`（musl `syscall.h` 宏）、`__lock`/`a_cas`/`a_crash`/`a_store`/`__wait`/`__wake`（musl_pthread）、`getenv`（musl_stdlib）、`memcpy/strlen`（musl_string）——均已就绪。(3) `__asctime.c` 用 `snprintf`（repo `stdio.cc`）+ `__nl_langinfo_l`（langinfo.c）+ `a_crash`（inline）。(4) `utime.c` 调 `utimensat`（`musl_unistd_objs` glob 已含 `src/unistd/utimensat.c`）。

**延后项**：
- ~~`wcsftime.c` — 依赖 wchar 层（`wcstoul`/`mbstowcs`/`wmemcpy` 未迁移），同 string 排除 wcs*/wmem*；随 wchar 迁移 tier 接入。~~ ✅ **已落地**：随「wchar/wctype/uchar 全量迁移」（见下）`wcsftime.c` 现由 `musl_wchar_objs` 编译。**已知 musl 上游 bug**：`wcsftime.c` 声明/调用 5 参 `__strftime_fmt_1`，而 `strftime.c` 定义 6 参（`..., int pad`）并使用 `pad`；x86-64 SysV ABI 下 `wcsftime` 调用时 `%r9`(`pad`)是垃圾，`strftime.c` 的 `switch(pad ? pad : def_pad)` 可能选错填充符。0 in-tree 调用方，按上游原样编译（HEAD 亦然）；若需修应 patch musl 而非迁移动作内处理。
- `timer_create.c`/`timer_settime.c`/`timer_delete.c`/`timer_gettime.c`/`timer_getoverrun.c` — 内核无 `SYS_timer_*`（返 `-ENOSYS`）且 SIGEV_THREAD 路径要重 pthread 内部（`pthread_barrier_*`/`__reset_tls`/`SIGTIMER`/`__libc_sigaction`），0 调用者。解锁：内核加 POSIX timer 系 syscall（`sys_timer_create/settime/gettime/getoverrun/delete`，timerfd 已有类似机制可参考）后接入。
- `sigtimedwait.c`/`sigqueue.c`/`sigsuspend.c`/`sigpause.c`（+ 依赖它们的 `sigwait.c`/`sigwaitinfo.c`）— 内核无 `SYS_rt_sigtimedwait`/`SYS_rt_sigqueueinfo`/`SYS_rt_sigsuspend`（dispatch 返 `-ENOSYS`），0 in-tree 调用者。signal 模块迁移（musl_worklist signal）只纳入安全源（killpg/sigorset/sigandset/sigisemptyset/sighold/sigrelse/sigset/sigignore/siginterrupt/siglongjmp/getitimer/psignal/psiginfo/strsignal），整个 sigwait 家族不导出以免暴露必然 `-ENOSYS` 的符号。解锁：内核加 `sys_rt_sigtimedwait`/`sys_rt_sigqueueinfo`/`sys_rt_sigsuspend`（参考 `sys_sigaction` 的 8-byte mask wire ABI + `musl_glue.c::__libc_sigaction` 转换模式）后，把这 6 个源 + libc.map 导出一并接入。另：`sigsetjmp` 仍 declare-only——setjmp tier（musl_worklist setjmp）已把 setjmp/longjmp 切到 musl x86_64 asm，但 musl 1.2.6 x86_64 的 setjmp.s **不定义 sigsetjmp**（src/signal/x86_64/ 目录不存在，src/signal/sigsetjmp.c 是空文件），故无实现可编，0 调用者，libc.map 不列；`siglongjmp` 已导出但无配对 `sigsetjmp`——半成品状态延续，待 musl 提供 x86_64 sigsetjmp asm 或独立实现后解锁。

### stdio 全量迁移到 musl（FILE 翻转 + ofl 链转真，退役手写 stdio.cc）

stdio 模块（最后一个核心 libc 模块）切到 musl 上游 `src/stdio/*.c`（`musl_stdio_objs`，`add_musl_lib` 单 -fPIC OBJECT lib 同时喂 libc.a + libc.so，同 stdlib/string/math/mman 模式）。源清单：`file(GLOB src/stdio/*.c)` + `list(REMOVE_ITEM)` 剔除 10 个冲突/拖依赖文件（见下）。FILE 布局整体翻转：repo 自写 `struct _FILE` → musl `struct _IO_FILE`（`src/internal/stdio_impl.h`）；`bits/alltypes.h` 已 `typedef struct _IO_FILE FILE`，全树核对无非-stdio 代码解引用 FILE 字段（全走 API），类型别名无害。

退役手写实现：删 `user/lib/stdio.cc`（799 行）+ 物理删 `user/lib/stdio_exit.c`（musl `__stdio_exit.c` 接管 ofl 链 + `__stdin_used`/`__stdout_used`/`__stderr_used`）；`utils/kvformat.c` 从 `LIBC_SOURCES` 移除（用户态无调用方，musl vfprintf 取代；kernel 的 `-D__KERNEL__` 独立编译副本 `utils/CMakeLists.txt` 不受影响，文件本身保留）；`stdlib_misc.c` 删 `getline`/`fscanf`/`scanf`/`remove` 四个 stub；`musl_loader_shim.c` 删 `getdelim`（musl `getdelim.c` 接管）。`user/include/stdio.h` 改薄 shim（纯转发 `#include "musl/include/stdio.h"`，仿 `sys/mman.h` 模式）；`install-headers.sh` §3k 发布 musl 真身到 sysroot。

exclude 名单（10 文件，防 multi-define / 拖依赖）：
- `ofl.c`/`__lockfile.c` — 已在 `musl_pthread`（PROVIDE `__ofl_lock`/`__ofl_unlock` + `__lockfile`）。**迁移后这两源从"仅 PROVIDE、运行期 no-op"转真身**：ofl 链现被 musl stdio/fclose/`__stdio_exit` 真正使用，`__lockfile` 的 FLOCK 现被 musl stdio 路径真正调用。保留在 `musl_pthread`（不再 exclude 进 `musl_stdio_objs`）。
- `__stdio_exit.c` — **由 `musl_stdio_objs` glob 带入**（不在 exclude 名单）；`stdio_exit.c` 已删，不再需要手写覆盖。
- `rename.c` — 已在 `musl_unistd_objs`（src/stdio/rename.c 编入 unistd）。
- `dprintf.c`/`vdprintf.c` — **不 exclude**。loader 直接解析到 musl 原生版（走 vfprintf）；所有 loader 调用点都在 `reloc_all(&ldso)`（`dynlink.c:1432`）之后，PLT 已就绪，无需 boot-safe shim（已验证：boot + 多次 exec ld.so 正常）。
- `popen.c`/`pclose.c` — 需 `posix_spawn`（`src/process` 重依赖），无消费方，本期排除。
- `tmpfile.c`/`tmpnam.c`/`tempnam.c` — 需 `__randname`→`__clock_gettime`，~~time 模块未迁~~ time 已迁 musl（见上「time 全量迁移」，`__clock_gettime` 由 musl `clock_gettime.c` 提供，同 stdlib `mkstemp` 阻断已消除）；这三源本期未纳入纯属迁移范围未含，解锁即为补编。

`libc.map` `<stdio.h>` 块扩至 81 符号（全窄字符 stdio 家族 + `*_unlocked` + `fseeko`/`ftello` + `stdin`/`stdout`/`stderr` OBJECT）；不导出 `__isoc99_*`/`_IO_*`/`__getdelim`/`*_used`（内部 `weak_alias`，头文件未声明，无消费方引用，`local: *;` 兜底隐藏）；不导出宽字符 w* 家族（`fwprintf`/`fgetwc`/`wscanf`/... 定义在 `musl_stdio_objs` 但声明在 `<wchar.h>`，本 OS 未发布 `<wchar.h>`，无消费方可引用，隐藏以保持 ABI 最小）；不导出 `dprintf`/`vdprintf`（musl 原生 stdio 实现，loader 解析到它们但无 app 消费方需导出，`local: *;` 隐藏）；`rename` 留在 unistd 块不重列。

**已根治（随本次迁移落地）**：
- todo.md:345 `getline`/`fscanf`/`scanf`（ENOSYS stub）→ musl `src/stdio` 真实现接管。
- todo.md:347 `remove`（stdlib_misc 薄封装）→ musl `src/stdio/remove.c` 接管（`mknod`/`chmod` 仍留 stdlib_misc）。
- todo.md:359 `MUSL_PTHREAD_SOURCES` 的 stdio 仪式符号 `ofl.c`/`__lockfile.c`/`__stdio_exit.c` → ofl 链转真（不再是 no-op），`*_used` 由 musl stdin/stdout/stderr.c 强定义（不再弱别名到 NULL dummy）；`ofl.c`/`__lockfile.c` 仍留 `musl_pthread`（ofl 锁真身），`__stdio_exit.c` 归 `musl_stdio_objs`。

**剩余缺口（迁移不解决/新引入，非 bug）**：
- **stdout 串口无缓冲**（行为变化）：musl `__stdout_write` 探 `TIOCGWINSZ`，`/dev/serial`（init dup2 成 fd 0/1/2）只答 `TCGETS`、`TIOCGWINSZ` 返 `-ENOTTY` → stdout `lbf=-1` 无缓冲（每字符一次 `sys_write`）。PTY 答 TIOCGWINSZ 保持行缓冲。旧手写 stdio 硬编码 `_IOLBF` 才有行缓冲。功能不损（exit 时 `__stdio_exit` 仍 flush 全部流）。若需恢复行缓冲：给 `kernel/driver/serial.c` ioctl 加 TIOCGWINSZ 分支返默认 winsize（即 todo.md:309/331③"串口补 TIOCGWINSZ"同一内核改动）。
- **popen/pclose 未采用**：需 `posix_spawn`，本期排除，无消费方。
- **tmpfile/tmpnam/tempnam 未采用**：需 `__randname`→`__clock_gettime`，time 已迁 musl（见上「time 全量迁移」），阻断已消除；本期未纳入纯属迁移范围未含，解锁即为补编（同 stdlib `mkstemp`，:344 同源）。
- **loader dprintf/vdprintf 已归一**：~~shim 的 boot-safe raw-syscall 版保留，musl `dprintf.c`/`vdprintf.c` 从 `musl_stdio_objs` exclude~~ ✅ 已改——shim 的 `dprintf`/`vdprintf` 已删，`dprintf.c`/`vdprintf.c` 不再 exclude，loader 直接用 musl 原生版（走 vfprintf）。所有 loader 调用点在 `reloc_all(&ldso)`（`dynlink.c:1432`）后，PLT 已就绪，原生版安全（boot + 多次 exec 验证通过）。公开 libc 仍无 `dprintf`/`vdprintf` 导出。
- ~~**宽字符 w* 系列已排除**（非"顺带纳入"）~~ ✅ **已纳入**（随「wchar/wctype/uchar 全量迁移」，见下节）：23 个 w* 文件（`vfwprintf`/`vfwscanf`/`fgetwc`/`fputwc`/`fputws`/`fgetws`/`ungetwc`/`fwide`/`fwprintf`/`fwscanf`/`wprintf`/`wscanf`/`swprintf`/`swscanf`/`v{w,sw}{printf,scanf}`/`getwc`/`getwchar`/`putwc`/`putwchar`/`open_wmemstream`）已从 `musl_stdio_objs` 的 REMOVE_ITEM 移出，编进 libc 并在 libc.map `<wchar.h>` 块导出。它们引用的 `isw*`/`wcsnlen`/`btowc`/`wctob` 由 `musl_wchar_objs` 提供、`__c_locale`/`__c_dot_utf8_locale` 由 `musl_time_objs` 提供，依赖已就绪故旧"运行期重定位失败：`iswspace: symbol not found`" blocker 彻底消解。`<wchar.h>` 已发布，故 wide-stdio 家族现**真可用**（不再 declare-only）。`*_unlocked`/`__isoc99_*` 内部别名仍 hidden（同窄 stdio 纪律）。multibyte 窄路径最小集（`wctomb`/`wcrtomb`/`mbrtowc`/`mbsinit`/`internal` 五文件，供 vfprintf `%ls`/vfscanf `%ls` link-time 引用）不变。


### wchar/wctype/uchar 全量迁移到 musl

`<uchar.h>`(C11)/`<wctype.h>`(C95)/`<wchar.h>`(C95) 三个宽字符模块此前是 libc 的空白：仓库无 `user/include/{wchar,wctype,uchar}.h`、无任何实现，`musl_string_objs` 刻意排除 `wcs*`/`wmem*`、`musl_time_objs` 排除 `wcsftime.c`、`musl_stdio_objs` 排除 23 个宽字符 stdio 文件（均标注 deferred to wchar tier）。本迁移兑现这些 deferral：新建 `musl_wchar_objs`（`add_musl_lib` 单 -fPIC OBJECT lib，同时喂 libc.a + libc.so，同 string/stdlib/mman 模式）编进上游宽字符源，发布三个头文件到 sysroot，libc.map 导出符号。

**零 in-tree 消费方**：grep `user/`/`kernel/`/`init/` 确认无 `wchar_t`/`wint_t`/`char16_t`/`char32_t`/`mbrtowc`/`iswalpha`/`wcwidth` 等任何引用（`third_party/`、`build/` 外）。故这是纯增量、零回归风险的 ABI 扩展，无源码删除（三模块此前本就无仓库实现）。

源清单（三桶，`musl_wchar_objs`）：
- `<uchar.h>`（4）：`src/multibyte/{c16rtomb,mbrtoc16,c32rtomb,mbrtoc32}.c` —— 皆 thin wrapper，建于 `wcrtomb`/`mbrtowc`（已在 `musl_multibyte_objs`）。
- `<wctype.h>`（17）：`src/ctype/{iswalnum,iswalpha,iswblank,iswcntrl,iswdigit,iswgraph,iswlower,iswprint,iswpunct,iswspace,iswupper,iswxdigit,iswctype,wctrans,towctrans,wcwidth,wcswidth}.c` —— 纯计算；`isw*.c` 每个定义基函数 + `__isw*_l` weak_alias，`towctrans.c` 同时定义 `towupper`/`towlower` + `_l`，`iswctype.c` 定义 `iswctype`/`wctype` + `_l`。窄 ctype（`isalnum.c`/`tolower.c`/...）**不编**——`user/lib/ctype.c` 已定义且 libc.map 已导出。
- `<wchar.h>`：`src/string` 的 `wcs*`/`wmem*` 全集（28 文件，`musl_string_objs` 仍排除，此处分立编译避免迁移它处）+ `src/multibyte/{btowc,wctob,mblen,mbrlen,mbstowcs,wcstombs,mbsrtowcs,wcsrtombs,mbsnrtowcs,wcsnrtombs,mbtowc}.c`（`wctomb`/`wcrtomb`/`mbrtowc`/`mbsinit`/`internal` 已在 `musl_multibyte_objs`，不重列，否则 multi-define）+ `src/stdlib/{wcstol,wcstod}.c`（`__intscan`/`__floatscan`/`shlim`/`shcnt` 由 `musl_stdlib_objs` 在合并 libc.a/libc.so 内解析；`wcstol.c` 解引用 `struct _IO_FILE` 字段，由 `src/internal/stdio_impl.h` 在 add_musl_lib 的 include 序前段解析）+ `src/time/wcsftime.c`（time 机制由 `musl_time_objs` 提供）。

头文件发布（`install-headers.sh` §3l）：`cp third_party/musl/include/{wchar,wctype,uchar}.h` → sysroot。仓库无 source-tree shim（零 in-tree 消费方，同 `dlfcn.h` §3b 模式）。三者 closure 均依赖 `<bits/alltypes.h>`（step 2 已发布，提供 `wchar_t`(int)/`wint_t`(unsigned)/`wctype_t`(unsigned long)/`mbstate_t`/`locale_t`）+ `<features.h>`；`<wchar.h>` 另需 `FILE`（alltypes `__NEED_FILE` → `struct _IO_FILE`）与 `struct tm`（前向声明）。无新 `bits/` 头。脚本末尾"扫描每个已发布头文件自包含 closure"自检自动覆盖。

libc.map（`global:` 增 ~113 符号，三块）：`<uchar.h>` 块（`c16rtomb`/`mbrtoc16`/`c32rtomb`/`mbrtoc32`）；`<wctype.h>` 块（`isw*` + `iswctype`/`wctype`/`wctrans`/`towctrans`/`towlower`/`towupper`/`wcwidth`/`wcswidth` + 全 `_l`）；`<wchar.h>` 块（`wcs*`/`wmem*` + `btowc`/`wctob`/`mblen`/`mbrlen`/`mbstowcs`/`wcstombs`/`mbsrtowcs`/`wcsrtombs`/`mbsnrtowcs`/`wcsnrtombs`/`mbtowc`/`wctomb`/`mbsinit`/`mbrtowc`/`wcrtomb` + `wcstof`/`wcstod`/`wcstold`/`wcstol`/`wcstoul`/`wcstoll`/`wcstoull`/`wcstoimax`/`wcstoumax` + `wcsftime` + 23 个 wide-stdio 符号 `fwide`/`wprintf`/`fwprintf`/`swprintf`/`vwprintf`/`vfwprintf`/`vswprintf`/`wscanf`/`fwscanf`/`swscanf`/`vwscanf`/`vfwscanf`/`vswscanf`/`fgetwc`/`getwc`/`getwchar`/`fputwc`/`putwc`/`putwchar`/`fgetws`/`fputws`/`ungetwc`/`open_wmemstream`）。可见性关键：`mbsinit`/`mbrtowc`/`wcrtomb`/`wctomb` 此前在 `build/libc.so` 中是 LOCAL（`add_musl_lib` 未设 `-fvisibility=hidden`，但 libc.map `local: *;` 把未列 global 的符号全隐藏）；现 `<wchar.h>` 声明为公共 API，故须在 libc.map global 列出以升为 GLOBAL 导出，并通过 `verify_libc_exports.sh` 双向门禁。

**范围分叉决策**（AskUserQuestion 确认，记此备查）：
- **宽字符 stdio（23 文件）纳入而非排除**：依赖已全部就绪（`isw*`/`wcsnlen`/`btowc`/`wctob` 由 `musl_wchar_objs` 提供，`__c_locale`/`__c_dot_utf8_locale` 由 `musl_time_objs` 提供），原 "runtime reloc: iswspace not found" blocker 彻底消解。`<wchar.h>` 已发布、声明了 `fwprintf`/`wscanf`/`fgetwc`/...，declare-only 是半成品状态（未来 Mesa/Proton/glibc 二进制/libwayland 移植 `#include <wchar.h>` 调 `fwprintf` 会 link error）；且这 23 个是纯用户态、无内核 syscall 依赖，编进去真可用。按 mman 迁移"导出无消费方 POSIX 符号"的先例纳入并导出。`*_unlocked`/`__isoc99_*` 内部别名仍 hidden（未在 `<wchar.h>` 声明，无消费方，同窄 stdio 纪律）。（原计划 defer，AskUserQuestion 复核后改为纳入。）
- ~~**`wcscoll`/`wcsxfrm`（+`_l`）declare-only**：需 `src/locale`（未构建），同 `strcoll`/`strxfrm`——libc.map 不列、不实现、0 调用者。~~ ✅ **已落地**：locale tier（musl_worklist locale，`musl_locale_objs`）编译 `src/locale/{strcoll,strxfrm,wcscoll,wcsxfrm}.c` 并在 libc.map `<locale.h>` 块导出（+`_l`）。C-locale code-point 桩（`strcoll=strcmp`/`strxfrm=strlen+strcpy`/宽字符对应 `wcscmp`/`wcslen+wmemcpy`），无 collation 重排。`strcasecmp_l`/`strncasecmp_l`（`src/locale/strcasecmp_l.c`）仍 declare-only——locale tier 范围未含，0 调用者。

**已知缺陷**（musl 上游 bug，非本迁移引入，记录不修）：`__strftime_fmt_1` 签名错配——`strftime.c`（已编）定义 6 参（`..., int pad`）并用 `pad`，`wcsftime.c` 声明/调用 5 参（无 `pad`）。x86-64 SysV ABI 下 `wcsftime` 调用时 `%r9`(`pad`)是垃圾，`strftime.c` 的 `switch(pad ? pad : def_pad)` 可能选错填充符。`wcsftime` 无任何 in-tree 调用方，musl HEAD 亦然；本迁移原样编译 `wcsftime.c`（与上游一致）。若需修应 patch musl 而非在此迁移动作内处理。

**验证**：`./build.sh`(clang) + `./build.sh --gcc` 双工具链构建通过；`verify_libc_exports.sh` 报 libc.so 导出匹配 libc.map（增 ~90 符号）；`./build.sh --test` + `./run.sh` 全套 88 测试 `PASS=88 FAIL=0`（无 wchar 测试，纯无回归校验，对齐 mman/stdio 迁移基线）；`install-headers.sh` closure 自检覆盖新增三头。不新增 `test_wchar`——既有迁移均靠现有 88 测试做无回归，零 in-tree 消费方下新增测试属范围蔓延。


### musl pthread + ldso 切换过渡层收尾

pthread（commit `8d19e24`，删 `user/lib/{pthread,tls,errno}.cc` + 旧头，`__errno_location` 由 musl `src/errno/__errno_location.c` 经 `%fs:0` 读取）与 ldso（commit `48b8d32`，删手写 `user/ldso/` 换 musl fused loader）都已切到 musl 真实现。剩下一批**过渡胶水**把 musl pthread/loader 接到本仓库自留 libc/syscall 上，均非 musl 原版，全部"整体切 musl libc.so 后可下掉"。逐项：

- [ ] **`musl_glue.c::__libc_sigaction`（128B ↔ 8B mask 转换）**：musl 用户态 `sigset_t` = `unsigned long[16]`(128B,_NSIG=65)，内核 `sigaction` 是 8B mask(信号 1..64)，`sys_sigaction` 严格要求 arg4==8 否则拒。故 musl 原版 `__libc_sigaction`(`src/signal/sigaction.c`)**不编**(会构 40B k_sigaction 传 mask-size 16)，由本 glue 逐字段转换：handler/flags/restorer 透传、低 8 字节 mask 取 128B 的 `__bits[0]`、`SA_RESTORER` 强置并塞 `__restore_rt`。根治路径二选一：① 内核 sigset 扩到对齐 musl 128B(内核侧全链路改 `sigset_t` 宽度，动静大)；② 反向——保留转换但迁到 musl 侧改写 `sigaction.c`(随整体 libc.so 切换吸收)。当前 glue 是**最小可工作**形态，未触内核信号语义。
- [x] ~~**`MUSL_PTHREAD_SOURCES` 里的 stdio 仪式符号（`ofl.c`/`__stdio_exit.c`/`__lockfile.c`）**~~ ✅ 已落地（stdio 全量迁 musl）：ofl 链转真——`pthread_create` 单线程→多线程过渡时遍历 musl open-FILE 链（`__ofl_lock`/`__ofl_unlock`）重锁 `__stdin_used`/`__stdout_used`/`__stderr_used` 现在是**真操作**（不再是 no-op），因 musl stdio `__fdopen`/`fopen` 经 `__ofl_add` 把 FILE 挂上 ofl 链、`__stdio_exit` 走 ofl + `*_used` flush。`*_used` 由 musl `stdin.c`/`stdout.c`/`stderr.c` 强定义（不再弱别名到 NULL dummy）。`ofl.c`/`__lockfile.c` 仍留 `musl_pthread`（ofl 锁 + FLOCK 真身，被 `musl_stdio_objs` exclude 防重复）；`__stdio_exit.c` 归 `musl_stdio_objs` glob（`stdio_exit.c` 已删）。
- [ ] **`musl_loader_shim.c`（ldso 重构残留，loader-only 非 pthread 符号）**：commit `48b8d32` 删整个手写 `user/ldso/`(`dls3.c`/`dls_init.c`/`load_so.c`/`relocate.c`/`symtab.c`/`link_map.c`/`minilibc.c`/`dl_puts.c`/`start.S` + 旧 `doc/design/ld.md` 2381 行)，换 musl fused loader(`ldso/dynlink.c`+`ldso/dlstart.c` 经 `musl_loader_objs` 融入 libc.so)。这个 shim 现仅提供 loader 引用、且 musl_pthread 与自留 libc 都不提供的符号：`__tlsdesc_static`/`__tlsdesc_dynamic`(TLSDESC handler 地址被 `R_X86_64_TLSDESC` reloc 存取，musl 定义在 `ldso/tlsdesc.c` 但未并入 `musl_loader_objs`/`musl_pthread`，仅 `-mtls-dialect=desc` 用，本 OS 从不用)。pthread/TLS 相关(`__libc`/`__hwcap`/`__init_tp`/`__copy_tls`/`__block_all_sigs`/`__inhibit_ptc`/`__tls_get_addr`...)已由 musl_pthread 真提供，loader 在 `__dls3` 跑 musl 真实 TLS 建立且自留 `__libc_start_main` 不再覆盖 fs_base。根治：loader 也整体切 musl dynlink 后由 musl 自身吸收，本 shim 删除。
- [ ] **ldso cmake wiring 的两处过渡机制（`musl_rules.cmake`）**：① `musl_loader_objs` 强制 `-O2`（无视 `CMAKE_BUILD_TYPE`）——规避自举陷阱：`-Os` 会把 dynlink.c 的 `find_sym` 内的 memset 循环降为 `call memset@plt`，而 `reloc_all(&ldso)` 解的是 ldso **自己**的 PLT GOT 槽，memset GOT 槽此时指向未映射野地址(0x6476)→ 崩溃。上游 musl 在 ldso 阶段 `-Os` 能跑因它整树一体；我们 fused 手写 libc + musl loader 的混合结构使该优化不安全。② `-Wno-all` 抑制 dynlink.c:1394 `for(...); auxv++;` idiom 警告。二者都是"fused 手写 libc + musl loader"混合期的临时约束，整体切 musl libc.so（loader 与 libc 同源）后这两处特殊编译参数随 `musl_loader_objs` 规则一并归一，不再需要。
- [ ] **`musl_startup.c` 桥（填 `libc.page_size`/`libc.auxv`）**：musl `pthread_create` 读 `libc.page_size`(经 `PAGE_SIZE` 宏)与 `libc.tls_size`；`__init_tls` 设 TLS 字段但**不**设 page_size，musl 原版 `__libc_start_main` 从 `aux[AT_PAGESZ]` 设。本仓库保留自己的 `__libc_start_main`(musl 的要读 `AT_SYSINFO`/`AT_UID`/`AT_SECURE`/...，内核不提供)，故此桥补 page_size + auxv(后者 `pthread_getattr_np` 走 mremap 探主栈用)。根治：内核补齐 auxv(`AT_SYSINFO`/`AT_UID`/`AT_EUID`/`AT_GID`/`AT_SECURE`/`AT_ENTRY`...)后换 musl `__libc_start_main`，本桥与自留 `start_main.cc` 一并下掉。
- [ ] **`musl_missing.c`（`strnlen`/`prctl`/`mremap` 补丁）**：musl 头声明、自留 libc 未提供或语义不同的兜底。`mremap` 已实现（`sys_mremap` in `kernel/bsd/syscall.c`，复用 `vma_*` + 新增 `move_user_pages`/`clear_user_pte` 原语）：shrink / 原地 grow / `MREMAP_MAYMOVE` move / `MREMAP_FIXED`，匿名全支持，file-backed/memfd-private 支持且 grow 走 demand fault-in，SHM/MAP_PHYSICAL 仅 shrink + 等量 move（grow 报 `-ENOMEM`，musl malloc 走 `copy_realloc` fallback）。**作用域限制：只处理整 region**（`old_addr==region->vaddr && old_size==region->size`），子区 mremap 报 `-EFAULT`——musl `malloc.c:407` 整 region 满足；`pthread_getattr_np.c:19` 的栈探测 old_addr 落在栈 region 中段，得 `-EFAULT`（errno≠ENOMEM），探测循环干净退出（与 stub 期行为一致，无回归）。根治随整体 musl libc.so 切换：`strnlen`/`prctl` 由 musl `src/string`/`src/linux` 真实现接管，`mremap` 直接用 musl `src/linux/mremap.c`（内核侧 `sys_mremap` 不变）。
- [ ] **`user/lib/musl_shim/` 目录仍未 `git add`（untracked）**：见"剩余债⑤"。内含 `syscall_cp.c`(`__setxid` shim，见上表机制缝)，errno 双包装修复落在 diff 盲区。整体切 musl 后 `__setxid` 随多线程凭证同步需求重做，本目录归档。

**统一切换前提**：上述各项的共同根治路径是"整体切 musl libc.so"——即不再保留自留 malloc/...，全部用 musl `src/` 树，动态链接器也走 musl dynlink。届时 musl_pthread 不再是"子库"而是 libc.so 本体，所有 glue/shim 桥文件(`musl_glue.c`/`musl_loader_shim.c`/`musl_startup.c`/`musl_missing.c`/`musl_shim/`)与 ldso cmake 特殊编译参数成批删除/归一。在此之前它们各自独立、可单独演进，是 pthread/ldso 已可用、stdio 已迁、但 startup/malloc 尚未整体迁移的**预期中间态**(非 bug)。切换的大前置是内核 auxv 补齐(第 5 项)——`AT_SYSINFO`/`AT_UID`/`AT_EUID`/`AT_GID`/`AT_SECURE`/`AT_ENTRY` 等，补齐后才能换掉自留 `__libc_start_main`(`start_main.cc`) 与 `musl_startup.c` 桥。

## udev 测试

- **test_dev_notify.c 废弃（P3-T3）**：base_worklist §2.4 P3-T3 列的 `test_dev_notify.c` 要测"内核通知 syscall"——但 `SYS_DEV_NOTIFY`(88) 已被 epoll 占用（base_worklist §2 开头），syscall 不存在。P3-T3 是 worklist 未同步架构演进的残留。内核 uevent 通知走 netlink（`nl_uevent_broadcast`），其测试已隐含在 test_udevd 的"收 uevent"路径里。**不再新增 test_dev_notify.c。**

## 设备节点命名

- [ ] **evdev 内核态字符设备 broker 重构（消除无意义 SHM 输出环）**（详见 [../../refact_evdev.md](../../refact_evdev.md)，实施方案 [../../plan_reface_evdev.md](../../plan_reface_evdev.md)）：当前 evdev 输出环（Ring #2，`input_event` 24B×128）走 `memfd_create`+`device_register_shm`+`ringbuf_fops`+`RINGBUF_WAKE` 全套 SHM 仪式，但消费者**不 mmap 该环**（`ringbuf_mmap` 对消费者返 `-EPERM`）且读走 `copy_to_user`——付出 SHM 代价却无 SHM 收益。重构为内核态字符设备 broker（对齐 Linux `drivers/input/evdev.c`）：`struct file` 增 `void *private_data`（`file_put` 已在 type-switch 前调 `f_op->close`，免新增 `FD_*`）；控制节点 `/dev/input/control` + `INPUT_REGISTER` ioctl（`driver_pid==0` direct path 返回 owner write-fd）；per-fd `evdev_client` kfifo + per-inode `client_list`+`client_lock`；`EVIOCG*`/`EVIOCGRAB` 由 broker 转发给用户态 evdev（Q 方案，**刻意偏离 Linux** 以保 caps/策略留用户态）+ liveness 前置检查（`dead`→`-ENODEV` 防盲发 3s 超时）；drop-new + `SYN_DROPPED` 帧感知；evdev crash 经控制 fd `.close`→`devtmpfs_remove`→remove uevent，init 扩展 respawn 镜像 udevd。删 `ring.c`/`ring.h`/`ringbuf.h`/`ringbuf_fops`/`RINGBUF_WAKE`/`RINGBUF_INJECT`/`INPUT_BIND`/`INPUT_UNBIND`/`consumers[]`/`notify()`。**消除技术债 #29（B3 大 getter 截断，broker 转发走既有 RECV_IOCTL 变长通道）与 #31（grab 僵尸锁，grab 下沉 evdev + consumer `f_op->close` 自动清理）。** 与下条 hidraw0 项逻辑独立、可并行。
- [ ] `/dev/usb_hid_kbd` 升级为真正的 `/dev/hidraw0`（Z-α 方案，详见 [../../refact_evdev.md](../../refact_evdev.md) §14）：内核 xHCI 驱动 `devtmpfs_create("usb_hid_kbd", ...)`（`kernel/driver/xhci.c:845`）硬编码单一键盘节点名。走查定稿：保留 evdev mmap 消费路径（`usb_kbd.cc` 不改），同时兑现 Linux hidraw 契约——`dev_ops` 填 `read()`（出队 8B 裸报告 + copy_to_user + 与 evdev 共享单 tail）与 `HIDIOCG*` ioctl，节点改名 `/dev/hidraw0`，ISR 追加 `__wake_up` 唤醒 `read()` 阻塞者。单读者消费型语义（evdev-mmap 与 `read()` 共享 tail，谁先读谁出队）= Linux hidraw 本意。evdev 侧唯一改动是 `evdev.cc:281` open 路径名。**与本节下条 report descriptor 项独立**——hidraw 契约骨架先行，descriptor 解析后补。
- [ ] hidraw `HIDIOCGRDESC`/`HIDIOCGRDESCSIZE` 返 ENOSYS（实现选择，**非微内核必然**，可消除技术债）：当前键盘走 USB HID **Boot Protocol**，跳过 report descriptor 解析（`usb_kbd.cc` 直接按 8B Boot report 固定布局解析 modifier+6key），故 `/dev/hidraw0` 的 `HIDIOCGRDESC*` 查询返 `-ENOSYS`（refact_evdev.md §14 D2）。Linux 真实 hidraw 有完整 HID descriptor，因其走 HID 层（`hid-input.c`）解析 report descriptor。**这是实现选择而非微内核结构必然**——补 HID 层 report descriptor 解析（从设备枚举时读 descriptor、按 descriptor 通用解析 report 而非 Boot 布局）即对齐 Linux。代价： sizable 一块工作（descriptor 解析器 + 枚举期读 descriptor + 通用 report 解析替换 Boot 布局），且当前无消费者依赖（evdev 不经 `HIDIOCGRDESC`，本 OS 无第三方 hidraw 工具），故延后。与上条 hidraw 契约骨架独立——骨架（read + RAWINFO）先行兑现，descriptor 后补不影响契约。

## 页错误 OOM 等内存 / reclaim / swapping（非常后期）

- [ ] **page fault 内存不足时的等待/回收/换页**：本期按需分页重构（详见 [../../page_fault.md](../../page_fault.md)）只做 fault 路径**可睡眠架构**（进程上下文 + rwsem 读锁可排队 schedule），`bfc_alloc_page` 失败仍直接发 SIGSEGV SEGV_MAPERR（与现状一致，对齐 Linux OOM killer 在不可回收时 SIGKILL 的退化行为）。**本期不做**的内存压力策略：① `bfc_alloc_page` 失败时不直接杀进程，而是等待内存可用/触发回收后重试；② page reclaim（LRU 淘汰用户页）；③ swapping（换出到磁盘 swap 区）。这三者构成完整的内存压力子系统，依赖 page_cache + 反向映射（rmap），工作量极大，置于路线图非常后期。本期 fault 可睡眠架构已为它们铺好路（读锁可睡眠、可 `lock_page` 等盘 I/O），届时无需再改 fault 锁模型。

## file-backed mmap + rmap / unmap_mapping_range（非常后期，与 page cache 同期）

- [ ] **page cache + address_space + i_mmap + rmap 子系统**：本期按需分页（详见 [../../page_fault.md](../../page_fault.md)）不支持普通磁盘文件 mmap（`sys_mmap` 无 FD_FILE 分支，普通文件 mmap 直接 `-EINVAL`），fault_handler 不预留 file-backed page-in 分支。要支持需引入 Linux mm 核心子系统：① page cache（`page_cache.c` 已有 `page_cache_fill`，需扩为完整 address_space）；② address_space + i_mmap 区间树（每个 inode 记录所有映射它的 VMA）；③ rmap 反向映射（page → VMA 反查）；④ file-backed fault 的 `lock_page` + 读盘 page-in。
- [ ] **ftruncate shrink 清别进程 PTE（shrink-UAF）**：当前 shm 是裸物理页数组（`shm->phys`/`page_list`），本期 `shm->lock` 只解决 fault 时元数据竞争（file_size/page_list 读写），**保护不了** ftruncate shrink 释放已被别进程映射的物理页——已建 PTE 的进程不再经 fault，shm->lock 救不了，会双重映射/UAF。正解是 Linux 的 `unmap_mapping_range`：ftruncate 释放页前经 rmap 清掉所有映射该页的 PTE。依赖上述 rmap/address_space/i_mmap 子系统，故与 file-backed mmap 同期做。本期 `pf_sigbus_shrink` 测试限定为**单进程顺序**（ftruncate→访问），不做跨进程并发 shrink。
- [ ] **`fadvise64(POSIX_FADV_DONTNEED)` 当前是 advisory no-op**（`kernel/bsd/syscall.c` `sys_fadvise64` 全 advice 返 0）。DONTNEED 应丢掉 `[offset, offset+len)` 区间内的**干净页**（脏页保留，不触发写回——区别于 `posix_fallocate`/ftruncate）。现 `page_cache_invalidate_inode` 是整 inode 粒度且不区分 clean/dirty（直接 `kfree(cp->data)`，丢脏页会丢数据），无逐 range 安全丢页 API。补 DONTNEED 需在 `page_cache.c` 新增 range-based、跳过 dirty 页的 invalidate（或先 `page_cache_flush_inode` 再丢，但那会引发写回，语义偏离 DONTNEED）。与 page cache/address_space 重构同期。`WILLNEED` 触发 readahead 同期补。`NORMAL`/`RANDOM`/`SEQUENTIAL`/`NOREUSE` 本就是纯提示，no-op 合理。

## fcntl 切换后暴露的 syscall/libc 缺口（fcntl_worklist §一/§3d 收尾）

- [ ] **`fallocate` 仅 mode=0**（`kernel/bsd/syscall.c` `sys_fallocate`：普通文件 grow+zero、SHM 委派 ftruncate；其它 mode 一律 `-EOPNOTSUPP`）。缺 `FALLOC_FL_KEEP_SIZE`/`PUNCH_HOLE`/`COLLAPSE_RANGE`/`INSERT_RANGE`/`ZERO_RANGE`/`UNSHARE_RANGE`——FAT32 连续无洞天然不支持，需 tmpfs/ext2 等有洞 fs 上线后再做。`posix_fallocate`（mode=0）已够 musl `posix_fallocate.c` 用。
- [ ] **fcntl cmds 未实现**：`sys_fcntl` switch 的 `default` 返 `-EINVAL`，下列 musl `<fcntl.h>` 声明的 cmd 内核不认：`F_SETLEASE`/`F_GETLEASE`（file lease，需 lease 子系统）、`F_NOTIFY`（目录变更通知 DN_*）、`F_CANCELLK`、`F_GETOWNER_UIDS`、`F_GET_RW_HINT`/`F_SET_RW_HINT`/`F_GET_FILE_RW_HINT`/`F_SET_FILE_RW_HINT`（write-life hint，RWF_*）。当前无消费方，留待对应子系统/需求上线。
- [ ] **libc 侧 `lockf(3)` + `fallocate(3)`（glibc 非 POSIX 变体）未提供**：musl `<fcntl.h>` 在 `_GNU_SOURCE` 下声明二者，但 musl 源在 `src/misc/lockf.c`、`src/linux/fallocate.c`，未并入 `musl_fcntl_objs`（只拉 `src/fcntl/*.c`）→ 用户态调用会链接未定义。需用时把这俩源并入 libc（或并入 `musl_unistd_objs` 的 src/linux glob）。
- [ ] **`splice`/`tee`/`vmsplice`/`readahead`/`sync_file_range` 缺失（libc + kernel 双侧）**：musl `<fcntl.h>` 在 `_GNU_SOURCE` 下声明，源在 `src/linux/*.c`（未被 `musl_unistd_objs` 的 `src/unistd` glob 覆盖，未并入）；内核无 `SYS_splice`(275)/`SYS_tee`(276)/`SYS_vmsplice`(278)/`SYS_readahead`(187)/`SYS_sync_file_range`(277) 的 dispatch（`syscall_nums.h`+`syscall.c` 均无）。需 kernel pipe/splice 子系统 + 这批 syscall + libc 源一并补。

## procfs 后续（procfs.md §6 "后续 TODO"）

procfs M0-M6 已上线（[procfs.md](procfs.md)）：`/proc/{meminfo,cpuinfo,uptime,version,self}` + per-pid `status/stat/comm/cmdline/maps/cwd/exe/fd/N`，回归测试 `test_procfs`（14 例，`user/test/test_procfs.c`）。后续缺口：

- [ ] **`/proc/[pid]/task/[tid]` 线程级**：解锁 musl `pthread_getname_np`/`pthread_setname_np`（读写 `/proc/self/task/%d/comm`）。需 per-tid 目录 + comm 字段，xtask 已有 tgid/tid 区分（`xtask.h`）。
- [ ] **切 musl 上游 `ttyname`/`ttyname_r`**：procfs `/proc/self/fd/N` 已就位，但 musl `ttyname_r` 做 `stat(路径)` vs `fstat(fd)` 的 dev+ino 交叉校验，要求 `/dev/ptsN` 解析到的 devtmpfs inode 与 tty fd `fstat` 命中同一 (dev,ino)。切之前需验证该校验精度，否则 musl 版返 ENODEV 反不如 repo 版 `TIOCGPTN` 拼 `/dev/ptsN`。届时移除 `build_script/third_party/musl/modules/unistd.cmake` 的 `ttyname.c`/`ttyname_r.c` REMOVE + 删 `user/lib/file.cc` repo 版。
- [ ] **`/proc/sys/`（可写 sysctl）**：当前 procfs 全只读。`/proc/sys/` 需可写 store 回调 + 内核参数读写收口。
- [ ] **`/proc/[pid]/environ`**：pinfo 侧表已留 `environ` 槽（`procfs_pinfo`），execve hook 当前只抓 exe/cmdline（`procfs_pinfo_set` 忽略 envp）；补 envp 抓取即可。
- [ ] **`/proc/loadavg`**：运行队列长度 + 负载均值，需调度器统计（`sched.c` run_queue 计数）。
- [ ] **seq_file（>页属性）**：当前 show 回调写单 4KB 页（`procfs_file_read` kbuf[4096]）。`/proc/[pid]/maps` 等大属性可能超页，需 seq_file 风格的多页迭代。当前 maps_show 在 VMA 多时可能截断。
- [ ] **`FD_REGULAR` 真路径反查**：`/proc/[pid]/fd/N` 对磁盘文件当前返 `anon_inode:[regular]`（`struct inode` 不存路径）。真路径需动 inode/VFS（inode 记录路径或 dentry 反查），超出 procfs 范围。procps `ls -l /proc/*/fd/*` 对磁盘文件显示 anon_inode 串，不崩。

