# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

**微内核操作系统** — 64位 x86-64 higher-half 微内核，UEFI stub 引导，freestanding C/C++ + 纯汇编。内核仅提供最小机制：调度器、内存管理、中断/异常分发、系统调用接口。策略和服务尽量在用户态实现（shell 等用户进程从磁盘加载为独立 ELF64 运行）。C++ 编译为 `-fPIE`，使用 RIP-relative 寻址。`_start` 和 `enable_paging` 在物理地址运行，设置分页后跳转到 `kernel_main` 在虚拟地址 0xFFFFFFFF80100000 运行。用户态 C 程序通过 libc.a 静态链接 printf/malloc/string 等标准库函数。

## 项目目标

-**支持简单elf可执行文件的执行**: 在宿主机上编译hello world，静态链接os中的libc.a（含printf），在os上执行
-**支持Wayland核心协议**: 验收目标，
-**支持构建gcc**：成功在os上构建出gcc

### 微内核设计原则

- **最小内核**：内核仅包含调度、内存管理、中断分发、系统调用等不可替代的机制。键盘驱动和磁盘驱动已在用户态运行（`driver/kbd_driver.cc`、`driver/disk_driver.cc`），KMS 渲染驱动也在用户态运行（`driver/kms_driver.cc`），内核仅保留 `init_fb` 做 framebuffer 物理页映射和元信息保存。ATA 驱动仍留在内核空间（`kernel/ata.cc`）。
- **用户态服务**：shell 等应用及驱动进程作为独立用户进程从磁盘加载，拥有独立地址空间（每进程 PML4），通过 syscall 和共享页与内核及其他进程通信。文件系统服务（fs_driver）作为用户态进程运行，通过共享页 IPC 间接访问磁盘驱动。
- **系统调用 + 共享内存 IPC**：17 个 syscall（putc[deprecated]/getpid/yield/getc[deprecated]/wait/notify/irq_bind/sbrk/exit/waitpid/spawn/mmap/munmap/fb_info/shm_create/shm_attach/serial_write）是用户态与内核的控制接口。KBD/KMS 数据传输通过动态共享内存（sys_shm_create/sys_shm_attach）+ 环形缓冲区 + sleeping flag 完成；DISK/FS 仍使用硬编码共享页（DISK_REQ/DISK_RESP/FS_REQ/FS_RESP）。`sys_putc` 已删除（返回 -ENOSYS），所有输出通过 kms_ring + sleeping flag 协议完成。新增功能应优先考虑扩展 syscall 接口而非在内核内实现策略。
- **用户态 libc**：libc.a 静态库（`ar rcs libc.a start.o stdio.o string.o malloc.o`）提供 printf/fprintf/vfprintf/fputc/fputs/puts/fflush、FILE 结构体（缓冲输出 + write_fn 抽象）、strlen/strcmp/strcpy/strcat/strchr/memcpy/memset/memmove、malloc/free/calloc/realloc、`_start` 入口（调用 main → sys_exit）。stdout/stderr 的 write_fn 为 `kms_write_flush`（写 kms_ring + sleeping flag 协议），均为无缓冲模式。kms_ring 初始化由 `kms_shm_init(addr)` 完成。用户程序（hello.c）用 gcc 编译 + `-Iuser/include` + 链接 libc.a。设计文档 `doc/design/libc.md`。
- **进程隔离**：每个用户进程拥有独立地址空间（代码 0x400000 起，堆 0x600000 起，栈 0x00007FFFFFFFD000），PML4[511] 共享内核映射，其余独立。硬编码共享页映射到 0x501000-0x507FFF（7 页，DISK_REQ/DISK_RESP/FS_REQ/FS_RESP）。KBD/KMS 使用动态共享内存（sys_shm_create/sys_shm_attach），虚拟地址从 0x510000 起。

## 构建与运行

```bash
./build.sh          # CMake 编译内核 + EFI bootloader + 编译 libc.a + 编译用户 ELF + 生成 disk.img
./build.sh -d       # Debug 模式（加 -g -fno-omit-frame-pointer，异常时打印栈回溯）
./build.sh clear    # 清除 build/ 目录
./run.sh            # QEMU 启动（OVMF UEFI, 512MB, VGA, -smp 2, 串口输出到 log.txt）
```

构建体系为 CMake + 自定义链接脚本（`build_script/cmake/do_link.cmake`）。C++ 使用 `-fPIE -mno-red-zone -mno-sse -mno-sse2 -mno-mmx`，C 使用 `-fno-pic -fno-pie`，纯汇编用 `-m64`。链接：`ld -m elf_x86_64 -T build_script/linker.ld`（输出 elf64-x86-64）。用户态 ELF（shell/driver）使用 `ld -m elf_x86_64 -Ttext 0x400000`，`-fno-pie -ffreestanding -nostdlib -I.`。hello.c 使用 gcc + `-I. -Iuser/include -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector` + 链接 libc.a（`ld ... build/hello.o build/libc.a`）。libc.a 源文件（stdio.cc/string.cc/start.cc）使用 g++ + `-I. -Iuser/include`。

**磁盘布局（disk.img, 1MB, 2048 扇区）**：
```
LBA 0:       MBR 分区表（sfdisk 创建）
LBA 1-100:   disk_driver.elf（100 扇区/50KB）
LBA 101-200: kbd_driver.elf（100 扇区/50KB）
LBA 201-300: kms_driver.elf（100 扇区/50KB）
LBA 301-400: shell.elf（100 扇区/50KB）
LBA 401-500: fs_driver.elf（100 扇区/50KB）
LBA 501+:    FAT32 分区（type=0x0C，4KB 簇，含 HELLO.TXT/README/HELLO.ELF）
```
MBR 分区1 (type=0xDA) 覆盖 LBA 1-500（裸 ELF 存储），分区2 (type=0x0C) 覆盖 LBA 501+（FAT32 文件系统）。内核 `ata_read_lba` 直接读裸扇区加载 ELF，fs_driver 通过 MBR 分区表找到 FAT32 分区起始 LBA。

**重要：** `add_library(OBJECT)` 不能设置 `POSITION_INDEPENDENT_CODE ON`，否则会加 `-fPIC`，破坏内核的 RIP-relative 寻址。

**添加新源文件：** 在对应目录的 CMakeLists.txt 中将文件加入 `add_library(... OBJECT ...)` 列表即可。

## 目录结构

```
build_script/
  cmake/toolchain-x86_64.cmake
  cmake/do_link.cmake
  linker.ld            — 64位链接脚本
CMakeLists.txt
build.sh / mkimg.sh / run.sh
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
  kernel.cc / kernel.h — kernel_main（加载 5 个 ELF → process_create_elf → schedule）
  serial.cc / serial.h — COM1 串口驱动
  trap.cc / trap.h   — trap_dispatch（用户态异常→sys_exit替代halt）+ syscall_dispatch + 11个系统调用（含 sys_exit/sys_waitpid/sys_spawn）+ IRQ 注册表 + irq_owner[]。sys_notify 同时匹配 WAIT_NOTIFY 和 WAIT_CHILD
  proc.cc / proc.h   — PCB, switch_to, process_create/process_create_elf, schedule, shm_init, map_shared_pages, proc_reap（进程资源回收，PTE 物理地址提取用 `pte & 0x000FFFFFFFFFF000` 清除 NX 位）
  fb.cc / fb.h       — Framebuffer 初始化（init_fb 映射 + g_fb_info 元信息保存），渲染已移至用户态 KMS 驱动
  ata.cc / ata.h     — ATA PIO LBA28 驱动（从盘，启动时读取 ELF，之后由用户态 disk_driver 接管）
  list.h             — 内嵌双向链表（list_node_t, list_push_back/remove/empty/front, LIST_ENTRY）
  spinlock.h         — spinlock_t, spin_lock/unlock, spin_lock_irqsave/unlock_irqrestore
  mem/
    CMakeLists.txt
    alloc.cc / alloc.h — init_mem, Bump/BFC 分配器, Page 描述符(union: bfc/slab), bfc_lock, page_to_phys/phys_to_virt, 用户页映射函数声明
    slab.cc / slab.h   — 内核 slab 分配器(kmalloc/kfree/kcalloc/krealloc), kmem_cache_t, per-CPU active slab
    user_mapping.cc    — ensure_pd, ensure_pt_in_pd, map_user_page_direct, map_user_pages, unmap_user_pages

driver/
  kbd_driver.cc      — 用户态键盘驱动进程（IOPL=3, sys_irq_bind(33), inb(0x60), kbd_shm, sys_notify）
  disk_driver.cc     — 用户态磁盘驱动进程（IOPL=3, disk_req_shm, ATA PIO, disk_resp_shm, sys_notify）
  kms_driver.cc      — 用户态 KMS 渲染驱动进程（IOPL=0, KMS_REQ 共享页, framebuffer 直写 0x700000, 8x16 字体, 先处理再等待避免通知丢失）
  fs_driver.cc       — 用户态 FAT32 文件系统驱动（IOPL=0, fs_req/fs_resp, readdir/open/read/close/raw_read/touch/mkdir）

shell/
  shell.cc           — Shell 用户进程（ls/cat/cd/pwd/touch/mkdir/run/sbrk/malloc/mtest 命令, kbd_shm 输入, fs_driver IPC, kms_flush 输出, 裸扇区 r 命令）

user/
  hello.c           — 最小用户程序（#include <stdio.h> + main + printf 输出 "Hello, World!"）
  include/
    stdio.h         — 用户态 C 标准库头文件（FILE/printf/fprintf/vfprintf/fputc/fputs/puts/fflush + 缓冲模式常量）
    stdlib.h         — 用户态 C 标准库头文件（malloc/free/calloc/realloc/exit + EXIT_SUCCESS/EXIT_FAILURE）
    string.h         — 用户态字符串操作头文件（strlen/strcmp/strcpy/strcat/strchr/memcpy/memset/memmove）
  lib/
    stdio.cc        — FILE 实现 + stdout/stderr + kms_write_flush（写 KMS_REQ + sys_notify）+ vfprintf 格式化引擎
    string.cc       — 字符串和内存操作函数实现
    start.cc        — libc _start 入口点（fflush(stdout) → main → fflush(stdout) → sys_exit）
    malloc.cc        — 用户态 malloc/free/calloc/realloc 实现（size-class slab + sys_mmap，替代旧显式空闲链表 + sys_sbrk 方案）
  malloctest.c     — malloc/free 测试程序（小分配/大分配/calloc/realloc，链接 libc.a）

common/
  common.h            — kernel_end 声明
  macro.h             — ALIGN_UP/ALIGN_DOWN 通用对齐宏
  errno.h             — 错误码定义（EOK/EPERM/ENOENT/ENOMEM/EINVAL/ENOSYS/ECHILD）
  efi.h               — EFI 类型定义（memory_descriptor, system_table, GOP, GUID 等）
  elf.cc / elf.h     — ELF64 静态二进制加载器
  syscall.h           — syscall 编号（SYS_PUTC 等）+ 语义封装（sys_putc 等，含 arch/x64/utils.h）
  shm.h               — 共享页地址定义 + 数据结构（kbd_shm, disk_req_shm, disk_resp_shm[2页], fs_req_shm, fs_resp_shm[2页], kms_fb_info, kms_cmd, kms_req_shm）
  pid.h               — 驱动 PID 集中定义（DISK_DRIVER_PID=2, KBD_DRIVER_PID=3, KMS_DRIVER_PID=4, SHELL_PID=5, FS_DRIVER_PID=6）
```

## 启动流程

```
UEFI 固件 → BOOTX64.EFI (boot/stub.c) → 加载 myos.elf 到 0x100000 → ExitBootServices → _start (arch/x64/start.S, 物理地址) → enable_paging → gdt_init → lretq → _entry64 (虚拟地址) → kernel_main → shell 用户进程
```

1. UEFI 固件加载 FAT32 上的 `\EFI\BOOT\BOOTX64.EFI`
2. `efi_main`（stub.c）：打开 `myos.elf`，加载 ELF64 段到物理 0x100000，读取 GOP/RSDP，解析 ACPI MADT（LAPIC/IOAPIC 基地址、CPU APIC ID），填充 `boot_info`，调用 `ExitBootServices`，跳转到 `_start`
3. `_start`（start.S）：设置物理栈 → 保存 `boot_info*`（r12）→ 调用 `enable_paging`（构建 4 级页表 + 加载 CR3）→ 调用 `gdt_init`（8项 GDT + TSS + ltr）→ `lretq` 跳转到 `_entry64`
4. `_entry64`：切换到虚拟地址栈 → 复制 `boot_info` 到 `g_boot_info` → 调用 `kernel_main`
5. `kernel_main`：`init_mem` → `isr_init`（内含第二次 `gdt_init`，BSP 的 `smp_init_cpu` + `smp_apply_cpu`，`enable_nx`，`idt_install` + `setup_syscall`，`apic_init`）→ `kernel_init_finish`（禁用 bump）→ `slab_init`（初始化 9 个 kmem_cache_t）→ `proc_init` → `shm_init` → `clear` → `smp_boot_aps`（启动所有 AP）→ 从磁盘加载 disk_driver.elf（LBA 1, IOPL=3）→ kbd_driver.elf（LBA 101, IOPL=3）→ kms_driver.elf（LBA 201, IOPL=0, map_fb=true）→ shell.elf（LBA 301, IOPL=0）→ fs_driver.elf（LBA 401, IOPL=0）→ 栈切换进入 idle_entry → idle 循环（schedule + hlt）

注意：GDT 被初始化两次 — 一次在 start.S 物理地址阶段，一次在 `isr_init` 中虚拟地址阶段。

## 地址映射

- 物理 0-1GB → 虚拟 0-1GB（identity map，PML4[0] → PDPT_ident）
- 物理 0-1GB → 虚拟 0xFFFFFFFF80000000-0xFFFFFFFFC0000000（higher-half，PML4[511] → PDPT_hh[510]）
- 使用 2MB huge pages（PD 级别 PS=1），初始映射覆盖 1GB。内核 huge page 不标 NX（代码数据混合）
- PTE 标志常量：`PTE_PRESENT`/`PTE_RW`/`PTE_USER`/`PTE_PS`/`PTE_NX`（定义在 `paging.h`）
- 用户栈页映射时加 `PTE_NX`（不可执行），用户代码页可执行，共享页（0x500000-0x506000）加 `PTE_NX`
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

新增 syscall 锁交互（exit/waitpid/spawn）：

| 操作 | 持锁 | 说明 |
|------|------|------|
| sys_exit 设 ZOMBIE | scheduler_lock[child->assigned_cpu] | 保护 state + run_count |
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
- sys_exit：持 `scheduler_lock[child_cpu]` 设 ZOMBIE → 释放 → sys_notify（内部持 scheduler_lock，匹配 WAIT_NOTIFY 或 WAIT_CHILD） → schedule
- sys_waitpid：持 `procs_lock` 验证父关系 → 释放 → 循环持 `scheduler_lock[child_cpu]` 检查 ZOMBIE → BLOCKED → schedule → proc_reap（持 `procs_lock` 清零 PCB）
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

- **PCB**：`proc_t`（pid, state, k_rsp, k_stack_top, cr3, entry, wait_event, assigned_cpu, iopl, parent_pid, exit_code, brk, run_node, wait_node），最多 64 个进程
- **状态**：READY / RUNNING / BLOCKED（可等待 WAIT_NOTIFY 或 WAIT_CHILD）/ ZOMBIE（已退出，等待父进程 waitpid 回收）
- **调度**：`schedule()` 从 per-CPU `run_queue`（`list_node_t` 链表）队头取出下一进程，FIFO round-robin。切换 FROM idle 时不设 READY 也不入队。无就绪进程时若 prev 为 BLOCKED 或 ZOMBIE 则切到 idle，否则 prev 继续运行。持锁模式：`spin_lock_irqsave(&scheduler_lock)` → 出队/入队 → `spin_unlock_irqrestore` → `switch_to` → `spin_lock_irqsave` → `spin_unlock_irqrestore`
- **上下文切换**：`switch_to` 保存 callee-saved（rbx, rbp, r12-r15）→ 保存/恢复 RSP → 切换 CR3 → 恢复 callee-saved → ret
- **新建进程首次运行**：`process_entry` → jmp `__trapret` → iretq 到用户态
- **idle 进程**：每 CPU 一个（`create_idle_process`），无用户态，内核 PML4，`idle_entry` 循环：`schedule()` → `sti()` → `hlt`
- **run_count 维护**：创建++、BLOCKED--（`__atomic_add_fetch` RELAXED）、唤醒++（scheduler_lock 下）、READY↔RUNNING 不变

## 用户进程

- **创建方式**：`process_create(entry)` 使用硬编码 `init_code[]`（使用 `syscall` 指令，RAX=syscall#，RDI=arg1）；`process_create_elf(elf_data, size, iopl)` 从 ELF64 加载，`iopl` 参数指定 IOPL（0=普通进程，3=驱动进程）。两者均先 `procs_lock` 分配槽位，再 `scheduler_lock` 入队。启动时进程由内核创建，`parent_pid=-1`；用户态 spawn 创建的子进程 `parent_pid` 为调用者 PID
- **地址空间**：代码从 0x400000 起，堆从 0x600000 起（brk 初始=0x600000，sbrk 按页扩展/缩小），用户栈页面映射在 0x00007FFFFFFFD000，RSP 初始值 0x00007FFFFFFFE000（页面顶端），每进程独立 PML4（共享 PML4[511] 内核映射）
- **共享页**：每进程映射 10 个共享页（KBD_SHM=0x500000, DISK_REQ=0x501000, DISK_REQ2=0x502000, DISK_RESP=0x503000, DISK_RESP2=0x504000, FS_REQ=0x505000, FS_RESP=0x506000, FS_RESP2=0x507000, KMS_INFO=0x508000, KMS_REQ=0x509000），所有进程共享同一物理页，用于驱动间 IPC。disk_resp、disk_req、fs_resp 扩展为 2 页以容纳大块数据。`shm_init()` 在 `kernel_main` 中分配，`map_shared_pages()` 在 `process_create_elf` 中映射
- **IPC 拓扑（三层）**：Shell(PID5) ←→ fs_driver(PID6) ←→ disk_driver(PID2)。Shell 通过 FS_REQ/FS_RESP 与 fs_driver 通信；fs_driver 通过 DISK_REQ/DISK_RESP 与 disk_driver 通信。kbd_driver(PID3) 通过 KBD_SHM 直接与 shell 通信。Shell 通过 KMS_REQ + sys_notify 与 kms_driver(PID4) 通信
- **系统调用**：13个（putc/getpid/yield/getc[deprecated]/wait/notify/irq_bind/sbrk/exit/waitpid/spawn/mmap/munmap），通过 `syscall` 指令触发（EFER.SCE），rax=syscall#，参数通过 rdi/rsi/rdx/r10/r8 传递。错误返回统一为负 errno（-EPERM/-EINVAL/-ENOMEM/-ENOSYS/-ECHILD，定义在 common/errno.h）
- **进程生命周期**：`sys_exit(exit_code)` 将当前进程设为 ZOMBIE（有父进程时 notify 父进程）或直接回收（`parent_pid==-1` 时调用 `proc_reap`）；`sys_waitpid(pid, exit_code_ptr)` 阻塞等待子进程 ZOMBIE 后回收资源，返回子 PID 和退出码；`sys_spawn(elf_data, elf_size, iopl)` 从用户态 ELF 缓冲区创建子进程，IOPL 权限检查，设 `parent_pid`。sys_exit 通过 sys_notify 唤醒父进程（sys_notify 同时匹配 WAIT_NOTIFY 和 WAIT_CHILD）
- **proc_reap**：回收进程全部资源——遍历 PML4[0-255] 释放用户页映射 + 页表页（PDPT/PD/PT）+ PML4 页 + 内核栈（2 页）+ PCB 槽位清零。共享页物理帧（KBD_SHM/DISK_REQ/FS_REQ/KMS_INFO/KMS_REQ 等）不回收，因为它们是全局资源。PTE 叶页物理地址提取使用 `pte & 0x000FFFFFFFFFF000`（清除 bit 63 NX 位），而非 `pte & ~0xFFF`（后者不清除 NX 位导致越界）
- **用户态异常**：CPU 异常在 `trap_dispatch` 中判断 `tf->cs==0x2B`（用户态）时调用 `sys_exit(-1)` 替代 `halt()`，防止用户进程 crash 拖垮全机。内核态异常仍 halt
- **IOPL 支持**：`proc_t::iopl` 字段允许 per-process IOPL。驱动进程（disk_driver, kbd_driver）IOPL=3，可直接 `in`/`out`；普通进程 IOPL=0，执行 I/O 指令触发 #GP
- **IRQ 绑定**：`sys_irq_bind(irq)` 将当前进程绑定到指定 IRQ（`__atomic_store_n` RELEASE），内核在 `irq_owner[irq]` 中记录 PID，该 IRQ 发生时内核直接获取 `scheduler_lock` 唤醒绑定进程
- **Shell**：`shell/shell.cc` 编译为 ELF64 静态可执行文件，写入 disk.img LBA 101 起始扇区。链接 `user/lib/malloc.o`（用户态 malloc/free）。交互式命令行：从 `kbd_shm` 环形缓冲区读键盘输入（支持 readline + 退格），通过 fs_driver 共享页（FS_REQ/FS_RESP）进行文件操作，支持 `r` 命令直接读裸扇区。命令：`ls [-l]`（目录列表）、`cat <path>`（读文件）、`cd <path>`（切换目录）、`pwd`（打印工作目录）、`touch <path>`（创建文件）、`mkdir <path>`（创建目录）、`run <path>`（执行 ELF 文件）、`r LBA [COUNT]`（hex dump 裸扇区）、`sbrk N`（测试 sbrk）、`malloc N`（测试 malloc）、`free ADDR`（测试 free）、`mtest`（malloc 测试套件）、`h`（帮助）
- **用户态驱动**：`driver/kbd_driver.cc`（LBA 101, IOPL=3）绑定 IRQ33，读扫描码写入 `kbd_shm`；`driver/disk_driver.cc`（LBA 1, IOPL=3）读 `disk_req_shm` 执行 ATA PIO 操作写入 `disk_resp_shm`；`driver/kms_driver.cc`（LBA 201, IOPL=0, map_fb=true）读 KMS_REQ 共享页渲染文本到 framebuffer（0x700000 直写），主循环先 `process_commands()` 再在 `count==0` 时 `sys_wait()`；`driver/fs_driver.cc`（LBA 401, IOPL=0）读 `fs_req_shm` 执行 FAT32 只读操作（readdir/open/read/close/raw_read），内部通过 `disk_req_shm`/`disk_resp_shm` 与 disk_driver 通信，结果写入 `fs_resp_shm`
- **Shell 构建**：`g++ -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector -I. -Iuser/include -c shell/shell.cc` + 链接 libc.a + `ld -m elf_x86_64 -Ttext 0x400000`。注意 `-fno-pie`（固定地址链接）和 `-I.`（允许 include `common/syscall.h` 和 `common/shm.h` 中的封装）
- **hello 构建**：`gcc -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector -I. -Iuser/include -c user/hello.c` + `ld -m elf_x86_64 -Ttext 0x400000 -o build/hello.elf build/hello.o build/libc.a`。注意用 gcc（不是 g++）编译 C 程序，链接 libc.a（含 _start/printf/string/malloc）。hello.elf 通过 `mcopy -i build/part2.img build/hello.elf ::` 写入 FAT32
- **hello 程序**：`user/hello.c` 编译为 ELF64 静态可执行文件，使用 libc.a 提供的 `_start`（调用 main → sys_exit）和 `printf`（"Hello, World!\n"）。通过 build.sh mcopy 写入 FAT32 分区。shell 的 `run` 命令可从 FAT32 加载并 spawn 执行
- **用户态堆**：`sys_sbrk` 系统调用（#7）管理进程 brk 堆空间（0x600000 起），保留但用户态 malloc 不再使用。`sys_mmap`（#11）/`sys_munmap`（#12）管理 mmap 区域（0x800000 起），是用户态 malloc 的底层支撑。`proc_t::mmap_brk` 记录 mmap 高水位，`proc_t::mmap_regions` 链表记录已映射区域
- **用户态 malloc/free**：`user/lib/malloc.cc` 实现 size-class slab 方案。小分配（≤2048B）走 per-class freelist + user_slab_header，大分配（>2048B）走 sys_mmap + big_alloc_header。底层统一用 sys_mmap，不再调用 sys_sbrk。头文件 `user/include/stdlib.h`。打包进 libc.a（`ar rcs libc.a start.o stdio.o string.o malloc.o`）
- **用户态 libc.a**：静态库包含 `_start.o`（入口点，调用 main → sys_exit）、`stdio.o`（FILE + printf/fprintf/vfprintf + stdout/stderr + kms_write_flush）、`string.o`（strlen/strcmp/memcpy/memset/memmove 等）、`malloc.o`。FILE 结构体含 fd/buffer/mode/flags/write_fn，stdout/stderr 均为无缓冲（_IONBF），write_fn 为 kms_write_flush（写 KMS_REQ 共享页 + sys_notify KMS_DRIVER_PID）。设计文档 `doc/design/libc.md`

## 内存管理架构

两阶段分配器 + slab 分配器，均在 `init_mem`/`slab_init` 中初始化：

1. **Bump 分配器**：极简线性分配，`kernel_end` 起始，仅向前增长。定义在 `arch/x64/paging.cc`。用于 `init_mem` 阶段分配 frames 数组和页表。返回虚拟地址。`kernel_init_finish` 后禁用。
2. **BFC 分配器**：Best-Fit Contiguous，基于 frames 数组 + 有序 free_list，支持分配/释放/合并。`init_mem` 完成后可用于通用分配。`bfc_lock`（`spinlock_t`）保护 SMP 并发安全。
3. **Slab 分配器**：简化 SLUB，per-CPU active slab 无锁 fast path + per-cache partial list 持锁 slow path。9 个 size class（8-2048B），大分配（>2048B）回退 BFC。`kernel/mem/slab.h` + `slab.cc`。

初始化顺序：EFI mmap解析 → Bump初始化 → 分配frames数组 → 标记FREE/USED/RESERVED → extend_mapping(arch层) → flush_tlb → 标记内核占用页 → 建立free_list → init_fb。

## 开发备注

- QEMU 调试：run.sh 注释掉的 `-s -S` 参数用于 GDB 远程调试
- `.clang-format` = LLVM 风格
- `outb`/`inb`/`inw`/`wrmsr`/`rdmsr`/`IrqGuard` 统一在 `arch/x64/utils.h`
- ATA 驱动字节为 `0xF0`（从盘），因为 disk.img 是 QEMU 第二个 IDE 设备
- `enable_paging` 在 `arch/x64/paging.cc` 中定义（物理地址运行），接受 `boot_info*` 参数（当前未使用）
- `init_fb` 在 `init_mem` 末尾调用，依赖 bump_alloc 和 device_vma_base 就绪
- CMake 链接步骤使用 `build_script/cmake/do_link.cmake` 脚本处理 `$<TARGET_OBJECTS>` 的分号列表问题
- Shell 的用户态 syscall 封装定义在 `common/syscall.h`（语义封装）+ `arch/x64/utils.h`（底层 `__syscall0` 等内联汇编），共享页结构定义在 `common/shm.h`
- libc.a 由 `ar rcs build/libc.a build/start.o build/stdio.o build/string.o build/malloc.o` 创建。hello.c 用 gcc 编译 + 链接 libc.a；shell 也链接 libc.a（通过 kms_write_flush 输出）
- 用户程序编译加 `-I. -Iuser/include`：`-Iuser/include` 让自定义 stdio.h/stdlib.h/string.h 优先于宿主机版本；`-I.` 让 common/syscall.h 和 arch/x64/utils.h 可用；宿主机 freestanding 头文件（stdint.h/stddef.h/stdarg.h）通过 gcc 默认路径可用
- PTE 物理地址提取必须用 `pte & 0x000FFFFFFFFFF000`（清除 bit 63 NX 位），`& ~0xFFF` 不清除 NX 位会导致越界
- `sys_notify` 同时匹配 `WAIT_NOTIFY` 和 `WAIT_CHILD`，因为 `sys_exit` 通过 `sys_notify(parent_pid)` 唤醒在 `sys_waitpid` 中等待的父进程

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

优先考虑串口打印定位，qemu初始化时间较长约5s加上引导时间，建议等待10s以上。串口输出在 `log.txt`。

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
