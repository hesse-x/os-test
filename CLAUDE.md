# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

**微内核操作系统** — 64位 x86-64 higher-half 微内核，UEFI stub 引导，freestanding C/C++ + 纯汇编。内核仅提供最小机制：调度器、内存管理、中断/异常分发、系统调用接口。策略和服务尽量在用户态实现。用户态 C 程序通过 libc.a 静态链接标准库函数。

**支持 POSIX** — 复用 Linux 生态，设计理念贴合 Linux。

## 项目目标

- **支持简单 ELF 可执行文件的执行**：在宿主机编译 hello world，静态链接 libc.a，在 OS 上执行
- **支持 Wayland 核心协议**：验收目标
- **支持构建 gcc**：成功在 OS 上构建出 gcc

## 微内核设计原则

- **最小内核**：内核仅包含调度、内存管理、中断分发、系统调用、FAT32 文件系统、块设备抽象等不可替代的机制。AHCI 在内核空间，键盘等驱动在用户态运行
- **用户态服务**：shell 及部分驱动进程作为独立用户进程从磁盘加载，拥有独立地址空间，通过 syscall 和 IPC 通信
- **四层 IPC**：req/resp（≤56B 内联）+ msg/msg_resp（≤64KB 变长）+ AF_UNIX SOCK_STREAM socket（双向字节流 + SCM_RIGHTS）+ pipe（匿名单向）。详见 `doc/design/rpc.md`、`doc/design/socket.md`
- **用户态 libc**：libc.a 提供 printf/malloc/string/FILE 等标准库函数。详见 `doc/design/libc.md`
- **进程隔离**：每进程独立地址空间和 PML4，共享内核映射。详见 `doc/design/process_lifecycle.md`

## 构建与运行

```bash
./build.sh          # 编译内核 + EFI bootloader + 用户态 ELF + disk.img + boot.img
./build.sh -d       # Debug 模式（-g -fno-omit-frame-pointer，异常时栈回溯）
./build.sh --test   # 测试构建（Unity 测试 ELF + test_runner）
./build.sh --no-serial  # 禁用串口打印（NSERIAL 宏）
./run.sh            # QEMU 启动（串口输出→log.txt，串口输入需 socat 连接）
./run.sh -s         # QEMU + GDB 远程调试服务器
```

构建体系为 CMake + 自定义链接脚本。详见 `doc/design/cmake_user_build.md`。

**磁盘布局**：disk.img（64MB），LBA 0=MBR，LBA 101-200=init.elf，LBA 201+=FAT32。详见 `doc/design/fat32.md`。

**重要：** `add_library(OBJECT)` 不能设置 `POSITION_INDEPENDENT_CODE ON`，否则加 `-fPIC` 破坏 RIP-relative 寻址。

**添加新内核源文件：** `add_kernel_object(lib_name SOURCES ... ASM_SOURCES ...)`

**添加新用户态 ELF：** `add_user_elf(name [C] SOURCES ... [LINK_LIBS ...])`，`C` 标记表示用 C 编译器，`LINK_LIBS` 声明依赖库（如 `c` 即 libc.a）。

## 启动流程

```
UEFI → BOOTX64.EFI → 加载 myos.elf → ExitBootServices → _start(物理地址) → enable_paging → kernel_main（VFS/FAT32初始化）→ 用户进程
```

详见 `doc/design/boot.md`。

## 目录结构

```
build_script/
  cmake/toolchain-x86_64.cmake
  cmake/do_link.cmake
  cmake/kernel_rules.cmake   — add_kernel_object()
  cmake/user_rules.cmake     — add_user_lib() / add_user_elf()
  linker.ld            — 64位链接脚本
  mkdisk.sh            — disk.img 打包（裸 ELF + FAT32）
CMakeLists.txt
build.sh / mkimg.sh / run.sh
testdata/

boot/
  stub.c              — EFI bootloader

arch/x64/
  CMakeLists.txt
  start.S             — _start（物理→虚拟跳转）
  vectors.S           — 48个中断向量桩
  trapentry.S         — __alltraps/__trapret, syscall_fast_entry/SYSRET, switch_to, process_entry
  ap_trampoline.S     — AP 实模式→长模式 trampoline
  paging.c / paging.h — GDT, enable_paging, 4级页表, bump_alloc
  trap.c / trap.h     — IDT, PIC, PIT
  smp.c / smp.h       — Per-CPU 数据/GDT/TSS, AP 启动
  apic.c / apic.h     — LAPIC/I/O APIC, 定时器校准
  utils.h             — outb/inb, wrmsr/rdmsr, IrqGuard, syscall 内联汇编
  memlayout.h         — PAGE_SIZE/PHY_TO_PAGE 等常量

kernel/
  CMakeLists.txt
  kernel.c / kernel.h   — kernel_main
  serial.c / serial.h   — COM1 串口（NSERIAL 门控）
  trap.c / trap.h       — trap_dispatch + syscall_dispatch + 59个syscall + IRQ注册表
  proc.c / proc.h       — task_t/mm_t, switch_to, schedule, task_reap
  ahci.c / ahci.h       — AHCI DMA 驱动
  acpi.c / acpi.h       — ACPI 表解析
  pci.c / pci.h         — PCI/PCIe ECAM 枚举与 BAR 分配
  xhci.c / xhci.h       — xHCI USB 主控驱动
  display.c / display.h — KMS 内核态 display（bochs-display, req flip）
  pty.c / pty.h         — PTY/TTY 子系统（pseudoterminal master/slave）
  log.c / log.h         — printk/panic/dump_stack_trace/BUG_ON/WARN_ON/ASSERT
  sparse.h              — Sparse 注解（__user, __iomem, phys_addr_t, kern_vaddr_t）
  user_check.h          — 用户态缓冲区/指针验证
  elf_loader.c / elf_loader.h — ELF 加载器
  socket.c / socket.h   — AF_UNIX SOCK_STREAM + SCM_RIGHTS
  vfs.c / vfs.h         — VFS 层（sys_open/sys_stat/sys_mkdir/sys_unlink/sys_rmdir/sys_dev_create）
  fat32.c / fat32.h     — FAT32 内核文件系统（路径解析/读写/创建/删除/目录操作）
  inode.c / inode.h     — inode cache（hash 表+引用计数+inode_put）
  page_cache.c / page_cache.h — 4KB page cache（LRU淘汰+写回）
  blk_dev.c / blk_dev.h — 块设备抽象层（AHCI 同步封装+spinlock）
  devtmpfs.c / devtmpfs.h — /dev/ 内存伪文件系统（设备节点注册+open）
  list.h                — 内嵌双向链表
  spinlock.h            — spinlock_t
  mem/
    alloc.c / alloc.h   — Bump/BFC 分配器
    slab.c / slab.h     — Slab 分配器
    user_mapping.c      — 用户页映射辅助
    copy_user.c         — copy_to_user/copy_from_user
    kasan.c / kasan.h   — KASAN（SANITIZE=1 条件编译）

driver/
  CMakeLists.txt
  kbd_driver.cc       — 用户态键盘驱动（USB HID SHM）
  display.h           — Display 协议（req CREATE_BUF/FLIP + client API）
  font.h              — 8x16 字体
  terminal.cc         — 用户态 Terminal（VT100 + Display client）

init/
  init.c              — init 进程（fork+exec 驱动 + waitpid）

shell/
  shell.cc            — Shell（ls/cat/cd/pwd/touch/mkdir + 路径执行）

user/
  CMakeLists.txt
  hello.c             — 最小用户程序
  include/             — libc 头文件（stdio.h, stdlib.h, string.h, fcntl.h, time.h, unistd.h,
                         sys.h, sys/*.h, input.h, usb_hid.h, assert.h, ctype.h, dirent.h,
                         errno.h, signal.h, termios.h）
  lib/
    stdio.cc, string.cc, start.cc, malloc.cc, file.cc, sys_ipc.cc, sys_shm.cc,
    usb_kbd.cc, time.cc, unistd.cc, sys_wait.cc, sys_mman.cc, sys_irq.cc,
    sys_device.cc, sys_process.cc, sys_pci.cc, signal.cc, ctype.c, strtol.c,
    stdlib_misc.c, uname.c, sleep.c, assert.c
  test/                — 15个测试 ELF（test_runner + 14子系统测试）
  lib/unity/           — Unity wrapper（源码在 third_party/Unity 子模块）

third_party/
  Unity/               — Unity v2.6.1 测试框架（git submodule）

common/
  boot.h, macro.h, errno.h, syscall.h, syscall_nums.h, shm.h, dev.h, socket.h,
  elf.h, dirent.h, stat.h, ioctl.h, input.h, font_metrics.h, display.h,
  fcntl.h, mman.h, signal.h, types.h, kvformat.c, kvformat.h

kernel/
  efi.h               — EFI 类型定义
```

## 设计文档索引

详细设计见 `doc/design/`：

| 文档 | 内容 |
|------|------|
| `boot.md` | UEFI 启动流程、GDT/IDT/TSS、中断架构 |
| `uefi.md` | UEFI 引导详细设计 |
| `syscall.md` / `sys_api.md` | 系统调用编号与 API 参考 |
| `rpc.md` | REQ/RESP 同步 IPC 协议 |
| `process_lifecycle.md` | 进程创建/退出/waitpid/proc_reap |
| `fork_exec.md` | fork+execve 设计（task_t/mm_t 拆分） |
| `schedule.md` | 调度器、run_queue、switch_to、idle |
| `smp.md` | SMP 多核、Per-CPU 数据、AP 启动 |
| `mem.md` | Bump/BFC/Slab 分配器 |
| `page.md` | 地址映射、higher-half、huge pages |
| `fine_grained_lock.md` | 细粒度锁、锁协议、获取顺序 |
| `pcie.md` | PCIe ECAM 枚举与 BAR 分配 |
| `xhci.md` | xHCI USB 控制器驱动 |
| `kbd.md` | USB HID 键盘驱动 |
| `kms.md` | KMS 内核态驱动（display buffer 分配 + req flip + devtmpfs /dev/kms） |
| `terminal_split.md` | Terminal 进程设计 |
| `driver_workflow.md` | 用户态驱动工作流 |
| `user_driver.md` | 用户态驱动详细设计 |
| `file_system.md` / `fat32.md` | FAT32 文件系统（内核化，详见 vfs.md） |
| `libc.md` | 用户态 libc 设计 |
| `shm.md` | SHM fd + mmap 模型 |
| `socket.md` | AF_UNIX SOCK_STREAM + SCM_RIGHTS + poll |
| `vfs.md` | VFS 统一 I/O（FAT32 内核化 + inode + page cache + devtmpfs） |
| `dev_table.md` | 设备注册表与动态发现 |
| `dev_vfs.md` | 设备 VFS 统一路径（迁移计划） |
| `signal.md` | 信号机制设计（sigframe/EINTR/force_sig） |
| `tty.md` | PTY/TTY 子系统设计 |
| `posix.md` | POSIX 接口覆盖现状与实现方案 |
| `cmake.md` / `cmake_user_build.md` | CMake 构建系统 |
| `shell.md` | Shell 命令设计 |
| `time.md` | 时间函数 |
| `spinlock.md` | 自旋锁实现 |
| `tss_ist.md` | TSS IST 栈 |
| `nx_bit.md` | NX 位与 W^X 策略 |
| `thread.md` | 线程设计（CLONE_VM 预留） |
| `x64_migration.md` | x86-64 迁移记录 |
| `sparse.md` | Sparse 注解设计 |
| `sanitize.md` | KASAN sanitizer 设计 |
| `serial.md` | 串口设计 |
| `kernel_exception.md` | 内核异常处理基础设施 |
| `test.md` | 测试框架设计 |
| `todo.md` | 待办事项与技术债务 |

## 关键陷阱

- PTE 物理地址提取必须用 `pte & 0x000FFFFFFFFFF000`（清除 bit 63 NX 位），`& ~0xFFF` 会导致越界
- `sys_notify` 入队 RECV_NOTIFY + 唤醒 WAIT_RECV；`wake_process` 只改状态不入队消息（pipe I/O 用）
- `add_library(OBJECT)` 不能设 `POSITION_INDEPENDENT_CODE ON`
- 用户程序编译加 `-I. -Iuser/include` 让自定义头文件优先
- libc.a 为 CMake target `c`，用户 ELF 通过 `LINK_LIBS c` 链接
- FAT32 锁获取顺序：`i_lock → fat_lock → ahci_lock`（固定，防死锁）
- inode 号 (ino) = FAT32 start_cluster（自然唯一），设备 inode 自动递增分配

## 开发备注

- 串口始终走 Unix socket，输出自动写入 `log.txt`（无需手动 tee）；串口输入仅在 socat 连接 `/tmp/qemu-serial.sock` 时启用。monitor 在 stdio
- `.clang-format` = LLVM 风格
- `outb/inb/wrmsr/rdmsr/IrqGuard` 统一在 `arch/x64/utils.h`
- 驱动自注册：`dev_create("/dev/xxx", DEV_XXX, &ops)` → devtmpfs inode

# 编程指导原则

以下准则源自 Andrej Karpathy 关于 LLM 编程陷阱的观察，旨在减少常见错误。对琐碎任务自行判断。

## 1. 编码前先思考

不要假设、不要掩盖困惑、要呈现权衡。不确定时主动询问，存在多种解释时呈现所有可能而非默默选择。

## 2. 洁优先

用最少代码解决问题。不添加超出需求的特性，不为一次性代码创建抽象，不添加未要求的"灵活性"。**关键问题：资深工程师会觉得这过于复杂吗？**

## 3. 精准修改

只碰必须碰的。不"改进"相邻代码，不重构未损坏的内容，匹配现有风格。移除因自己更改产生的孤儿代码，但不主动删除既有死代码。**检验标准：每行更改都应直接追溯到用户请求。**

## 4. 目标驱动执行

定义成功标准，循环验证直到完成。对多步骤任务说明简短计划：
```
1. [步骤] → 验证: [检查]
2. [步骤] → 验证: [检查]
```

## 5. debug

### 串口打印

优先考虑串口打印定位，QEMU 初始化约 5s + 引导时间，建议等待 10s 以上。

串口输出自动写入 `log.txt`（QEMU chardev socket logfile），串口输入需通过 socat 连接 Unix socket 才可用：

```bash
# 查看日志
tail -f log.txt
# 交互式连接（同时可输入）
socat -,rawer UNIX-CONNECT:/tmp/qemu-serial.sock
```

常见错误信号：
- **Page Fault (#PF)**：检查地址映射和空指针
- **General Protection (#GP)**：检查段选择子、IOPL、MSR 访问
- **Triple Fault (#DF)** → QEMU 重启：通常是 TSS IST 栈或 IDT 未正确设置

### Debug 模式（栈回溯）

`./build.sh -d` 启用 `-g -fno-omit-frame-pointer`，异常时打印完整寄存器 + RBP 链栈回溯（最多 16 帧）。

```bash
./build.sh -d
./run.sh            # 串口输出自动写 log.txt
cat log.txt            # 找 BACKTRACE 段
addr2line -e build/myos.elf -f -C 0xFFFFFFFF8010XXXX
```

### GDB 远程调试

```bash
./run.sh -s            # 启用 GDB 服务器
gdb -ex "target remote localhost:1234" build/myos.elf
```

### tmux + QEMU + GDB 自动化调试

```bash
rm -f log.txt
tmux new-session -d -s qemu './run.sh -s 2>&1'
tmux new-session -d -s serial 'socat -,rawer UNIX-CONNECT:/tmp/qemu-serial.sock'
tmux new-session -d -s gdb 'gdb -ex "target remote localhost:1234" build/myos.elf'
tmux send-keys -t gdb 'continue' Enter
sleep 20
tmux send-keys -t gdb '' C-c          # Ctrl-C 中断
tmux send-keys -t gdb 'bt' Enter
tmux capture-pane -t gdb -p
addr2line -e build/init.elf -f -C 0x400245  # 用户态地址解析
# 向串口发送输入（需 serial session）
tmux send-keys -t serial 'ls' Enter
# 查看串口日志
cat log.txt
tmux kill-session -t gdb; tmux kill-session -t serial; tmux kill-session -t qemu
```

注：tmux send-keys 只能发按键到对应 session 的 stdio。QEMU monitor 在 qemu session 的 stdio，串口输入需通过 serial session 的 socat 连接发送。

## 6. 技术债务

综合考量工作量和规范性、扩展性，不能一味追求简单快速，也不要过度设计。接口设计不确定时参考 Linux 实现方案，对于功能在内核态还是用户态参考 MAC 的方案。技术妥协中间状态需补充到 `doc/design/todo.md`。

## 7. 文档原则

过时内容直接删除仅保留最新状态，历史信息由 git log 记录。文档内容在自己模块文档中最详细，其它引用处只保留必要信息 + 链接。
