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
| 40 | clone 未支持 flags 静默忽略 | kernel/bsd/proc.c `sys_clone` | S19 仅落地 `CLONE_PARENT` + 拒绝 `CLONE_NEW*/IO`(`CLONE_UNSUPPORTED_MASK`→`-EINVAL`)。其余 `CLONE_VFORK`/`CLONE_SYSVSEM`/`CLONE_PTRACE`/`CLONE_CLEAR_SIGHAND`/`CLONE_NEWTIME`/`CLONE_INTO_CGROUP` 静默忽略(无 ns/ptrace/sem 子系统,语义为空)。落地需对应子系统;记此避免误以为已支持。见 [../../refact_syscall/S19_process_cluster.md](../../refact_syscall/S19_process_cluster.md) §3 | [posix.md](posix.md) |
| 41 | fork/clone 路径未统一 do_fork | kernel/bsd/proc.c `sys_fork`/`sys_clone` | fork 仍独立手写(:869-1063),clone 另走一路(:1059+),代码重复且 fork 不复用 clone 的 deferred_files RCU 死锁修复。长期应抽 Linux `do_fork(flags, stack, ...)` 内部 helper 统一。S19 §4 取最小改(fork 只加 `exit_signal=SIGCHLD`),统一记此。见 [../../refact_syscall/S19_process_cluster.md](../../refact_syscall/S19_process_cluster.md) §4 | [posix.md](posix.md) |
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
- test_udevd crash respawn 重连测点（`plan_use_udev.md` U4 推迟项）：**前置基建缺失**——当前 OS 无 pid 发现机制（无 `/proc`、udevd 不写 pid 文件、`udevd_pid` 仅存 `init.c:116` 栈变量、netlink 广播 `nlmsg_pid=0`，`test_udevd` 是 udevd 兄弟进程无法发现其 pid → `kill_udevd` 不可行）。需先扩 `init.c` spawn udevd 后写 `/run/udev/udevd.pid`（respawn 时刷新），测点读此文件 kill。crash 重连本身已由 U3 终端 spin→`suspend`/`resume` 人工驱动验收（use_udev_design.md §4.2 层2）。

## 内核限制与已知缺陷（静默截断 / 固定上限类）

均为既有现状，非任一在制方案引入，但会阻塞大缓冲/大设备场景。按修复成本排序：

- `devtmpfs_getdents` 不支持分批续传（`kernel/bsd/devtmpfs.c:556`）：`ctx->pos != 0` 即返 0（`:559`），且末尾无条件 `ctx->pos = (uint64_t)-1` 标 EOF（`:642`）。`dir_emit` buffer 满 break 时直落 EOF 覆盖，故"装得下则一次返回全部、装不下则静默丢失剩余条目"。libc `readdir` 用 4096 buffer 分批拉（`user/lib/file.cc:626`），`/dev` 条目总 reclen 超 4096 即漏设备。**根因**：pos 当 EOF 标记而非"上次发到第几个节点"的游标。**修复**：pos 改节点序号游标，emit 成功 pos++、buffer 满则 break 存回 pos、下次从 pos 续传。**障碍**：root `/dev` 是两段遍历（先 `dir_list` 发子目录 `:588`、再 `dev_list` 发顶层设备 `:598`），跨段续传需统一编号（dir 段 [0,dir_count) + dev 段 [dir_count,...)），但 dev_count/dir_count 已随 devtmpfs 动态化删除——改用节点指针当 pos 或独立段计数。work2_design §3.6 明令 getdents 零改动，此为独立改进。发现于 devtmpfs 动态化 `test_dev_vfs_dynamic`（90 条 ≈ 3600 字节逼近 4096 边界）。
- `sys_read`/`sys_write` 单次上限 65536（`kernel/bsd/syscall.c:1306` read / `:960` write）：`if (len > 65536) len = 65536;` 静默截断，用户态不检查返回值会以为读/写全。阻塞大 uevent、大 props、大文件单次 I/O 场景。**修复**：移除截断改为循环 I/O 或按调用方需求放宽（对齐 Linux 无单次上限）。注意 socket/msg 已有同源 64KB 限制（`kernel/bsd/socket.h:17 MAX_SOCKET_DATA`、`kernel/xcore/ipc.c:844/:928`），属 IPC 层独立约束，不与此处 read/write 同步改。
- fd 表固定上限 `MAX_FD`（`kernel/bsd/types.h:26` `#define MAX_FD 1024`，进程 `files->fd_table[MAX_FD]` 固定数组 `:114`）：S20 由 128 提升到 1024（对齐 Linux `RLIMIT_NOFILE` soft 默认），`struct files` 增到 ~8KB/进程（堆分配，`files_create` kmalloc），proc.c 三处 fd-collect-then-put 快照（`proc_reap`/`files_put`/execve cloexec）改 `kmalloc` 避免撑爆 16KB 内核栈。仍为固定上限——fd 数超 1024 即 `EMFILE`，无法运行时扩张。对齐 Linux 动态 fd 表（`files_struct` + `fdtable` 可扩容 `krealloc` + `RLIMIT_NOFILE` 软硬限）作为独立改进。**修复**：fd_table 改 `krealloc` 动态数组 + 容量翻倍（tmpfs `ti->data`/`ti->cap` 模式可参考，`kernel/bsd/tmpfs.c:161`）。与 devtmpfs 动态化同类（静态数组→动态），但属进程 fd 子系统独立改进。见 [../../refact_syscall/S20_misc_finish.md](../../refact_syscall/S20_misc_finish.md) §5

### S20 杂项收尾延后项

S20（[../../refact_syscall/S20_misc_finish.md](../../refact_syscall/S20_misc_finish.md)）以最小代价对齐 Linux x86-64 语义；下列属"需要更大子系统才能真实生效"的项，S20 仅接常量/退化语义或显式拒绝，真实语义延后：

- **mount 权限校验**（`kernel/bsd/mount.c` `sys_mount`）：已加 `euid==0` 门（等价 `CAP_SYS_ADMIN`，复用 `kill_permitted`/`setuid` 的 euid==0 root 阶梯，非伪造能力位图）。单用户 root-default 系统下为 no-op，仅当未来引入非 root euid 时生效。引入真实 capability 模型后改为 `capable(CAP_SYS_ADMIN)`，届时 `kill_permitted`/`setuid`/`mount` 一并迁移。S20 已接 `flags` 最低语义：`MS_REMOUNT`/`MS_BIND` 显式 `-ENOSYS`（未实现，优于静默吞），`MS_RDONLY`/`MS_NOSUID`/`MS_NODEV`/`MS_NOEXEC` 接受但无生效（本 FS 无权限/执行位语义），随权限 FS 上线。`data`(arg5) 透传未做——无 fstype 消费，等真实 mount 选项需求出现时连同 `mount_root` 回调签名一起加。
- **`MS_RDONLY`/`NOSUID`/`NODEV`/`NOEXEC` 真实生效**：本 FS（FAT32/tmpfs/devtmpfs）无权限位与执行位语义，四 flag 接受为 no-op。需权限 FS + suid/exec 位模型。
- **`MAP_GROWSDOWN` 栈自动扩展**（`include/uapi/xos/mman.h`）：S20 补常量 `0x100`，接受为 no-op（本 OS 栈固定）。真实 growsdown 栈扩展需 fault handler 在栈 guard 页 fault 时向下扩展 VMA，属 S10 范围。
- **`PROT_GROWSDOWN` 栈 guard 扩展**（`kernel/bsd/syscall.c` `sys_mprotect`）：S20 接受该位（mask 掉不报错），但无栈自动扩展语义。同 `MAP_GROWSDOWN`，属 S10。
- **sparse 文件真实 hole 跟踪**（`kernel/bsd/syscall.c` `sys_lseek` `SEEK_DATA`/`SEEK_HOLE`）：S20 补 `SEEK_DATA 3`/`SEEK_HOLE 4`，退化到 Linux "无洞文件"语义（全文件=data，尾部=hole，`offset>=size` 返 `ENXIO`）。本 FS 无 sparse/punch，真实 hole 报告随 sparse FS。
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
- [ ] link/symlink/readlink：FAT32 不支持硬链接与符号链接。三条路：① 暂报 `ENOSYS`/`EPERM`（推荐短期，最干净，不污染路径解析）；② 伪符号链接——用 Windows 式 `.SYMLINK` 伪文件存目标路径，`lstat` 识别 `S_IFLNK`，路径解析时跟随（工作量中等，污染纯路径解析，违背 FAT 语义）；③ 换文件系统（ext2/TAR 等，长期）。`lstat` 在无 symlink 前提下语义等价 `stat`，可直接别名。详见 [vfs.md](kernel/vfs.md) 待完成项

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
