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
- **四层 IPC**：req/resp（≤56B 内联）+ msg/msg_resp（≤64KB 变长）+ AF_UNIX SOCK_STREAM socket（双向字节流 + SCM_RIGHTS）+ pipe（匿名单向）。详见 `doc/design/ipc.md`
- **用户态 libc**：libc.a 提供 printf/malloc/string/FILE 等标准库函数。详见 `doc/design/libc.md`
- **进程隔离**：每进程独立地址空间和 PML4，共享内核映射。详见 `doc/design/proc.md`

## 构建与运行

```bash
./build.sh                  # 编译内核 + EFI bootloader + 用户态 ELF + disk.img（Release）
./build.sh -d               # Debug 模式（-g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG）
./build.sh --test           # 测试构建（Unity 测试 ELF → disk.img 含 /test/ 目录）
./build.sh --sanitizer      # KASAN sanitizer 构建（-DSANITIZE=1，内核内存错误检测）
./build.sh --perf           # 性能计数构建（-DPERF=1）
./run.sh                     # QEMU 启动（串口→log.txt, -monitor:stdio 可 sendkey 注入键盘）
./run.sh -s                  # QEMU + GDB 远程调试服务器（端口 1234, gdb → target remote :1234）
./check.sh                   # 检查脚本（incremental vs origin/master: sparse + iwyu + clang-format + clang-tidy）
./check.sh --all             # 全量检查（所有源文件，非增量）
./check.sh --filter iwyu     # 只跑 iwyu 检查
./check.sh --filter sparse,clang-format   # 只跑 sparse + clang-format
```

- `./check.sh` 无 `--filter` 会尝试跑 clang-tidy（该脚本不存在），需用 `./check.sh --filter sparse,iwyu,clang-format` 只跑已实现的检查

**测试运行**：测试是动态 ELF（`add_user_dyn_elf`），由 `test_runner.elf`（用户态进程）逐个 `spawn` + `waitpid`。`--test` 构建后 `test_runner.elf` 写入 disk.img `/test/` 目录；在 OS 上执行 `/test/test_runner.elf` 即可运行全部 22 项测试。单测可单独 `spawn`，如 `spawn("/test/pipe.elf")`。`--test` 构建时 `#define TEST` 传递到 shell（`user/shell/shell.cc`），shell 启动前先 fork+execve `/test/test_runner.elf` 运行全部测试。

**GDB 调试**：`./run.sh -s` 启动 QEMU + GDB server；宿主机 `gdb build/myos.elf` → `target remote :1234` 连接。断点、单步、栈回溯均可。详见 `doc/design/debug.md`。

**磁盘布局**：disk.img（192MB），单盘两分区：分区1=ESP(FAT16, LBA 2048 起, 32MB，放 BOOTX64.EFI/myos.elf/init.elf)，分区2=根(FAT32, LBA 67648 起)。stub 把 init.elf 读进内存传给内核（initrd-style），内核不再用裸 LBA slot。详见 `doc/design/vfs.md`。

**libc 双产物**：libc.a（静态）与 libc.so（动态）同源构建，`#if DYNAMIC` 编译期分流。libc.so 使用 `libc.map` 版本脚本控制导出符号（`LIBC_EXPORT` 标注 + `verify_libc_exports.sh` 校验）。动态 ELF 通过 ld.so 加载 libc.so（`PT_INTERP` → `/lib/ld.so`）。详见 `doc/design/ld.md`。

**重要：** `add_library(OBJECT)` 不能设置 `POSITION_INDEPENDENT_CODE ON`，否则加 `-fPIC` 破坏 RIP-relative 寻址。

**添加新内核源文件：** `add_kernel_object(lib_name SOURCES ... ASM_SOURCES ...)`，自动加 `-mno-sse -mno-sse2 -mno-mmx`、`__KERNEL__` 定义、`POSITION_INDEPENDENT_CODE OFF`

**添加新静态用户态 ELF：** `add_user_elf(name [C] SOURCES ... [LINK_LIBS ...] [DEFS ...])`，`C` 标记表示用 C 编译器，`LINK_LIBS` 声明依赖库（如 `c` 即 libc.a），`DEFS` 添加编译定义

**添加新动态用户态 ELF：** `add_user_dyn_elf(name [C] SOURCES ... [LINK_LIBS ...] [DEFS ...])`，链接 libc.so + crt0.o，产物为动态 ELF（DT_NEEDED libc.so）

**添加 ld.so 组件：** `add_user_ldso(name SOURCES ...)`，`-fPIC -fvisibility=hidden -shared`，自带 minilibc 不链接 libc.a

**添加用户态库：** `add_user_lib(name [C] [SHARED] SOURCES ... [FLAGS ...] [OUTPUT_NAME ...])`，无 `SHARED` → `add_library(STATIC)`（libc.a）；有 `SHARED` → gcc -shared -fPIC 自定义命令（libc.so + libc.map 版本脚本）

## 启动流程

```
UEFI → BOOTX64.EFI → 加载 myos.elf + init.elf 到内存 → ExitBootServices → _start(物理地址) → enable_paging → kernel_main → init 进程（从内存加载 init.elf）
```

详见 `doc/design/boot.md`。

## 内核初始化顺序

`kernel_main` 按以下顺序初始化（不可乱序）：

1. **Xcore** (`xcore_init`)：serial_init（跨层例外，driver 层函数在 xcore 阶段调用以使 printk 可用）→ boot_info → bump/BFC/slab 分配器 → ACPI → IDT/IRQ → KASAN → sched (含 lazy RCU) → SMP (AP 启动)
2. **VFS 数据结构**：`inode_init()` → `page_cache_init()` → `devtmpfs_init()` — 直接在 `kernel_main` 中调用（不在 `bsd_init` 中），因为 driver_init 需要 `devtmpfs_create()` 注册设备
3. **Driver** (`driver_init`)：PCI ECAM → 注册四个内置驱动（ahci/xhci/display/serial）→ PCI auto-match → 设 `timer_poll_hook = xhci_poll`
4. **BSD** (`bsd_init`)：`vfs_init`（FAT32 mount）→ futex 表 → 注册七个 BSD hook（`signal_check_hook`/`reap_hook`/`proc_reap_hook`/`devtmpfs_cleanup_hook`/`syscall_dispatch_hook`/`signal_pending_hook`/`force_sig_hook`）
5. **init 进程**：从 `bi->init_elf_addr` 内存加载（EFI stub 预加载，非磁盘/AHCI 读取），因为此时才刚 mount FAT32

Xcore→BSD 通过 hook 函数指针通信（BSD 注册，Xcore 调用，NULL 默认值保证 boot 早期安全）。调用方向：BSD→Xcore 单向（via `kpi.h`），Driver→Xcore+BSD 单向，Xcore 不回调上层。Xcore 唯一跨层例外是 `serial_init()`（通过 `kernel/xcore/serial_hook.h` 声明桥接）。

## 目录结构

```
build_script/
  cmake/toolchain-x86_64.cmake
  cmake/do_link.cmake
  cmake/kernel_rules.cmake   — add_kernel_object()
  cmake/user_rules.cmake     — add_user_lib() / add_user_elf()
  linker.ld            — 64位链接脚本
  mkdisk.sh            — disk.img 打包（单盘两分区: ESP FAT16 + 根 FAT32）
CMakeLists.txt
build.sh / run.sh
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

  xcore/                  — Xcore 层：调度器、IPC、内存管理、中断分发
    CMakeLists.txt
    init.c               — xcore_init（serial_init, mem, ACPI, ISR, proc, SMP）
    sched.c / sched.h    — xtask_t, switch_to, schedule, task_reap, idle_entry
    xtask.h              — xtask_t 定义（PCB）、bsd_proc 前向声明
    proc.c               — create_process (ELF 加载+调度入队)
    trap.c / trap.h      — trap_dispatch + syscall_dispatch + IRQ注册表 + BSD hook 调用点
    serial_hook.h         — serial_init 声明（xcore 跨层调用 driver 函数的唯一例外）
    ipc.c                — IPC 原语实现（sys_recv/req/resp/msg/msg_to）
    kpi.h                — Xcore KPI 导出声明
    log.c / log.h        — printk/panic/dump_stack_trace/BUG_ON/WARN_ON/ASSERT
    rcu.c / rcu.h        — RCU 机制
    acpi.c / acpi.h      — ACPI 表解析
    atomic.h             — 原子操作
    list.h               — 内嵌双向链表
    spinlock.h           — spinlock_t
    sparse.h             — Sparse 注解（__user, __iomem, phys_addr_t, kern_vaddr_t）
    mem/
      alloc.c / alloc.h — Bump/BFC 分配器
      slab.c / slab.h   — Slab 分配器
      user_mapping.c    — 用户页映射辅助
      copy_user.c       — copy_to_user/copy_from_user
      kasan.c / kasan.h — KASAN（SANITIZE=1 条件编译）

  bsd/                    — BSD 层：POSIX 语义、VFS、syscall
    CMakeLists.txt
    init.c               — bsd_init（vfs_init, hook 注册）
    proc.c / proc.h      — bsd_proc_t, files_t, sys_fork, sys_execve, bsd_proc_reap
    syscall.c / syscall.h — BSD syscall 分发（86个 syscall，slot 20-85）
    types.h              — fd/pipe/shm/mm/file/files_t 类型定义
    signal.c             — 信号处理（force_sig, check_pending_signals）
    vfs.c / vfs.h        — VFS 层（sys_open/sys_stat/sys_mkdir/sys_unlink/sys_rmdir/sys_dev_create）
    fat32.c / fat32.h    — FAT32 内核文件系统
    inode.c / inode.h    — inode cache（hash 表+引用计数+inode_put）
    page_cache.c / page_cache.h — 4KB page cache（LRU淘汰+写回）
    devtmpfs.c / devtmpfs.h — /dev/ 内存伪文件系统（设备节点注册+open）
    elf_loader.c / elf_loader.h — ELF 加载器
    socket.c / socket.h  — AF_UNIX SOCK_STREAM + SCM_RIGHTS
    pty.c / pty.h        — PTY/TTY 子系统

  driver/                 — 驱动层：PCI 设备驱动、块设备抽象
    CMakeLists.txt
    init.c               — driver_init（pci_init, driver_register, driver_pci_match）
    registry.c / driver.h — dev_driver_t 注册表 + PCI 自动匹配
    ahci.c / ahci.h      — AHCI DMA 驱动
    xhci.c / xhci.h      — xHCI USB 主控驱动
    display.c / display.h — KMS 内核态 display（bochs-display, req flip）
    serial.c / serial.h   — COM1 串口
    pci.c / pci.h         — PCI/PCIe ECAM 枚举与 BAR 分配
    blk_dev.c / blk_dev.h — 块设备抽象层（AHCI 同步封装+spinlock）
    user_check.h          — 用户态缓冲区/指针验证

init/
  init.c              — init 进程（fork+exec 驱动 + waitpid）

user/
  CMakeLists.txt
  hello.c             — 最小用户程序
  driver/             — 用户态驱动进程（kbd_driver.cc/display.h/font.h/terminal.cc）
  shell/              — Shell 用户进程（shell.cc，ls/cat/cd/pwd/touch/mkdir + 路径执行）
  include/             — libc 头文件（stdio.h, stdlib.h, string.h, fcntl.h, time.h, unistd.h,
                         sys.h, sys/*.h, input.h, usb_hid.h, assert.h, ctype.h, dirent.h,
                         errno.h, signal.h, termios.h）
  ldso/               — 动态链接器 ld.so（dls_init.c, load_so.c, relocate.c, symtab.c,
                         link_map.c, minilibc.c, start.S — 自举重定位+加载 libc.so+跳 main）
  lib/
    stdio.cc, string.cc, start.cc, malloc.cc, file.cc, sys_ipc.cc, sys_shm.cc,
    usb_kbd.cc, time.cc, unistd.cc, sys_wait.cc, sys_mman.cc, sys_irq.cc,
    sys_device.cc, sys_process.cc, sys_pci.cc, signal.cc, ctype.c, strtol.c,
    stdlib_misc.c, uname.c, sleep.c, assert.c, crt0.S
    libc.map           — libc.so 导出符号版本脚本
  test/                — 22个测试 ELF（test_runner + 21子系统测试，全部为动态 ELF）
  lib/unity/           — Unity wrapper（源码在 third_party/Unity 子模块）

include/uapi/xos/     — 内核→用户态 ABI 头文件（syscall.h, syscall_nums.h, syscall_asm.h,
                         types.h, mman.h, shm.h, fcntl.h, stat.h, signal.h, socket.h,
                         thread.h, time.h, errno.h, ioctl.h, input.h, display.h, elf.h 等）

third_party/
  Unity/               — Unity v2.6.1 测试框架（git submodule）

utils/
  macro.h, kvformat.c, kvformat.h, CMakeLists.txt

kernel/
  efi.h               — EFI 类型定义
```

## 设计文档索引

详细设计见 `doc/design/`：

| 文档 | 内容 |
|------|------|
| `boot.md` | 启动流程（UEFI→内核→init→服务，ioperm/IOPM，孤儿收养） |
| `uefi.md` | UEFI 引导设计（EFI stub、boot_info、内存映射安全） |
| `libc.md` | libc 设计（FILE/printf/malloc/time，含时间函数） |
| `cmake.md` | 构建系统（CMake 规则、工具链、磁盘映像，含用户态构建） |
| `test.md` | 测试框架设计（Unity + test_runner） |
| `todo.md` | 项目路线图（Wayland 验收 + clang/LLVM 里程碑 + 已知 Bug） |
| `kbd.md` | USB HID 键盘驱动（用户态） |
| `driver_workflow.md` | 用户态驱动工作流 |
| `user_driver.md` | 用户态驱动详细设计 |
| `terminal.md` | Terminal/PTY/串口设计（内核 PTY + 内核串口 + 用户态 ldisc + Shell） |
| `debug.md` | 内核调试方法论与操作（诊断原则、sendkey、串口日志、GDB、tmux、栈回溯、watchdog） |
| `ld.md` | 动态链接方案（ld.so 自举+重定位+libc.so 加载，libc.a/libc.so 双产物，crt0 统一入口） |
| `code_standard.md` | 代码规范（LLVM 格式、Linux 命名、头文件引用规则、POSIX 接口命名例外） |

内核设计见 `doc/design/kernel/`：

| 文档 | 内容 | 层级 |
|------|------|------|
| `structure.md` | 内核架构（Xcore/BSD/Driver 三层分层、KPI、hook、syscall 归属） | 总体 |
| `schedule.md` | 调度器、run_queue、switch_to、idle | Xcore |
| `ipc.md` | IPC 统一设计（REQ/RESP + MSG/MSG_RESP + socket + pipe + SHM + signal） | Xcore/BSD |
| `mem.md` | 内存管理（Bump/BFC/Slab、sys_mmap/munmap、用户态 malloc） | Xcore |
| `page.md` | 分页设计（higher-half、2MB huge pages、RIP-relative、NX 保护 W^X） | Xcore |
| `smp.md` | SMP 多核（Per-CPU 数据、AP 启动、中断控制器、TSS IST 栈、锁模型） | Xcore |
| `kernel_lock.md` | 内核锁设计（spinlock + 细粒度锁模型） | Xcore |
| `sparse.md` | Sparse 注解设计 | Xcore |
| `sanitize.md` | KASAN sanitizer 设计 | Xcore |
| `kernel_exception.md` | 内核异常处理基础设施 | Xcore |
| `proc.md` | 进程管理（xtask_t/bsd_proc_t/files_t、fork/execve/exit/waitpid） | BSD |
| `syscall.md` | 系统调用（SYSCALL/SYSRET、syscall 编号表、Xcore/BSD 分发） | BSD |
| `vfs.md` | VFS + 文件系统设计（FAT32 + inode + page cache + devtmpfs） | BSD |
| `posix.md` | POSIX 接口覆盖现状 | BSD |
| `thread.md` | 多线程设计（xtask_t + proc_t + signal_struct + clone + pthread，含 TLS/FPU lazy switch/futex/两级信号） | BSD/Xcore |
| `pcie.md` | PCIe ECAM 枚举与 BAR 分配 | Driver |
| `xhci.md` | xHCI USB 控制器驱动 | Driver |
| `kms.md` | KMS 内核态驱动（display buffer 分配 + req flip + devtmpfs /dev/kms） | Driver |

## 关键陷阱

- PTE 物理地址提取必须用 `pte & 0x000FFFFFFFFFF000`（清除 bit 63 NX 位），`& ~0xFFF` 会导致越界
- `sys_notify` 入队 RECV_NOTIFY + 唤醒 WAIT_RECV；`wake_process` 只改状态不入队消息（pipe I/O 用）
- `add_library(OBJECT)` 不能设 `POSITION_INDEPENDENT_CODE ON`
- 内核编译加 `-mno-sse -mno-sse2 -mno-mmx`（内核不碰 XMM），用户态必须保留 SSE（printf/double 依赖）；若用户态出现 `-mno-sse` 说明内核 flag 泄漏到全局 `CMAKE_C_FLAGS`
- 用户程序编译加 `-I. -Iuser/include` 让自定义头文件优先
- libc.a 为 CMake target `c`，用户 ELF 通过 `LINK_LIBS c` 链接；动态 ELF 通过 `LINK_LIBS c` 链接 libc.so
- FAT32 锁获取顺序：`i_lock → fat_lock → ahci_lock`（固定，防死锁）
- inode 号 (ino) = FAT32 start_cluster（自然唯一），设备 inode 自动递增分配
- syscall 编号分两段：Xcore slot 0-19（ipc/sched/mem），BSD slot 20-85（vfs/proc/signal/socket），总 NR_SYSCALL=86
- 动态 ELF `crt0.o` 提供 `_start`（crt0.S），链接时必须第一个输入；静态/动态共用同一 crt0

## 开发备注

- **串口输出**：始终走 `-serial file:log.txt`，输出自动写入 `log.txt`。**串口输入已移除**（RX ring/ISR/read 全删）
- **QEMU monitor**：`-monitor stdio`，用 `sendkey` 注入键盘输入（替代串口输入，详见 `doc/design/debug.md`）
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

串口输出写入 `log.txt`，**串口输入已移除**。键盘输入通过 QEMU monitor `sendkey` 命令注入（`-monitor stdio` 在 QEMU shell 直接输入）。Debug 模式、GDB、tmux 自动化、sendkey 键名映射、常见错误信号等操作详见 `doc/design/debug.md`。

## 6. 技术债务

综合考量工作量和规范性、扩展性，不能一味追求简单快速，也不要过度设计。接口设计不确定时参考 Linux 实现方案，对于功能在内核态还是用户态参考 MAC 的方案。技术妥协中间状态需补充到 `doc/design/todo.md`。

## 7. 文档原则

过时内容直接删除仅保留最新状态，历史信息由 git log 记录。文档内容在自己模块文档中最详细，其它引用处只保留必要信息 + 链接。

## 8. 代码风格

全部遵循 Linux 命名标准（`snake_case` 函数/变量/类型，`SCREAMING_SNAKE_CASE` 宏/常量），`.clang-format` = LLVM 风格。**关键例外**：POSIX 接口（`open`/`O_RDONLY`/`pid_t`）和硬件 spec 符号（AHCI `PxCLB`/`PxIS`）保留原命名不改。详细命名规则、头文件引用分类、POSIX/Linux 命名例外清单见 `doc/design/code_standard.md`。
