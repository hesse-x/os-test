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
- [ ] `modetest`/`weston` 验证

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

## VFS mount 框架 ✅

详细设计见 [kernel/mount.md](kernel/mount.md)。待完成项（sysfs 占位、ino 唯一化、子目录 getdents、path_walk 重写等）见该文档末尾。

## udevd 改读 sysfs 真文件

- [ ] udevd 改读 /sys/class/... 真文件 (sysfs_design.md 3.8)
  - udev_device_get_sysattr_value 改为 open("/sys/class/<subsys>/<dev>/<attr>") + read()
  - 废弃虚拟 syspath 内存库
  - 回改 udev_design.md 1.3/3.2.2/3.3.1 章节

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
| 29 | evdev ioctl 大 getter 数据截断(B3) | kernel/bsd/syscall.c FD_DEV req 段 | 客户端回填守卫硬限 `_IOC_SIZE<=48`、`req_reply_len=56`,完整 KEY 位图 96B 等 >48B 查询会静默截断。本轮 B1(test 传 ≤48B buf)够桩用,**接 libinput 前必须做 B3**:ioctl 段按 `_IOC_SIZE>48` 分流到 `sys_msg_to`/`sys_msg_resp` 变长通道,复用已有 MSG 机制 | [../../evdev_design.md](../../evdev_design.md) |
| 31 | evdev grab 僵尸锁 | kernel/bsd/proc.c `file_put` FD_DEV 段 | `file_put` 对 user-driver(`driver_pid>0`)不调 close,客户端崩溃/关 fd 后 `grab_client` 残留 → eventN 永久 EBUSY。FD_FILE 已有 `kernel_msg_send` close 通知范式(proc.c:220)可复用:给 user-driver close 时 `kernel_msg_send(driver, &close_req, ...)` → evdev 收 RECV_MSG 释放 grab。但改全局 close 语义(所有 user-driver 的 close 阻塞等驱动)作用域溢出 evdev 查询接口本轮,留接真实多客户端前 | [../../evdev_design.md](../../evdev_design.md) |
| 32 | VFS 路径解析为字符串前缀匹配,非逐组件遍历 | kernel/bsd/vfs.c | mount 框架用 `normalize_path`(字符串级 `.`/`..` 处理)+ `vfs_resolve`(最长前缀匹配)替代 Linux 的 `path_walk`(inode 级逐组件遍历 + `follow_dotdot` 穿越挂载边界)。无 symlink 场景下结果等价。引入 symlink 后需重写为逐组件遍历,同时可引入 dentry cache。见 [mount.md](kernel/mount.md) | [mount.md](kernel/mount.md) |
| 33 | pipe read_pid/write_pid 滞后引用致 wake_process ASSERT panic | kernel/bsd/syscall.c `pipe_write`/`pipe_read` | 读/写端 `schedule()` 返回后才清 `read_pid`/`write_pid = -1`,目标线程可能已切换到 `WAIT_FUTEX` 等非 PIPE 等待状态;另一核 `pipe_write`/`pipe_read` 仍看到旧 pid,调 `wake_process` 触发 `ev==WAIT_PIPE` ASSERT( ipc.c:648)。多核偶发。**修复**:`wake_process(p->read_pid)` → `wake_with_event(task_get(p->read_pid), WAIT_PIPE)`,内部持 `scheduler_lock` 校验 `wait_event==expected`,不匹配 no-op。write 端同理。 | [ipc.md](ipc.md) |

各 Bug 详细说明见归属文档的待完成项。已修复的 Bug（#30 socket 锁粒度）不再列出。

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
