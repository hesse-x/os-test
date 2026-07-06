# 构建系统

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 构建体系 | CMake + 自定义链接脚本 | 统一管理内核和用户态，编译规则封装在 cmake 函数中 |
| 2 | 内核寻址 | -fPIE + RIP-relative | x86-64 RIP-relative 不需要 GOT，物理/虚拟地址阶段自动正确 |
| 3 | 用户态寻址 | -fno-pie + -Ttext 0x400000 | 用户态用绝对地址，链接脚本指定入口 |
| 4 | POSITION_INDEPENDENT_CODE | OFF（内核 OBJECT library） | 防止 add_library(OBJECT) 自动加 -fPIC，破坏 RIP-relative 寻址 |
| 5 | objcopy | --remove-section .note.gnu.property | 移除 GNU property note，避免 ld 警告 |
| 6 | 磁盘映像 | 脚本生成（mkdisk.sh） | mkdisk.sh 生成单盘两分区 disk.img：ESP(FAT16) + 根(FAT32) |

### 构建入口

```bash
./build.sh          # 编译内核 + EFI bootloader + 用户态 ELF + 生成 disk.img
./build.sh -d       # Debug 模式（-g -fno-omit-frame-pointer）
./build.sh --test   # 测试构建（Unity 测试 ELF + test_runner）
```

build.sh 两步流程：CMake configure + make → mkdisk.sh

### 工具链

build_script/cmake/toolchain-x86_64.cmake：

- CMAKE_SYSTEM_NAME = Generic（裸机，无 OS）
- 编译器：gcc / g++ / gcc（ASM）
- 全局 -m64
- CMAKE_TRY_COMPILE_TARGET_TYPE = STATIC_LIBRARY（跳过 link check）

### 内核编译规则

build_script/cmake/kernel_rules.cmake — add_kernel_object(lib_name SOURCES ... ASM_SOURCES ...)

创建 OBJECT library，flags：
- C++：-ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fPIE -mno-red-zone -mno-sse -mno-sse2 -mno-mmx
- C：-ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -mno-sse -mno-sse2 -mno-mmx
- ASM：-m64（纯汇编手动使用 symbol(%rip)）
- POSITION_INDEPENDENT_CODE OFF
- Debug：CMAKE_BUILD_TYPE=Debug 时追加 -g -fno-omit-frame-pointer

内核 OBJECT library：

| target | 源码 |
|--------|------|
| arch_x64 | arch/x64/ 下所有 .cc/.S |
| kernel_mem | kernel/mem/ 下所有 .cc |
| kernel_obj | kernel/ 下其余 .cc |

### 内核链接

顶层 CMakeLists.txt add_custom_target(link) 调 do_link.cmake：

ld -m elf_x86_64 -T build_script/linker.ld <obj_files> -o build/myos.elf

do_link.cmake 处理 $<TARGET_OBJECTS> 的分号/空格混合分隔，统一为 CMake list 后传 ld。

链接脚本 build_script/linker.ld：VMA=0xFFFFFFFF80100000，LMA 用 AT(ADDR(.section) - VMA_BASE) 指定。段顺序 .text → .rodata → .data → .got → .bss。导出 kernel_end。

### 用户态编译规则

build_script/cmake/user_rules.cmake — add_user_lib() 和 add_user_elf()

公共 flags：-m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-sse2 -mno-mmx

与内核区别：-fno-pie（用户态绝对地址）而非 -fPIE。

构建类型 flags：CMake 目标（内核 OBJECT lib、static libc.a）自动继承
`CMAKE_<LANG>_FLAGS_<CONFIG>`；但 add_user_elf / add_user_ldso / SHARED libc.so /
add_user_dyn_elf 用 `add_custom_command` 裸调 gcc，不继承这些 flags。user_rules.cmake
据此定义 `USER_BUILD_FLAGS` 按 CMAKE_BUILD_TYPE 显式补齐（Release=-O3 -DNDEBUG，
Debug=-g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG，RelWithDebInfo/MinSizeRel 同 CMake
默认），并注入每个裸 gcc 命令。crt0.S 是纯汇编，不参与。

add_user_lib(lib_name SOURCES ...) — 创建 STATIC library（如 libc.a，target 名 c → libc.a）。

add_user_elf(name [C] SOURCES ... [LINK_LIBS ...]) — 三步管线：compile → objcopy → ld
- C 标记选择 gcc/g++
- objcopy --remove-section .note.gnu.property
- ld -m elf_x86_64 -Ttext 0x400000 <obj> [libs] -o <name>.elf
- LINK_LIBS 声明依赖（如 c 即 libc.a）

当前用户态 ELF：

| target | 源码 | LINK_LIBS |
|--------|------|-----------|
| shell | shell.cc | c |
| hello | hello.c (C) | c |
| init | init.c (C) | c |
| terminal | terminal.cc | — |
| kbd_driver | kbd_driver.cc | — |

Terminal 和驱动不链接 libc，使用 syscall 原语。

### 磁盘映像生成

mkdisk.sh 生成单盘两分区 build/disk.img（192MB）：
- sfdisk 创建 MBR 分区表（分区1: ESP type 0xEF FAT16 1MB 对齐，分区2: 根 type 0x0C FAT32）
- mkfs.fat 格式化两个分区（ESP FAT16，根 FAT32 512B 簇）
- mmd + mcopy 创建 FHS 目录结构并复制文件
- ESP 放 \EFI\BOOT\BOOTX64.EFI + myos.elf + init.elf（stub 把 init.elf 读进内存传给内核，initrd-style）
- 根分区放 /driver、/usr/bin、/usr/lib、/lib、/local 等用户态文件

### 添加新源文件

- 内核：add_kernel_object(lib_name SOURCES ... ASM_SOURCES ...)
- 用户态库：add_user_lib(lib_name SOURCES ...)
- 用户态 ELF：add_user_elf(name [C] SOURCES ... [LINK_LIBS ...])

### 关键源码位置

- 工具链：build_script/cmake/toolchain-x86_64.cmake
- 内核规则：build_script/cmake/kernel_rules.cmake
- 用户态规则：build_script/cmake/user_rules.cmake
- 链接脚本：build_script/linker.ld
- 链接辅助：build_script/cmake/do_link.cmake
- 磁盘映像：build_script/mkdisk.sh
- 顶层构建：CMakeLists.txt / build.sh

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| ld --remove-section 替代 objcopy | ld 命令中加 --remove-section .note.gnu.property，省掉中间 stripped.o | 低 |
| 用户态放开 red zone / SSE | 内核实现 FXSAVE/RXRSTORE 后，用户态可移除 -mno-red-zone -mno-sse -mno-sse2 -mno-mmx | 中 |
| 缺少 -mcmodel=kernel | C 文件用 -fno-pie 绝对寻址，应改为 -mcmodel=kernel | 低 |
| 链接脚本缺 section 对齐 | linker.ld 无 section 对齐声明，无法设置不同页权限 | 低 |
| disk.img 扩容至 ≥8GB | 当前 64MB 不足 clang/LLVM 构建（~5GB 产物），需解决 FAT32 4GB 单文件上限 | 中 |
