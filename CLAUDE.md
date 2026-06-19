# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

**微内核操作系统** — 64位 x86-64 higher-half 微内核，UEFI stub 引导，freestanding C/C++ + 纯汇编。内核仅提供最小机制：调度器、内存管理、中断/异常分发、系统调用接口。策略和服务尽量在用户态实现（shell 等用户进程从磁盘加载为独立 ELF64 运行）。C++ 编译为 `-fPIE`，使用 RIP-relative 寻址。`_start` 和 `enable_paging` 在物理地址运行，设置分页后跳转到 `kernel_main` 在虚拟地址 0xFFFFFFFF80100000 运行。用户态 C 程序通过 libc.a 静态链接 printf/malloc/string 等标准库函数。

## 项目目标

-**支持简单elf可执行文件的执行**: 在宿主机上编译hello world，静态链接os中的libc.a（含printf），在os上执行
-**支持Wayland核心协议**: 验收目标，
-**支持构建gcc**：成功在os上构建出gcc

### 微内核设计原则

- **最小内核**：内核仅包含调度、内存管理、中断分发、系统调用等不可替代的机制。AHCI 驱动在内核空间（`kernel/ahci.cc`），键盘驱动在用户态运行（`driver/kbd_driver.cc`），KMS 渲染驱动在用户态运行（`driver/kms_driver.cc`），内核仅保留 `init_fb` 做 framebuffer 物理页映射和元信息保存。
- **用户态服务**：shell 等应用及驱动进程作为独立用户进程从磁盘加载，拥有独立地址空间（每进程 PML4），通过 syscall 和共享页与内核及其他进程通信。文件系统服务（fs_driver）作为用户态进程运行，通过 sys_msg IPC 与客户端通信，内部通过 sys_block_read/write 访问磁盘。
- **系统调用 + 共享内存 IPC + fd/pipe + REQ/MSG**：34 个 syscall（NR_SYSCALL=34，编号 0-33 连续无空洞）：getpid/yield/recv/req/resp/irq_bind/exit/waitpid/spawn/mmap/munmap/serial_write/fb_info/shm_create/shm_attach/pipe/write/read/close/load_dev/lookup_dev/notify/gettime/clock/msg/msg_resp/ioperm/dup2/fcntl/dma_alloc/dma_free/pci_dev_info/block_read/block_write。双层同步 IPC：sys_req/sys_resp（≤56B 内联载荷，控制信令）+ sys_msg/sys_msg_resp（变长≤64KB，内核 kmalloc 中转，数据传输如文件 I/O）。所有驱动间 IPC 统一使用动态共享内存（sys_shm_create/sys_shm_attach）+ sleeping flag + sys_recv/sys_notify 同步，详见 `doc/design/dynamic_shm_migration.md`。驱动间通过 `sys_lookup_dev(dev_type)` 动态发现对端 PID（`common/dev.h` 定义设备类型），不再依赖硬编码 PID 常量，详见 `doc/design/dev_table.md`。同步 REQ 通过 sys_req/sys_resp 实现（56 字节载荷），键盘驱动 bind/unbind 使用此机制，详见 `doc/design/rpc.md`。所有输出通过 fd 1 (stdout pipe) → terminal → display SHM → KMS flip 路径完成。用户进程间 I/O 通过 pipe（sys_pipe/sys_write/sys_read/sys_close）+ fd 继承（sys_spawn 自动继承 fd 0/1）。新增功能应优先考虑扩展 syscall 接口而非在内核内实现策略。
- **用户态 libc**：libc.a 静态库（CMake target `c`，输出 `libc.a`）提供 printf/fprintf/vfprintf/fputc/fputs/puts/fflush、FILE 结构体（缓冲输出 + write_fn 抽象）、stdin（read_fn 抽象 + fgetc/getchar）、strlen/strcmp/strcpy/strcat/strchr/memcpy/memset/memmove、malloc/free/calloc/realloc、`_start` 入口（调用 main → sys_exit）、open/read/write/close（基于 sys_msg 文件 I/O）、timespec_get/clock。stdout 为行缓冲（_IOLBF, 256B），write_fn 为 `sys_write_flush`（调用 sys_write(fd, ...) 写 pipe + 串口镜像）；stderr 为无缓冲；stdin 的 read_fn 为 `sys_read_fill`（调用 sys_read(fd, ...) 读 pipe）。用户程序（hello.c）通过 `add_user_elf` 链接 libc。设计文档 `doc/design/libc.md`。
- **进程隔离**：每个用户进程拥有独立地址空间（代码 0x400000 起，堆 mmap 从 0x800000 起，栈 0x00007FFFFFFFD000），PML4[511] 共享内核映射，其余独立。动态共享内存虚拟地址从 0x510000 起（sys_shm_create/sys_shm_attach 分配）。每进程有内核 fd_table[32]（fd 0=stdin, fd 1=stdout, 其余 FD_NONE，type=FD_PIPE），通过 sys_pipe/sys_write/sys_read/sys_close 管理 pipe 通道。libc 维护用户态 fd_table[32]（type=FD_PIPE/FD_FILE），FD_FILE 类 fd 通过 sys_msg 与 fs_driver 通信。每进程有 recv_buf[16][64] 消息队列（sys_recv 接收 IRQ/REQ/NOTIFY/MSG 四类消息）和 REQ 状态（req_caller_pid/req_reply_buf/req_result/req_target_pid）及 MSG 状态（msg_caller_pid/msg_reply_buf/msg_reply_len/msg_result/msg_target_pid）。

## 构建与运行

```bash
./build.sh          # CMake 编译内核 + EFI bootloader + 用户态 ELF + 生成 disk.img + boot.img
./build.sh -d       # Debug 模式（加 -g -fno-omit-frame-pointer，异常时打印栈回溯）
./run.sh            # QEMU 启动（OVMF UEFI, 512MB, VGA, -smp 2, 串口输出到 log.txt）
```

构建体系为 CMake + 自定义链接脚本（`build_script/cmake/do_link.cmake`）。内核和用户态统一由 CMake 管理，编译规则封装在 `build_script/cmake/kernel_rules.cmake`（`add_kernel_object`）和 `build_script/cmake/user_rules.cmake`（`add_user_lib` + `add_user_elf`）。内核 C++ 使用 `-fPIE -mno-red-zone -mno-sse -mno-sse2 -mno-mmx`，C 使用 `-fno-pic -fno-pie -mno-red-zone -mno-sse -mno-sse2 -mno-mmx`，纯汇编用 `-m64`。内核链接：`ld -m elf_x86_64 -T build_script/linker.ld`。用户态编译 flags：`-m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-sse2 -mno-mmx`，链接：`ld -m elf_x86_64 -Ttext 0x400000`。用户态 ELF 通过 `add_user_elf` 自动执行 compile → objcopy（strip .note.gnu.property）→ ld 三步管线。详细设计见 `doc/design/cmake_user_build.md`。

磁盘映像生成：`build_script/mkdisk.sh` 生成 `disk.img`（裸 ELF + FAT32），`mkimg.sh` 生成 `boot.img`（EFI 启动盘）。

**磁盘布局（disk.img, 64MB, 131072 扇区）**：
```
LBA 0:       MBR 分区表（sfdisk 创建）
LBA 1-100:   未使用（间隙）
LBA 101-200: fs_driver.elf（100 扇区/50KB）
LBA 201-300: init.elf（100 扇区/50KB）
LBA 301+:    FAT32 分区（type=0x0C，4KB 簇，~63.5MB）
```
MBR 分区1 (type=0xDA) 覆盖 LBA 1-300（裸 ELF 存储），分区2 (type=0x0C) 覆盖 LBA 301-131071（FAT32 文件系统，4KB/簇，目录结构见 `doc/design/fs_restructure.md`）。内核 `ahci_read_lba` 直接读裸扇区加载 ELF，fs_driver 通过 MBR 分区表找到 FAT32 分区起始 LBA。kbd_driver/kms_driver/terminal/shell 等用户进程由 init 从 FAT32 加载。

**重要：** `add_library(OBJECT)` 不能设置 `POSITION_INDEPENDENT_CODE ON`，否则会加 `-fPIC`，破坏内核的 RIP-relative 寻址。

**添加新内核源文件：** 在对应目录的 CMakeLists.txt 中调用 `add_kernel_object(lib_name SOURCES ... ASM_SOURCES ...)` 即可。

**添加新用户态 ELF：** 在对应目录的 CMakeLists.txt 中调用 `add_user_elf(name [C] SOURCES ... [LINK_LIBS ...])` 即可。`C` 标记表示使用 C 编译器，`LINK_LIBS` 声明依赖的库（如 `c` 即 libc.a）。

## 目录结构

```
build_script/
  cmake/toolchain-x86_64.cmake
  cmake/do_link.cmake
  cmake/kernel_rules.cmake   — add_kernel_object() 内核编译规则封装
  cmake/user_rules.cmake     — add_user_lib() / add_user_elf() 用户态编译规则封装
  linker.ld            — 64位链接脚本
  mkdisk.sh            — disk.img 打包脚本（裸 ELF + FAT32）
CMakeLists.txt
build.sh / mkimg.sh / run.sh
testdata/
  README               — FAT32 测试文件
.clang-format / .gitignore

boot/
  stub.c              — EFI bootloader（GNU-EFI），读取 myos.elf + GOP + RSDP + MADT，ExitBootServices 后跳转内核

arch/x64/
  CMakeLists.txt
  start.S            — 纯汇编入口 _start（物理地址 → 虚拟地址跳转）
  vectors.S          — 48个中断向量桩（syscall 由 SYSCALL 指令直接进入 syscall_fast_entry）
  trapentry.S        — __alltraps/__trapret, syscall_fast_entry/SYSRET返回, switch_to, process_entry, reload_cs
  ap_trampoline.S    — AP 16位实模式 trampoline（物理 0x8000，实模式→长模式→ap_entry_c）
  paging.cc / paging.h — GDT(8项+TSS), enable_paging, 4级页表, bump_alloc, extend_mapping, flush_tlb
  trap.cc / trap.h   — IDT(256项), PIC, PIT 初始化
  smp.cc / smp.h     — Per-CPU 数据/GDT/TSS, AP 启动, smp_init_cpu, smp_apply_cpu, smp_boot_aps
  apic.cc / apic.h   — LAPIC/I/O APIC 初始化, MMIO 访问, 定时器校准, pic_disable
  utils.h            — outb/inb/inw, wrmsr/rdmsr, KERNEL_CS, L16/H16, memcpy, serial_early_out, IrqGuard, 底层 syscall 内联汇编（__syscall0 等）
  memlayout.h        — 内存布局常量（PAGE_SIZE/PAGE_SHIFT/PAGE_SIZE_2M/PHY_TO_PAGE/GET_PAGE_NUM），内核/用户态共享

kernel/
  CMakeLists.txt
  kernel.cc / kernel.h — kernel_main（acpi_init → isr_init → pci_init → ahci_init → 加载 fs_driver + init → schedule）
  serial.cc / serial.h — COM1 串口驱动
  trap.cc / trap.h   — trap_dispatch（用户态异常→sys_exit替代halt）+ syscall_dispatch + 34个系统调用（含 sys_recv/sys_req/sys_resp/sys_msg/sys_msg_resp/sys_exit/sys_waitpid/sys_spawn/sys_pipe/sys_write/sys_read/sys_close/sys_notify/sys_gettime/sys_clock）+ IRQ 注册表 + irq_owner[] + dev_table[]。sys_recv 统一接收 IRQ/REQ/NOTIFY/MSG 四类消息（4参数签名：msg, data_buf, data_buf_len, timeout_ms）；sys_notify 入队 RECV_NOTIFY 并唤醒 WAIT_RECV/WAIT_CHILD
  proc.cc / proc.h   — PCB, switch_to, process_create_elf, schedule, proc_reap（进程资源回收，PTE 物理地址提取用 `pte & 0x000FFFFFFFFFF000` 清除 NX 位）。proc_t 含 fd_table[MAX_FD]，proc_reap 清理 fd（pipe ref_count-- + wake_process + kfree）+ CPU 时间记账（cpu_time_ns/last_sched）
  fb.cc / fb.h       — Framebuffer 初始化（init_fb 映射 + g_fb_info 元信息保存），渲染已移至用户态 KMS 驱动
  ahci.cc / ahci.h     — AHCI DMA 驱动（ICH9 SATA，ECAM 配置，启动时加载 ELF + sys_block_read/write 服务用户态）
  list.h             — 内嵌双向链表（list_node_t, list_push_back/remove/empty/front, LIST_ENTRY）
  spinlock.h         — spinlock_t, spin_lock/unlock, spin_lock_irqsave/unlock_irqrestore
  mem/
    CMakeLists.txt
    alloc.cc / alloc.h — init_mem, Bump/BFC 分配器, Page 描述符(union: bfc/slab), bfc_lock, page_to_phys/phys_to_virt, 用户页映射函数声明
    slab.cc / slab.h   — 内核 slab 分配器(kmalloc/kfree/kcalloc/krealloc), kmem_cache_t, per-CPU active slab
    user_mapping.cc    — ensure_pd, ensure_pt_in_pd, map_user_page_direct, map_user_pages, unmap_user_pages

driver/
  CMakeLists.txt
  kbd_driver.cc      — 用户态键盘驱动进程（IOPL=0, shm_attach_kernel USB HID SHM, get_keycode, kbd_shm, sys_notify terminal, device_register(DEV_KBD)）
  kms_driver.cc      — 用户态 KMS 显示驱动进程（IOPL=0, display_backend_init 创建 display SHM, display_backend_poll/wait flip 循环, framebuffer 直写 0x700000）
  display.h          — Display 协议（display_shm_header + client API + backend API），terminal 和 KMS 共享
  font.h             — 8x16 字体位图（font8x16[96][16]），从 KMS/terminal 提取的共享资源
  terminal.cc        — 用户态 Terminal 进程（IOPL=0, display_client_init 渲染到 back buffer, kbd_ring 读 + stdin pipe 写, stdout pipe 读 + VT100 状态机 + cell 缓冲区 + flush_dirty_cells, device_lookup(DEV_KBD) + req(KBD_REQ_BIND) 绑定键盘）
  fs_driver.cc       — 用户态 FAT32 文件系统驱动（IOPL=0, sys_block_read/write 访问磁盘, sys_msg/sys_msg_resp 处理文件请求, 多客户端 session, readdir/open/read/close/raw_read/touch/mkdir, device_register(DEV_FS)）

init/
  CMakeLists.txt
  init.c             — 用户态 init 进程（PID 3），等待 fs_driver 就绪后依次 spawn kbd_driver/kms_driver/terminal，然后循环 waitpid(-1) 收养并回收孤儿进程

shell/
  CMakeLists.txt
  shell.cc           — Shell 用户进程（ls/cat/cd/pwd/touch/mkdir 命令 + 路径执行替代 run 命令, fd 0 stdin 输入, fd 1 stdout 输出, sys_msg 与 fs_driver 通信, 裸扇区 r 命令）

user/
  CMakeLists.txt
  hello.c           — 最小用户程序（#include <stdio.h> + #include <time.h> + main + printf + timespec_get 计时）
  include/
    stdio.h         — 用户态 C 标准库头文件（FILE/printf/fprintf/vfprintf/fputc/fputs/puts/fflush/stdin/fgetc/getchar + 缓冲模式常量）
    stdlib.h         — 用户态 C 标准库头文件（malloc/free/calloc/realloc/exit + EXIT_SUCCESS/EXIT_FAILURE）
    string.h         — 用户态字符串操作头文件（strlen/strcmp/strncmp/strcpy/strncpy/strcat/strchr/memcpy/memset/memmove）
    fcntl.h          — 文件控制（O_RDONLY/O_WRONLY/O_RDWR + open 声明）
    time.h           — 时间函数（time_t/clock_t/timespec/CLOCKS_PER_SEC/timespec_get/clock）
    unistd.h         — POSIX 封装（getpid/_exit/read/write/close/pipe/open/sched_yield）
    sys.h            — 用户态 syscall 封装头文件（req_call 声明）
    sys/ipc.h        — IPC 封装（notify/recv/req/resp/msg/msg_resp，带 errno 设置）
    sys/shm.h        — SHM 封装（shm_create/shm_attach/shm_attach_kernel 声明）
    input.h          — 键盘输入定义（key_event 结构体、input_key 枚举、修饰键、KBD_REQ_BIND/UNBIND 请求/回复结构体）
    usb_hid.h        — USB HID SHM 常量 + get_keycode/get_keycode_init 声明（引用 common/shm.h 结构体）
  lib/
    stdio.cc        — FILE 实现 + stdout（line-buffered, sys_write_flush 写 pipe + 串口镜像）+ stderr（无缓冲）+ stdin（sys_read_fill 读 pipe）+ vfprintf 格式化引擎
    string.cc       — 字符串和内存操作函数实现
    start.cc        — libc _start 入口点（fflush(stdout) → main → fflush(stdout) → sys_exit）
    malloc.cc        — 用户态 malloc/free/calloc/realloc 实现（size-class slab + sys_mmap）
    sys.cc           — req_call() 封装（委托 sys_req，56 字节载荷限制）
    file.cc          — libc 文件 I/O（open/read/write/close/pipe via sys_msg + 用户态 fd_table[32] 分发 FD_PIPE/FD_FILE）
    sys_ipc.cc       — IPC 封装（notify/recv/req/resp/msg/msg_resp，带 errno 设置）
    sys_shm.cc       — shm_attach/shm_create 封装 + shm_attach_kernel（内核 SHM ID 查找，arg2 mode=1）
    usb_kbd.cc       — USB HID keyboard get_keycode 实现（SHM ring 消费 + HID report 对比 + keycode 映射）
    time.cc          — timespec_get（sys_gettime 纳秒→timespec）+ clock（sys_clock 纳秒→微秒）
  malloctest.c     — malloc/free 测试程序（小分配/大分配/calloc/realloc，链接 libc，输出 ELF 名 malloc.elf）

common/
  common.h            — kernel_end 声明
  macro.h             — ALIGN_UP/ALIGN_DOWN 通用对齐宏
  errno.h             — 错误码定义（EOK/EPERM/ENOENT/ENOMEM/EINVAL/ENOSYS/ECHILD/EFAULT/EEXIST/EBUSY/ESRCH/ETIMEDOUT/EBADF/EMFILE/EISDIR/ENOTDIR/EIO）
  elf_loader.cc / elf_loader.h — ELF64 静态二进制加载器（elf_load_result, elf_load）
  syscall.h           — syscall 编号（SYS_GETPID 等，NR_SYSCALL=34）+ 语义封装 + sys_recv/sys_req/sys_resp/sys_notify/sys_msg/sys_msg_resp/sys_gettime/sys_clock/sys_ioperm/sys_dup2/sys_fcntl/sys_dma_alloc/sys_dma_free/sys_pci_dev_info/sys_block_read/sys_block_write
  shm.h               — 共享内存 IPC 协议定义（disk_req_shm, disk_resp_shm, driver_shm_header, kbd_ring, fs_dirent, fs_req_request/fs_req_reply, kms_fb_info, offset 常量, USB HID SHM 结构体 usb_hid_slot/usb_hid_shm_header + 常量）
  dev.h               — 设备类型定义（DEV_NONE/DEV_DISK/DEV_KBD/DEV_KMS/DEV_FS/DEV_TERMINAL），用于 sys_load_dev/sys_lookup_dev

kernel/
  efi.h               — EFI 类型定义（memory_descriptor, system_table, GOP, GUID 等）

tutorial/
  boot/
    boot.bin          — 引导教程示例（512字节启动扇区）
```

## 设计文档参考

设计文档位于 `doc/design/`，涵盖各子系统详细设计：

| 文档 | 内容 |
|------|------|
| `boot.md` | UEFI 启动流程细节 |
| `syscall.md` | 系统调用设计与编号 |
| `sys_api.md` | 系统调用 API 参考 |
| `rpc.md` | REQ/RESP 同步 IPC 协议 |
| `process_lifecycle.md` | 进程生命周期管理 |
| `schedule.md` | 调度器设计 |
| `smp.md` | SMP 多核启动与 per-CPU 数据 |
| `mem.md` | 内存管理架构 |
| `page.md` | 分页与地址映射 |
| `fine_grained_lock.md` | 细粒度锁设计与锁协议 |
| `pcie.md` | PCIe ECAM 枚举与 BAR 分配 |
| `xhci.md` | xHCI USB 控制器驱动设计 |
| `kbd.md` | USB HID 键盘驱动 |
| `kms.md` | KMS 显示驱动与 Display 协议 |
| `terminal_split.md` | Terminal 进程拆分设计 |
| `driver_workflow.md` | 用户态驱动工作流 |
| `file_system.md` | FAT32 文件系统驱动 |
| `fat32.md` | FAT32 磁盘布局 |
| `libc.md` | 用户态 libc 设计 |
| `dynamic_shm_migration.md` | 动态共享内存 IPC 迁移 |
| `dev_table.md` | 设备注册表与动态发现 |
| `cmake.md` / `cmake_user_build.md` | CMake 构建系统设计 |
| `shell.md` | Shell 命令设计 |
| `time.md` | 时间函数设计 |
| `spinlock.md` | 自旋锁实现 |
| `tss_ist.md` | TSS IST 栈设计 |
| `nx_bit.md` | NX 位与 W^X 策略 |
| `uefi.md` | UEFI 数据类型参考 |
| `x64_migration.md` | x86-64 迁移记录 |
| `todo.md` | 待办事项与技术债务 |
| `screen_plan.md` | 屏幕渲染计划 |
| `boot.md` | 启动细节 |

## 启动流程

```
UEFI 固件 → BOOTX64.EFI (boot/stub.c) → 加载 myos.elf 到 0x100000 → ExitBootServices → _start (arch/x64/start.S, 物理地址) → enable_paging → gdt_init → lretq → _entry64 (虚拟地址) → kernel_main → shell 用户进程
```

1. UEFI 固件加载 FAT32 上的 `\EFI\BOOT\BOOTX64.EFI`
2. `efi_main`（stub.c）：打开 `myos.elf`，加载 ELF64 段到物理 0x100000，读取 GOP/RSDP，解析 ACPI MADT（LAPIC/IOAPIC 基地址、CPU APIC ID），填充 `boot_info`，调用 `ExitBootServices`，跳转到 `_start`
3. `_start`（start.S）：设置物理栈 → 保存 `boot_info*`（r12）→ 调用 `enable_paging`（构建 4 级页表 + 加载 CR3）→ 调用 `gdt_init`（8项 GDT + TSS + ltr）→ `lretq` 跳转到 `_entry64`
4. `_entry64`：切换到虚拟地址栈 → 复制 `boot_info` 到 `g_boot_info` → 调用 `kernel_main`
5. `kernel_main`：`init_mem` → `acpi_init`（RSDP→XSDT→MADT+MCFG）→ `isr_init`（内含第二次 `gdt_init`，BSP 的 `smp_init_cpu` + `smp_apply_cpu`，`enable_nx`，`idt_install` + `setup_syscall`，`apic_init`）→ `kernel_init_finish`（禁用 bump）→ `slab_init`（初始化 9 个 kmem_cache_t）→ `proc_init` → `clear` → `smp_boot_aps`（启动所有 AP）→ `pci_init`（ECAM 枚举 + BAR 映射）→ `ahci_init`（ICH9 AHCI 控制器初始化）→ 从磁盘加载 fs_driver.elf（LBA 101）→ init.elf（LBA 201）→ 栈切换进入 idle_entry → idle 循环（schedule + hlt）。kbd_driver/kms_driver/terminal/shell 等进程由 init 从 FAT32 加载。

注意：GDT 被初始化两次 — 一次在 start.S 物理地址阶段，一次在 `isr_init` 中虚拟地址阶段。

## 地址映射

- 物理 0-1GB → 虚拟 0-1GB（identity map，PML4[0] → PDPT_ident）
- 物理 0-1GB → 虚拟 0xFFFFFFFF80000000-0xFFFFFFFFC0000000（higher-half，PML4[511] → PDPT_hh[510]）
- 使用 2MB huge pages（PD 级别 PS=1），初始映射覆盖 1GB。内核 huge page 不标 NX（代码数据混合）
- PTE 标志常量：`PTE_PRESENT`/`PTE_RW`/`PTE_USER`/`PTE_PS`/`PTE_NX`（定义在 `paging.h`）
- 用户栈页映射时加 `PTE_NX`（不可执行），用户代码页可执行，动态 SHM 页加 `PTE_NX`
- `extend_mapping` 动态扩展：每 1GB 物理块对应 PDPT_hh[510+n]
- 设备映射区：`device_vma_base = ALIGN_UP(VMA_BASE + max_phys_addr, 1GB)`，framebuffer 等映射到此区域
- 地址转换：`vaddr = paddr + VMA_BASE`，`PHY_ADDR(vaddr) = vaddr - VMA_BASE`
- VMA_BASE = `0xFFFFFFFF80000000`，KERNEL_VMA_BASE = `0xFFFFFFFF80100000`

## 链接脚本（build_script/linker.ld）

VMA=0xFFFFFFFF80100000，LMA 用 `AT(ADDR(.section) - 0xFFFFFFFF80000000)` 指定：

`.text` → `.rodata` → `.data`（含 GOT）→ `.got` → `.bss`(4KB对齐)

导出符号：`kernel_end`（内核映像结束地址）

## RIP-relative 寻址

`-fPIE` 在 x86-64 使用 RIP-relative 寻址：
- 所有符号访问通过 `[rip + offset]` 形式，无需 GOT 间接
- 物理地址运行时 RIP-relative 自动给出物理地址；虚拟地址运行时自动给出虚拟地址
- 不需要 GOT fixup，编译器/链接器自动处理
- 纯汇编文件需手动使用 `symbol(%rip)` 或计算偏移

## 中断架构

- **GDT**：8项（null/code64/data/user_code32_compat/user_data/user_code64/TSS_low/TSS_high），在 `arch/x64/smp.cc` 的 `smp_init_cpu` + `gdt_init` 中设置，通过 `lgdt` + `reload_cs`(lretq) 加载
- **TSS**：64位格式（128字节），填充 RSP0 + IST1/2/3（NMI/Double Fault/Machine Check），占两个 GDT slot
- **IDT**：256项，`arch/x64/trap.cc` 的 `idt_install` 安装门描述符（16字节/项，selector=0x08），支持 IST index 参数
- **向量桩**：`arch/x64/vectors.S` 定义 vector0-47，CPU异常自动push error code的向量（8,10-14,17）不push dummy 0，其余push 0+向量号。Syscall 不走 IDT，由 SYSCALL 指令直接跳转 MSR_LSTAR
- **trapentry.S**：`__alltraps` 条件 swapgs（CS==0x2B 时）→ 保存全部16个GP寄存器→加载内核DS/ES→调用 `trap_dispatch(rdi=&trapframe)`；`__trapret` 条件 swapgs → 恢复→`iretq`。**BKL 已移除**，不再有 kernel_lock_acquire/release 调用
- **SYSCALL/SYSRET**：`syscall_fast_entry`（swapgs → 读 `tss_rsp0` 切栈 → 构建 trapframe → call `syscall_dispatch`），SYSRET 返回（恢复寄存器 → RCX=rip/R11=rflags → swapgs → sysretq）。调用约定：RAX=syscall#，args=RDI/RSI/RDX/R10/R8/R9。无 int 0x80 fallback
- **IRQ 注册表与 irq_owner**：`kernel/trap.cc` 维护 `irq_handler_t` 数组和 `irq_owner[]` 数组。`register_irq(vec, fn)` 注册处理器。`trap_dispatch` 优先检查 `irq_owner[vector]`（`__atomic_load_n` ACQUIRE）：若该 IRQ 已绑定到用户进程（通过 `sys_irq_bind`），则获取 `scheduler_lock` 唤醒该进程（入队 run_queue + run_count++）并发送 EOI；否则查 `irq_handlers[]` 注册表调用处理器
- **IrqGuard**：`arch/x64/utils.h` 中的 RAII 类，构造时 `local_irq_save`，析构时 `local_irq_restore`，用于 fb.cc 等需要中断安全操作的场景
- **PIC→APIC 切换**：`isr_init` 中 `apic_init()` 调用 `pic_disable()` 禁用旧 PIC，改用 LAPIC 定时器（~100Hz 周期模式）和 I/O APIC 路由外部中断。BSP 初期仍由 PIT 驱动（用于 LAPIC 校准），校准后切换到 LAPIC 定时器
- **trap分发**：向量128=syscall→`syscall_dispatch`；irq_owner 优先→若已绑定用户进程则 `scheduler_lock` 唤醒+EOI；否则查注册表→默认：向量32=定时器（`tf->cs == 0x2B` 时调 `schedule()`，内核态仅 EOI），其他 IRQ 仅 EOI，CPU 异常=串口诊断+用户态(`tf->cs==0x2B`)调`sys_exit(-1)`替代halt、内核态仍halt
- **TSS IST**：每 CPU 3 个独立 IST 栈（4KB），IST1=NMI(#2)、IST2=Double Fault(#8)、IST3=Machine Check(#18)，在 `smp_init_cpu()` 中分配并填入 TSS
- **NX 位**：`enable_nx()` 设置 CR4.NXDE + EFER.NXE，PTE 标志常量（PTE_PRESENT/PTE_RW/PTE_USER/PTE_PS/PTE_NX），用户栈页映射时加 PTE_NX

## 锁架构（细粒度锁，BKL 已移除）

BKL（大内核锁）已被完全移除，替换为以下细粒度锁：

| 锁 | 粒度 | 保护对象 | irqsave? |
|----|------|---------|----------|
| `scheduler_lock` | per-CPU（`cpu_local_t` 内） | run_queue + `procs[i].state`（assigned_cpu==本 CPU） + `run_count` + `schedule()` | **是**（timer_handler 会争） |
| `procs_lock` | 全局 | `procs[]` 空闲槽位分配（pid==-1）+ PCB 初始化 + PCB 槽位释放（proc_reap） | 否 |
| `irq_owner[]` | 无锁 | `__atomic_store_n` 写 / `__atomic_load_n` 读 | N/A |
| `recv_lock` | per-process（`proc_t` 内） | per-process recv 队列（recv_buf/recv_head/recv_tail） | 否 |

新增 syscall 锁交互（exit/waitpid/spawn）：

| 操作 | 持锁 | 说明 |
|------|------|------|
| sys_exit 设 ZOMBIE | scheduler_lock[child->assigned_cpu] | 保护 state + run_count |
| sys_exit 入队 RECV_NOTIFY + 唤醒父进程 | scheduler_lock[parent->assigned_cpu] | 保护 state（WAIT_CHILD 或 WAIT_RECV） |
| sys_exit 无父进程回收 | procs_lock | 保护 PCB 槽位释放 |
| sys_waitpid 检查 state | scheduler_lock[child->assigned_cpu] | 保护 state |
| sys_waitpid 验证父关系 | procs_lock | 读 child->parent_pid |
| sys_waitpid 回收 (proc_reap) | procs_lock | 保护 PCB 槽位释放 |
| sys_spawn 创建 | procs_lock + scheduler_lock[target_cpu] | 复用 process_create_elf |

### 锁协议

```
scheduler_lock[cpu] 保护：
  - run_queue[cpu]（per-CPU 就绪队列链表）
  - procs[i].state（where procs[i].assigned_cpu == cpu）
  - cpu_locals[cpu].run_count
  - schedule() 的选择逻辑

procs_lock 保护：
  - procs[] 数组的空闲槽位查找（pid == -1）
  - PCB 字段初始化
  - PCB 槽位释放（proc_reap 中清零 pid=-1）
  - 不保护 state（state 归 scheduler_lock）

irq_owner[]：
  - sys_irq_bind 写：__atomic_store_n(&irq_owner[irq], pid, __ATOMIC_RELEASE)
  - trap_dispatch 读：__atomic_load_n(&irq_owner[trapno], __ATOMIC_ACQUIRE)
```

### 锁获取顺序（防死锁）

当需要同时持多把锁时，按以下顺序获取：

1. `procs_lock`（低频，先锁）
2. `scheduler_lock[cpu]`（高频，后锁）

不存在需要同时持 `scheduler_lock` + `procs_lock` 之外的组合锁路径。

新增的锁交互（sys_exit/sys_waitpid/sys_spawn）遵循 `procs_lock` 先、`scheduler_lock` 后的顺序：
- sys_exit：持 `scheduler_lock[child_cpu]` 设 ZOMBIE → 释放 → 入队 RECV_NOTIFY 到父进程 recv 队列 → 唤醒父进程（持 scheduler_lock[parent_cpu]，匹配 WAIT_CHILD 或 WAIT_RECV） → schedule
- sys_waitpid：持 `procs_lock` 验证父关系 → 释放 → 循环持 `scheduler_lock[child_cpu]` 检查 ZOMBIE → BLOCKED(WAIT_CHILD) → schedule → proc_reap（持 `procs_lock` 清零 PCB）
- sys_spawn：复用 `process_create_elf`（procs_lock → scheduler_lock）

### BKL 移除后的入口/出口

- `__alltraps`：条件 swapgs，**不再**有 `cli` + `kernel_lock_acquire`
- `__trapret`：条件 swapgs，**不再**有 `kernel_lock_release`
- `syscall_fast_entry`：swapgs + 切栈，**不再**有 `kernel_lock_acquire`
- SYSRET 返回路径：**不再**有 `kernel_lock_release`
- `process_entry`：直接 `jmp __trapret`，**不再**有 `kernel_lock_acquire`
- `idle_entry`：`sti()` + `while(1) { schedule(); sti(); hlt; }`，**不再**有 BKL 操作
- `schedule()`：内部使用 `spin_lock_irqsave(&scheduler_lock)` / `spin_unlock_irqrestore`

## SMP 多核支持

- **AP 启动**：`smp_boot_aps()`（`arch/x64/smp.cc`）按序启动 AP，流程：复制 trampoline 到物理 0x8000 → INIT IPI → 等待 10ms → SIPI x2（vector=0x08）→ 等待 200us
- **AP trampoline**：`arch/x64/ap_trampoline.S`，16 位实模式入口，经实模式→保护模式→长模式跳转到 `ap_entry_c`。数据区（0xC0-0xDC）存放 BSP 填入的 PML4 地址、内核栈指针、`ap_entry_c` 地址、CPU ID
- **Per-CPU 数据**：`cpu_local_t`（`arch/x64/smp.h`）通过 SWAPGS/GS_BASE MSR 机制访问，包含 cpu_id、apic_id、`_cur_proc`、lapic_base、kernel_stack、tss_rsp0、run_count、idle_proc、scheduler_lock、run_queue。`current_proc` 宏展开为 `get_cpu_local()->_cur_proc`。SYSCALL 入口通过 `%gs:32` 读 `tss_rsp0` 切换内核栈
- **Per-CPU GDT/TSS**：每 CPU 独立的 8 项 GDT + TSS（`per_cpu_gdt[MAX_CPUS][8]`、`per_cpu_tss[MAX_CPUS]`、`per_cpu_ist_stack[MAX_CPUS][3]`），在 `smp_init_cpu()` 中初始化，`smp_apply_cpu()` 中通过 `lgdt` + `swapgs` + `ltr` 加载
- **LAPIC**：`arch/x64/apic.h` + `apic.cc`，MMIO 方式访问（映射到 higher-half 设备区），BSP 在 `apic_init()` 中初始化并校准定时器，AP 在 `ap_entry_c()` 中启用 LAPIC 并启动 per-CPU 周期定时器
- **I/O APIC**：`apic_init()` 配置 24 个 GSI 重定向项，定时器（GSI 0）和键盘（GSI 1）unmask 并路由到 BSP APIC ID
- **MADT 解析**：`boot/stub.c` 解析 ACPI MADT 表，提取 LAPIC/IOAPIC 基地址和各 CPU APIC ID（最多 MAX_CPUS=4），结果存入 `boot_info`
- **CPU 挑选**：`pick_cpu()`（`kernel/proc.cc`）遍历 `cpu_locals[].run_count`（`__atomic_load_n` RELAXED），选最小值
- **AP 参与调度**：AP 有自己的 idle 进程（`create_idle_process`），`timer_handler` 用 `tf->cs == 0x2B` 判断是否抢占（不再有 `cpu_id != 0` 守卫）
- **MSR 常量**：`MSR_EFER`/`MSR_STAR`/`MSR_LSTAR`/`MSR_CSTAR`/`MSR_SFMASK`/`EFER_SCE`/`EFER_NXE` 定义在 `arch/x64/smp.h`，用于 SYSCALL/SYSRET 和 NX 位设置

## 进程与调度

- **PCB**：`proc_t`（pid, state, k_rsp, k_stack_top, cr3, entry, wait_event, assigned_cpu, iopl, parent_pid, exit_code, mmap_brk, mmap_regions, fd_table[MAX_FD], run_node, wait_node, wait_deadline, wait_timed_out, shm_regions[MAX_SHM_PER_PROC], cpu_time_ns, last_sched, recv_buf[16][64]/recv_head/recv_tail/recv_lock, req_caller_pid/req_reply_buf/req_result/req_target_pid, msg_caller_pid/msg_reply_buf/msg_reply_len/msg_result/msg_target_pid），最多 64 个进程
- **状态**：READY / RUNNING / BLOCKED（可等待 WAIT_RECV 或 WAIT_REQ_REPLY 或 WAIT_MSG_REPLY 或 WAIT_CHILD 或 WAIT_PIPE）/ ZOMBIE（已退出，等待父进程 waitpid 回收）/ REAPING（proc_reap 中，防止重入调度）
- **调度**：`schedule()` 从 per-CPU `run_queue`（`list_node_t` 链表）队头取出下一进程，FIFO round-robin。切换 FROM idle 时不设 READY 也不入队。无就绪进程时若 prev 为 BLOCKED 或 ZOMBIE 则切到 idle，否则 prev 继续运行。持锁模式：`spin_lock_irqsave(&scheduler_lock)` → 出队/入队 → `spin_unlock_irqrestore` → `switch_to` → `spin_lock_irqsave` → `spin_unlock_irqrestore`
- **上下文切换**：`switch_to` 保存 callee-saved（rbx, rbp, r12-r15）→ 保存/恢复 RSP → 切换 CR3 → 恢复 callee-saved → ret
- **新建进程首次运行**：`process_entry` → jmp `__trapret` → iretq 到用户态
- **idle 进程**：每 CPU 一个（`create_idle_process`），无用户态，内核 PML4，`idle_entry` 循环：`schedule()` → `sti()` → `hlt`
- **run_count 维护**：创建++、BLOCKED--（`__atomic_add_fetch` RELAXED）、唤醒++（scheduler_lock 下）、READY↔RUNNING 不变

## 用户进程

- **创建方式**：`process_create_elf(elf_data, size, iopl, map_fb)` 从 ELF64 加载，`iopl` 参数指定 IOPL（0=普通进程，3=驱动进程），`map_fb` 参数控制是否映射 framebuffer 物理页（KMS 驱动需要）。先 `procs_lock` 分配槽位，再 `scheduler_lock` 入队。启动时进程由内核创建，`parent_pid=-1`；用户态 spawn 创建的子进程 `parent_pid` 为调用者 PID
- **地址空间**：代码从 0x400000 起，堆通过 mmap 从 0x800000 起，用户栈页面映射在 0x00007FFFFFFFD000，RSP 初始值 0x00007FFFFFFFE000（页面顶端），每进程独立 PML4（共享 PML4[511] 内核映射）
- **共享页**：所有驱动间 IPC 统一使用动态共享内存（sys_shm_create/sys_shm_attach），虚拟地址从 0x510000 起。kbd_driver 创建 1 页 SHM（含 driver_shm_header + kbd_ring），KMS 创建 display SHM（header 1 页 + back buffer N 页）。fs_driver 通过 sys_block_read/write 访问磁盘（无需 SHM）。客户端通过 sys_msg 与 fs_driver 通信。详见 `doc/design/dynamic_shm_migration.md`
- **IPC 拓扑（三层 + terminal 中间层）**：kbd_driver 通过动态 SHM + sys_notify 与 terminal 通信（terminal 通过 sys_req KBD_REQ_BIND 绑定 kbd_driver）；terminal 通过 pipe 与 shell 通信（stdin pipe + stdout pipe）；terminal 通过 display SHM + sys_notify 与 kms_driver 通信（terminal 渲染到 back buffer，KMS flip）；Shell ←→ fs_driver。Shell 通过 sys_msg 与 fs_driver 通信；fs_driver 通过 sys_block_read/write 访问内核 AHCI 驱动
- **系统调用**：34 个 syscall（NR_SYSCALL=34，编号 0-33 连续无空洞），通过 `syscall` 指令触发（EFER.SCE），rax=syscall#，参数通过 rdi/rsi/rdx/r10/r8 传递。错误返回统一为负 errno（-EPERM/-EINVAL/-ENOMEM/-ENOSYS/-ECHILD/-EFAULT/-EEXIST/-EBUSY/-ESRCH/-ETIMEDOUT/-EBADF/-EMFILE/-EISDIR/-ENOTDIR/-EIO，定义在 common/errno.h）。pipe/write/read/close 实现 fd 抽象和进程间 pipe 通信。recv/req/resp 实现统一消息接收和同步内联请求（详见 `doc/design/rpc.md`），msg/msg_resp 实现变长消息 IPC（≤64KB），sys_recv 支持 4 参数签名（msg, data_buf, data_buf_len, timeout_ms）接收 IRQ/REQ/NOTIFY/MSG 四类消息，sys_notify（#21）消息入队模式，sys_gettime（#22）全局单调时钟（纳秒），sys_clock（#23）per-process CPU 时间（纳秒），sys_ioperm（#26）端口 I/O 权限，sys_dup2（#27）fd 复制，sys_fcntl（#28）fd 控制，sys_dma_alloc（#29）/sys_dma_free（#30）物理连续内存分配，sys_pci_dev_info（#31）PCI 设备查询，sys_block_read（#32）/sys_block_write（#33）块设备读写。
- **进程生命周期**：`sys_exit(exit_code)` 将当前进程设为 ZOMBIE（有父进程时向父进程 recv 队列入队 RECV_NOTIFY 并唤醒 WAIT_CHILD 或 WAIT_RECV）或直接回收（`parent_pid==-1` 时调用 `proc_reap`）；`sys_waitpid(pid, exit_code_ptr)` 阻塞等待子进程 ZOMBIE 后回收资源，返回子 PID 和退出码；`sys_spawn(elf_data, elf_size, iopl)` 从用户态 ELF 缓冲区创建子进程，IOPL 权限检查，设 `parent_pid`，自动继承 fd 0/1（pipe ref_count++）。sys_exit 通过 RECV_NOTIFY 唤醒父进程。proc_reap 清理进程全部 fd（pipe ref_count--，归零则 kfree pipe buf + pipe struct，wake_process 对端）+ dev_table_cleanup + 唤醒所有等待该进程 REQ/MSG reply 的进程（设 req_result/msg_result=ESRCH）+ 释放 recv 队列中的 RECV_MSG 内核缓冲区（kfree kmaddr）
- **proc_reap**：回收进程全部资源——遍历 PML4[0-255] 释放用户页映射（跳过 SHM 页）+ 页表页（PDPT/PD/PT）+ PML4 页 + 内核栈（2 页）+ 释放动态 SHM（扫描所有进程 ref_count，无引用则释放物理页）+ 关闭所有 fd（pipe ref_count-- + wake 对端 + 归零 kfree）+ dev_table_cleanup + 唤醒 REQ/MSG reply 等待者（设 req_result/msg_result=ESRCH）+ 释放 recv 队列 RECV_MSG 内核缓冲区 + 最终 CPU 时间记账 + PCB 槽位清零。PTE 叶页物理地址提取使用 `pte & 0x000FFFFFFFFFF000`（清除 bit 63 NX 位），而非 `pte & ~0xFFF`（后者不清除 NX 位导致越界）
- **用户态异常**：CPU 异常在 `trap_dispatch` 中判断 `tf->cs==0x2B`（用户态）时调用 `sys_exit(-1)` 替代 `halt()`，防止用户进程 crash 拖垮全机。内核态异常仍 halt
- **IOPL 支持**：`proc_t::iopl` 字段允许 per-process IOPL。kbd_driver 现为 IOPL=0（USB HID SHM 模式，不再访问 I/O 端口）；普通进程 IOPL=0，执行 I/O 指令触发 #GP
- **IRQ 绑定**：`sys_irq_bind(irq)` 将当前进程绑定到指定 IRQ（`__atomic_store_n` RELEASE），内核在 `irq_owner[irq]` 中记录 PID，该 IRQ 发生时内核直接获取 `scheduler_lock` 唤醒绑定进程
- **Shell**：`shell/shell.cc` 编译为 ELF64 静态可执行文件，通过 mkdisk.sh 写入 FAT32。链接 libc.a。交互式命令行：从 fd 0 (stdin pipe) 读键盘输入（支持 readline + 退格），通过 fd 1 (stdout pipe) 输出，通过 sys_msg 与 fs_driver 通信，支持 `r` 命令直接读裸扇区。命令：`ls [-l]`（目录列表）、`cat <path>`（读文件）、`cd <path>`（切换目录）、`pwd`（打印工作目录）、`touch <path>`（创建文件）、`mkdir <path>`（创建目录）、`r LBA [COUNT]`（hex dump 裸扇区）、`<path>`（路径执行 ELF 文件）、`h`（帮助）。路径执行替代旧 `run`/`malloc`/`free`/`mtest` 命令
- **用户态驱动**：`driver/kbd_driver.cc`（IOPL=0）USB HID 键盘驱动：`shm_attach_kernel(USB_HID_SHM_ID)` attach 内核预分配 HID SHM，`get_keycode_init` + `get_keycode`（`user/lib/usb_kbd.cc`）读 SHM keyboard sub-ring + HID report 对比 → key_event，`kbd_push` 映射为 ASCII 或 ESC 序列写入 kbd_ring。主循环：RECV_NOTIFY → get_keycode → kbd_push。支持 REQ bind/unbind（KBD_REQ_BIND/KBD_REQ_UNBIND，通过 sys_req/sys_resp），bind 创建 1 页 SHM（含 driver_shm_header + kbd_ring[8]）并记录 consumer_pid，unbind 清除状态。consumer 已绑定时返回 -EBUSY。自注册 DEV_KBD（device_register）。PS/2 路径已完全移除（不再 bind IRQ1、不再 ioperm、不再 inb）。`driver/kms_driver.cc`（IOPL=0, map_fb=true）调用 display_backend_init 创建 display SHM + 注册 DEV_KMS，display_backend_poll/wait flip 循环（memcpy back→front buffer + generation counter 校验）；`driver/terminal.cc`（IOPL=0）启动时通过 device_lookup(DEV_KBD) + sys_req(KBD_REQ_BIND) 绑定键盘，display_client_init attach display SHM，读键盘→sys_write(1) 到 stdin pipe，sys_read(0) 从 stdout pipe 读→VT100 状态机→cell 缓冲区→display_client_render_cell 渲染到 back buffer→display_client_flush，fd 0 为 O_RDONLY|O_NONBLOCK；`driver/fs_driver.cc`（IOPL=0）通过 sys_block_read/write 访问磁盘，通过 sys_msg/sys_msg_resp 处理客户端文件请求（多客户端 session, MAX_CLIENTS=16），执行 FAT32 操作（readdir/open/read/close/raw_read/touch/mkdir），自注册 DEV_FS
- **Shell 构建**：通过 `add_user_elf(shell SOURCES shell.cc LINK_LIBS c)` 在 CMake 中管理。编译 flags 由 `user_rules.cmake` 统一设置（`-m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -I. -Iuser/include`），链接 `libc.a`
- **hello 构建**：通过 `add_user_elf(hello C SOURCES hello.c LINK_LIBS c)` 在 CMake 中管理。`C` 标记指定使用 gcc 编译 C 程序。hello.elf 通过 mkdisk.sh 写入 FAT32
- **hello 程序**：`user/hello.c` 编译为 ELF64 静态可执行文件，使用 libc 提供的 `_start`（调用 main → sys_exit）和 `printf` + `timespec_get` 计时。通过 mkdisk.sh 写入 FAT32 分区。shell 路径执行（如 `hello.elf`）可从 FAT32 加载并 spawn 执行
- **用户态堆**：`sys_mmap`（#11）/`sys_munmap`（#12）管理 mmap 区域（0x800000 起），是用户态 malloc 的底层支撑。`proc_t::mmap_brk` 记录 mmap 高水位，`proc_t::mmap_regions` 链表记录已映射区域
- **用户态 malloc/free**：`user/lib/malloc.cc` 实现 size-class slab 方案。小分配（≤2048B）走 per-class freelist + user_slab_header，大分配（>2048B）走 sys_mmap + big_alloc_header。底层统一用 sys_mmap。头文件 `user/include/stdlib.h`。打包进 libc（CMake target `c`）
- **用户态 libc.a**：CMake STATIC library target `c`，输出为 `build/libc.a`。包含 `_start.o`（入口点，调用 main → sys_exit）、`stdio.o`（FILE + printf/fprintf/vfprintf + stdout（line-buffered, sys_write_flush 写 pipe + 串口镜像）+ stderr（无缓冲）+ stdin（sys_read_fill 读 pipe））、`string.o`（strlen/strcmp/strncmp/strcpy/strncpy/strcat/strchr/memcpy/memset/memmove 等）、`malloc.o`、`sys.o`（req_call 封装）、`file.o`（open/read/write/close/pipe via sys_msg + 用户态 fd_table[32]）、`sys_ipc.o`（notify/recv/req/resp/msg/msg_resp 封装）、`time.o`（timespec_get/clock）。FILE 结构体含 fd/buffer/mode/flags/write_fn/read_fn。设计文档 `doc/design/libc.md`

## fd 与 pipe 架构

Phase 3 实现了最小 fd + pipe 机制，将 Shell 的 I/O 从直写 KMS ring 重构为经 Terminal 中转的分层模型。

- **内核 fd 表**：每进程 `struct file fd_table[MAX_FD]`（MAX_FD=32），每项含 type（FD_NONE/FD_PIPE）+ flags（O_RDONLY/O_WRONLY/O_RDWR/O_NONBLOCK）+ pipe 指针。初始化全为 FD_NONE
- **libc 用户态 fd 表**：libc 维护额外 `fd_table[32]`（`user/lib/file.cc`），type=FD_NONE/FD_PIPE/FD_FILE。FD_PIPE 委托 sys_write/sys_read/sys_close；FD_FILE 通过 sys_msg 与 fs_driver 通信（open/read/close）。fd 0-2 初始化为 FD_PIPE
- **pipe**：内核 `struct pipe`（4KB ring buffer + head/tail + read_pid/write_pid + ref_count）。sys_pipe 创建一对相联 fd（read_fd + write_fd），ref_count=2。写满阻塞（设 write_pid + schedule），读完阻塞（设 read_pid + schedule，O_NONBLOCK 立即返回 0）。对端写入/关闭时 wake_process 唤醒
- **sys_pipe(fd_ptr)**：syscall #17，kmalloc pipe buf + pipe struct，找两个空闲 fd slot，写 fd_ptr[0]=read_fd, fd_ptr[1]=write_fd
- **sys_write(fd, buf, len)**：syscall #18，写 pipe ring buffer，满时阻塞，写后 wake read_pid
- **sys_read(fd, buf, len)**：syscall #19，读 pipe ring buffer，空时阻塞（O_NONBLOCK 返回 0），读后 wake write_pid
- **sys_close(fd)**：syscall #20，ref_count--，归零则 kfree(pipe->buf) + kfree(pipe)，否则 wake 对端
- **fd 继承**：sys_spawn 自动复制 fd 0/1 到子进程，pipe ref_count++。子进程（如 hello.elf）继承 shell 的 stdin/stdout pipe
- **proc_reap fd 清理**：遍历 fd_table，每个 FD_PIPE 项执行 ref_count-- + wake 对端 + 归零 kfree
- **Terminal/Shell pipe 拓扑**：kernel_main 创建两对 pipe：
  - pipe_stdin：terminal fd_table[1](O_WRONLY) → shell fd_table[0](O_RDONLY)
  - pipe_stdout：shell fd_table[1](O_WRONLY) → terminal fd_table[0](O_RDONLY | O_NONBLOCK)
- **数据流**：USB键盘 → xHCI Transfer Ring → ISR → SHM keyboard sub-ring → RECV_NOTIFY → kbd_driver get_keycode → kbd_push → kbd_ring → terminal → sys_write(1) → stdin pipe → shell sys_read(0)；shell printf → sys_write(1) → stdout pipe → terminal sys_read(0) → VT100 → cell buffer → display_client_render_cell → back buffer → display_client_flush → KMS display_backend_poll → memcpy back→front → framebuffer
- **Terminal 进程**（`driver/terminal.cc`）：通过 device_lookup(DEV_KBD) + sys_req(KBD_REQ_BIND) 绑定键盘，display_client_init attach display SHM（从 header 读 fb 元信息），维护 VT100 状态机（NORMAL/ESC/CSI）+ cell 缓冲区（ch + fg_color + bg_color），支持 \033[H/\033[row;colH/\033[2J/\033[K/\033[...m。主循环轮询 kbd_ring 和 stdout pipe（fd 0 O_NONBLOCK），flush_dirty_cells 渲染 dirty cell 到 back buffer，display_client_flush 标记 dirty + 递增 generation + 检查 backend_sleeping 决定是否 notify KMS
- **Display 协议**（`driver/display.h`）：display_shm_header（fb 元信息 + dirty_full + generation + backend_sleeping），client API（terminal 侧渲染 cell/clear/scroll/flush），backend API（KMS 侧 init/poll/wait）。generation counter 乐观锁防撕裂。详见 `doc/design/kms.md`

## 内存管理架构

两阶段分配器 + slab 分配器，均在 `init_mem`/`slab_init` 中初始化：

1. **Bump 分配器**：极简线性分配，`kernel_end` 起始，仅向前增长。定义在 `arch/x64/paging.cc`。用于 `init_mem` 阶段分配 frames 数组和页表。返回虚拟地址。`kernel_init_finish` 后禁用。
2. **BFC 分配器**：Best-Fit Contiguous，基于 frames 数组 + 有序 free_list，支持分配/释放/合并。`init_mem` 完成后可用于通用分配。`bfc_lock`（`spinlock_t`）保护 SMP 并发安全。
3. **Slab 分配器**：简化 SLUB，per-CPU active slab 无锁 fast path + per-cache partial list 持锁 slow path。9 个 size class（8-2048B），大分配（>2048B）回退 BFC。`kernel/mem/slab.h` + `slab.cc`。

初始化顺序：EFI mmap解析 → Bump初始化 → 分配frames数组 → 标记FREE/USED/RESERVED → extend_mapping(arch层) → flush_tlb → 标记内核占用页 → 建立free_list → init_fb。

## 开发备注

- QEMU 调试：run.sh 注释掉的 `-s -S` 参数用于 GDB 远程调试
- `.clang-format` = LLVM 风格。格式化代码：`clang-format -i <file>` 或 `find . -name '*.cc' -o -name '*.c' -o -name '*.h' | xargs clang-format -i`
- `outb`/`inb`/`inw`/`wrmsr`/`rdmsr`/`IrqGuard` 统一在 `arch/x64/utils.h`
- AHCI 驱动通过 ECAM 枚举 ICH9 SATA 控制器，Q35 自带 AHCI（PCI class 0x0106）
- `enable_paging` 在 `arch/x64/paging.cc` 中定义（物理地址运行），接受 `boot_info*` 参数（当前未使用）
- `init_fb` 在 `init_mem` 末尾调用，依赖 bump_alloc 和 device_vma_base 就绪
- CMake 链接步骤使用 `build_script/cmake/do_link.cmake` 脚本处理 `$<TARGET_OBJECTS>` 的分号列表问题
- Shell 的用户态 syscall 封装定义在 `common/syscall.h`（语义封装）+ `arch/x64/utils.h`（底层 `__syscall0` 等内联汇编），共享页结构定义在 `common/shm.h`。libc 封装：`user/include/sys.h`（req_call）+ `user/include/sys/ipc.h`（notify/recv/req/resp/msg/msg_resp）+ `user/include/sys/shm.h`（shm_create/shm_attach/shm_attach_kernel）+ `user/include/unistd.h`（getpid/read/write/close/pipe/open）+ `user/lib/file.cc`（用户态 fd_table + open/read/write/close）
- libc.a 为 CMake STATIC target `c`，输出 `build/libc.a`。shell/hello/malloctest/terminal 通过 `add_user_elf(LINK_LIBS c)` 链接
- 用户程序编译加 `-I. -Iuser/include`：`-Iuser/include` 让自定义 stdio.h/stdlib.h/string.h 优先于宿主机版本；`-I.` 让 common/syscall.h 和 arch/x64/utils.h 可用；宿主机 freestanding 头文件（stdint.h/stddef.h/stdarg.h）通过 gcc 默认路径可用
- PTE 物理地址提取必须用 `pte & 0x000FFFFFFFFFF000`（清除 bit 63 NX 位），`& ~0xFFF` 不清除 NX 位会导致越界
- `sys_notify` 入队 RECV_NOTIFY 消息到目标进程 recv 队列并唤醒 WAIT_RECV，不再直接操作 state/run_queue（与 `wake_process` 区分：pipe I/O 唤醒用 `wake_process` 只改状态不入队消息，避免 sys_recv 误消费）
- `common/input.h` 已移至 `user/include/input.h`，KBD_RPC_BIND/UNBIND 已改名为 KBD_REQ_BIND/UNBIND
- `common/elf.cc` 已移至 `kernel/elf_loader.cc`，`common/efi.h` 已移至 `kernel/efi.h`
- `sys_shm_attach` 扩展：arg2 mode=0 为原有 PID attach，mode=1 为内核 SHM ID attach（`kernel_shm_table` + `register_kernel_shm`）
- kbd_driver 从 PS/2 迁移到 USB HID：IOPL=0，不再 bind IRQ1/inb，改为 `shm_attach_kernel` + `get_keycode`
- `add_user_elf` 现支持多源文件：kbd_driver = `kbd_driver.cc` + `../user/lib/usb_kbd.cc`
- 驱动自注册：kbd_driver/kms_driver/fs_driver 通过 `device_register(getpid(), DEV_XXX)` 自注册

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

优先考虑串口打印定位，qemu初始化时间较长约5s加上引导时间，建议等待10s以上。串口输出在 `log.txt`（`run.sh` 中 `2>&1 | tee log.txt` 重定向）。

实时查看串口输出：
```bash
tail -f log.txt
```

常见错误信号：
- **Page Fault (#PF)**：检查地址映射和空指针
- **General Protection (#GP)**：检查段选择子、IOPL、MSR 访问
- **Triple Fault (#DF)** → QEMU 重启：通常是 TSS IST 栈或 IDT 未正确设置

### Debug 模式（栈回溯）

`./build.sh -d` 启用 Debug 模式，编译时添加 `-g -fno-omit-frame-pointer`。当 CPU 异常发生时，`trap_dispatch` 会打印完整寄存器 + RBP 链栈回溯（最多 16 帧），输出到串口。典型流程：

```bash
./build.sh -d          # 带 -g -fno-omit-frame-pointer 编译
./run.sh               # 启动 QEMU
# 触发异常后查看串口输出
cat log.txt            # 找到 BACKTRACE 段，每帧格式: #N <return_addr>
# 用 addr2line 将虚拟地址解析为源码位置
addr2line -e build/myos.elf -f -C 0xFFFFFFFF8010XXXX
```

注意：仅内核态异常的栈回溯可解析（内核 ELF 含 debug info）；用户态异常的回溯地址在用户地址空间，需用对应用户 ELF 解析。

### GDB 远程调试

取消 `run.sh` 底部注释行的注释（添加 `-s -S` 参数），QEMU 会等待 GDB 连接后再执行。连接命令：
```
gdb -ex "target remote localhost:1234"
```

## 6. 技术债务

在设计过程中需要综合考量工作量和规范性、扩展性，不能一味追求简单快速，但也不要过度设计，如果有不确定的地方参考Linux的实现方案。对于暂时的技术妥协中间状态，需要补充后续工作到doc/design/todo.md中

## 7. 文档原则

过时的内容直接删除仅保留最新状态，历史信息由git log记录，文档内容在自己模块文档中最详细，其它引用的地方只保留必要信息，需要补充的部分附上链接即可
