# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

64位 x86-64 higher-half 内核，UEFI stub 引导，freestanding C++ + 纯汇编。C++ 编译为 `-fPIE`，使用 RIP-relative 寻址。`_start` 和 `enable_paging` 在物理地址运行，设置分页后跳转到 `kernel_main` 在虚拟地址 0xFFFFFFFF80100000 运行。引导后启动 shell 用户进程（从磁盘加载 shell.elf，ELF64 格式）。

## 构建与运行

```bash
./build.sh          # CMake 编译内核 + EFI bootloader + 编译 shell.elf + 生成 disk.img + mkimg.sh 生成 boot.img
./build.sh clear    # 清除 build/ 目录
./run.sh            # QEMU 启动（OVMF UEFI, 512MB, VGA, 串口输出到 log.txt）
```

构建体系为 CMake + 自定义链接脚本（`build_script/cmake/do_link.cmake`）。C++ 使用 `-fPIE -mno-red-zone -mno-sse -mno-sse2 -mno-mmx`，C 使用 `-fno-pic -fno-pie`，纯汇编用 `-m64`。链接：`ld -m elf_x86_64 -T build_script/linker.ld`（输出 elf64-x86-64）。

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
  stub.c              — EFI bootloader（GNU-EFI），读取 myos.elf + GOP + RSDP，ExitBootServices 后跳转内核

arch/x64/
  CMakeLists.txt
  start.S            — 纯汇编入口 _start（物理地址 → 虚拟地址跳转）
  vectors.S          — 48个中断向量桩 + vector128（syscall int 0x80）
  trapentry.S        — __alltraps/__trapret, syscall_entry/syscall_ret, switch_to, process_entry, reload_cs
  paging.cc / paging.h — GDT(7项+TSS), enable_paging, 4级页表, bump_alloc, extend_mapping, flush_tlb
  trap.cc / trap.h   — IDT(256项), PIC, PIT 初始化
  utils.h            — outb/inb/inw, wrmsr/rdmsr, KERNEL_CS, L16/H16, memcpy, serial_early_out, IrqGuard, 用户态 syscall 封装

kernel/
  CMakeLists.txt
  kernel.cc / kernel.h — kernel_main（加载 shell.elf → process_create_elf → schedule）
  serial.cc / serial.h — COM1 串口驱动
  trap.cc / trap.h   — trap_dispatch + syscall_dispatch + IRQ 注册表 + 4个系统调用
  proc.cc / proc.h   — PCB, switch_to, process_create/process_create_elf, schedule, round-robin
  elf.cc / elf.h     — ELF64 静态二进制加载器
  mem/
    CMakeLists.txt
    alloc.cc / alloc.h — init_mem, Bump/BFC 分配器, Page 描述符

driver/
  CMakeLists.txt
  kbd.cc / kbd.h     — 键盘驱动
  fb.cc / fb.h       — Framebuffer 驱动（含 IrqGuard 保护的操作）
  ata.cc / ata.h     — ATA PIO LBA28 驱动（从盘，读取 shell.elf）

user/
  shell.cc           — Shell 用户进程（putc/getc syscall 循环）

common/
  common.h            — kernel_end 声明
  macro.h             — ALIGN_UP 宏
  efi.h               — EFI 类型定义（memory_descriptor, system_table, GOP, GUID 等）
```

## 启动流程

```
UEFI 固件 → BOOTX64.EFI (boot/stub.c) → 加载 myos.elf 到 0x100000 → ExitBootServices → _start (arch/x64/start.S, 物理地址) → enable_paging → gdt_init → lretq → _entry64 (虚拟地址) → kernel_main → shell 用户进程
```

1. UEFI 固件加载 FAT32 上的 `\EFI\BOOT\BOOTX64.EFI`
2. `efi_main`（stub.c）：打开 `myos.elf`，加载 ELF64 段到物理 0x100000，读取 GOP/RSDP，填充 `boot_info`，调用 `ExitBootServices`，跳转到 `_start`
3. `_start`（start.S）：设置物理栈 → 保存 `boot_info*`（r12）→ 调用 `enable_paging`（构建 4 级页表 + 加载 CR3）→ 调用 `gdt_init`（7项 GDT + TSS + ltr）→ `lretq` 跳转到 `_entry64`
4. `_entry64`：切换到虚拟地址栈 → 复制 `boot_info` 到 `g_boot_info` → 调用 `kernel_main`
5. `kernel_main`：`init_mem` → `isr_init`（内含第二次 `gdt_init`）→ `kernel_init_finish`（禁用 bump）→ `proc_init` + `init_idle_proc` → 从磁盘加载 shell.elf → `process_create_elf` → `schedule` → idle 循环

注意：GDT 被初始化两次 — 一次在 start.S 物理地址阶段，一次在 `isr_init` 中虚拟地址阶段。

## 地址映射

- 物理 0-1GB → 虚拟 0-1GB（identity map，PML4[0] → PDPT_ident）
- 物理 0-1GB → 虚拟 0xFFFFFFFF80000000-0xFFFFFFFFC0000000（higher-half，PML4[511] → PDPT_hh[510]）
- 使用 2MB huge pages（PD 级别 PS=1），初始映射覆盖 1GB
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

- **GDT**：7项（null/code64/data/user_code64/user_data/TSS_low/TSS_high），在 `arch/x64/paging.cc` 的 `gdt_init` 中设置，通过 `lgdt` + `reload_cs`(lretq) 加载
- **TSS**：64位格式（128字节），仅填充 RSP0，占两个 GDT slot
- **IDT**：256项，`arch/x64/trap.cc` 的 `idt_install` 安装门描述符（16字节/项，selector=0x08）
- **向量桩**：`arch/x64/vectors.S` 定义 vector0-47 + vector128（syscall），CPU异常自动push error code的向量（8,10-14,17）不push dummy 0，其余push 0+向量号
- **trapentry.S**：`__alltraps` 保存全部16个GP寄存器→加载内核DS/ES→调用 `trap_dispatch(rdi=&trapframe)`；`__trapret` 恢复→`iretq`
- **syscall路径**：vector128 → `syscall_entry`（同 __alltraps 布局）→ `syscall_dispatch` → `syscall_ret` → `iretq`
- **IRQ 注册表**：`kernel/trap.cc` 维护 `irq_handler_t` 数组，`register_irq(vec, fn)` 注册处理器，`trap_dispatch` 查表调用
- **IrqGuard**：`arch/x64/utils.h` 中的 RAII 类，构造时 `local_irq_save`，析构时 `local_irq_restore`，用于 fb.cc 等需要中断安全操作的场景
- **PIC**：master(0x20)/slave(0xA0)重映射，master IRQ 32-39，slave IRQ 40-47；仅unmask timer(IRQ0)和keyboard(IRQ1)
- **PIT**：通道0 ~100Hz（divisor 11932）
- **trap分发**：向量128=syscall→`syscall_dispatch`；注册表优先→默认：向量32=定时器（EOI+schedule），向量33=键盘（kbd_handle+wakeup+EOI），其他IRQ仅EOI，CPU异常=串口诊断+halt

## 进程与调度

- **PCB**：`proc_t`（pid, state, k_rsp, k_stack_top, cr3, entry, wait_event），最多 64 个进程
- **状态**：READY / RUNNING / BLOCKED（可等待 WAIT_KBD）
- **调度**：`schedule()` 轮询扫描，找到 READY 进程后 `switch_to(prev, next)`
- **上下文切换**：`switch_to` 保存 callee-saved（rbx, rbp, r12-r15）→ 保存/恢复 RSP → 切换 CR3 → 恢复 callee-saved → ret
- **新建进程首次运行**：`process_entry` → jmp `__trapret` → iretq 到用户态
- **idle 进程**：PID=0，使用引导栈，halt 循环

## 用户进程

- **创建方式**：`process_create(entry)` 使用硬编码 `init_code[]`；`process_create_elf(elf_data, size)` 从 ELF64 加载
- **地址空间**：代码从 0x400000 起，用户栈页面映射在 0x00007FFFFFFFD000，RSP 初始值 0x00007FFFFFFFE000（页面顶端），每进程独立 PML4（共享 PML4[511] 内核映射）
- **系统调用**：4个（putc/getpid/yield/getc），通过 `int 0x80` 触发，rax=syscall#，参数通过 rbx/rcx/rdx/rsi/rdi 传递（当前最多使用1个参数，但基础设施支持5个）
- **键盘阻塞**：`sys_getc` 在键盘缓冲区空时标记进程 BLOCKED+WAIT_KBD，IRQ1 唤醒
- **Shell**：`user/shell.cc` 编译为 ELF64 静态可执行文件，写入 disk.img LBA 1 起始扇区（SHELL_ELF_SECTORS=16，即 8KB），内核启动时通过 ATA PIO 读取并用 `process_create_elf` 创建进程
- **Shell 构建**：`g++ -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector -I. -c user/shell.cc` + `ld -m elf_x86_64 -Ttext 0x400000`。注意 `-fno-pie`（固定地址链接）和 `-I.`（允许 include `arch/x64/utils.h` 中的 syscall 封装）

## 内存管理架构

两阶段分配器，均在 `init_mem`（`kernel/mem/alloc.cc`）中初始化：

1. **Bump 分配器**：极简线性分配，`kernel_end` 起始，仅向前增长。定义在 `arch/x64/paging.cc`。用于 `init_mem` 阶段分配 frames 数组和页表。返回虚拟地址。`kernel_init_finish` 后禁用。
2. **BFC 分配器**：Best-Fit Contiguous，基于 frames 数组 + 有序 free_list，支持分配/释放/合并。`init_mem` 完成后可用于通用分配（进程创建等）。

初始化顺序：EFI mmap解析 → Bump初始化 → 分配frames数组 → 标记FREE/USED/RESERVED → extend_mapping(arch层) → flush_tlb → 标记内核占用页 → 建立free_list → init_fb。

## 开发备注

- QEMU 调试：run.sh 注释掉的 `-s -S` 参数用于 GDB 远程调试
- `.clang-format` = LLVM 风格
- `outb`/`inb`/`inw`/`wrmsr`/`rdmsr`/`IrqGuard` 统一在 `arch/x64/utils.h`
- ATA 驱动字节为 `0xF0`（从盘），因为 disk.img 是 QEMU 第二个 IDE 设备
- `enable_paging` 在 `arch/x64/paging.cc` 中定义（物理地址运行），接受 `boot_info*` 参数（当前未使用）
- `init_fb` 在 `init_mem` 末尾调用，依赖 bump_alloc 和 device_vma_base 就绪
- CMake 链接步骤使用 `build_script/cmake/do_link.cmake` 脚本处理 `$<TARGET_OBJECTS>` 的分号列表问题
- Shell 的用户态 syscall 封装定义在 `arch/x64/utils.h`（内核头文件），shell.cc 通过 `-I.` 直接 include

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

优先考虑串口打印定位，qemu初始化时间较长约5s加上引导时间，建议等等时间10s以上
