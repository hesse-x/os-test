# 构建系统

## 概述

CMake + 自定义链接脚本构建体系。内核和用户态统一由 CMake 管理，编译规则封装在 `kernel_rules.cmake`（`add_kernel_object`）和 `user_rules.cmake`（`add_user_lib` + `add_user_elf`）。磁盘映像由 `mkdisk.sh` 和 `mkimg.sh` 生成。

## 构建入口

```bash
./build.sh          # CMake 编译内核 + EFI bootloader + 用户态 ELF + 生成 disk.img + boot.img
./build.sh -d       # Debug 模式（加 -g -fno-omit-frame-pointer，异常时打印栈回溯）
```

`build.sh` 三步流程：

1. CMake configure + make（内核 + 用户态）
2. `mkdisk.sh` 生成 `disk.img`
3. `mkimg.sh` 生成 `boot.img`

## 工具链

`build_script/cmake/toolchain-x86_64.cmake`：

- `CMAKE_SYSTEM_NAME = Generic`（裸机，无 OS）
- 编译器：gcc / g++ / gcc（ASM）
- 全局 `-m64`
- `CMAKE_TRY_COMPILE_TARGET_TYPE = STATIC_LIBRARY`（跳过 link check）

## 内核编译规则

`build_script/cmake/kernel_rules.cmake` — `add_kernel_object(lib_name SOURCES ... ASM_SOURCES ...)`

- 创建 OBJECT library（不链接，仅编译为 .o）
- C++ flags：`-ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fPIE -mno-red-zone -mno-sse -mno-sse2 -mno-mmx`（顶层 CMakeLists.txt 设置）
- C flags：`-ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -mno-sse -mno-sse2 -mno-mmx`
- ASM flags：`-m64`（不走 -fPIE，纯汇编手动使用 `symbol(%rip)`）
- `POSITION_INDEPENDENT_CODE OFF`：防止 `add_library(OBJECT)` 自动加 `-fPIC`，破坏 RIP-relative 寻址
- Debug 模式：`CMAKE_BUILD_TYPE=Debug` 时追加 `-g -fno-omit-frame-pointer`

内核当前 OBJECT library：

| target | 源码 |
|--------|------|
| `arch_x64` | arch/x64/ 下所有 .cc/.S |
| `kernel_mem` | kernel/mem/ 下所有 .cc |
| `kernel_obj` | kernel/ 下其余 .cc |

## 内核链接

顶层 CMakeLists.txt 中 `add_custom_target(link)` 调用 `do_link.cmake`：

```cmake
ld -m elf_x86_64 -T build_script/linker.ld <obj_files> -o build/myos.elf
```

`do_link.cmake` 处理 `$<TARGET_OBJECTS>` 的分号/空格混合分隔问题，统一为 CMake list 后传给 `ld`。

链接脚本 `build_script/linker.ld`：VMA=0xFFFFFFFF80100000，LMA 用 `AT(ADDR(.section) - 0xFFFFFFFF80000000)` 指定。段顺序：`.text` → `.rodata` → `.data`（含 GOT）→ `.got` → `.bss`（4KB 对齐）。导出 `kernel_end`。

## 用户态编译规则

`build_script/cmake/user_rules.cmake` — `add_user_lib()` 和 `add_user_elf()`

### 公共编译 flags

```
-m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector
-mno-red-zone -mno-sse -mno-sse2 -mno-mmx
```

与内核的区别：`-fno-pie`（用户态用绝对地址，`-Ttext 0x400000`）而非 `-fPIE`。

### add_user_lib(lib_name SOURCES ...)

创建 STATIC library（如 `libc.a`）。

- 输出目录：`build/`（`ARCHIVE_OUTPUT_DIRECTORY`）
- include 路径：项目根 + `user/include/`

### add_user_elf(name [C] SOURCES ... [LINK_LIBS ...])

三步管线：compile → objcopy → ld。

1. **compile**：`C` 标记选择 gcc/g++，flags 为 `USER_COMPILE_FLAGS` + `-I. -Iuser/include`
2. **objcopy**：`objcopy --remove-section .note.gnu.property`（移除 GNU property note，避免 ld 警告）
3. **ld**：`ld -m elf_x86_64 -Ttext 0x400000 <obj> [libs] -o <name>.elf`

`LINK_LIBS` 声明依赖的库（如 `c` 即 libc.a），自动添加依赖和链接参数。

当前用户态 ELF：

| target | 源码 | LINK_LIBS |
|--------|------|-----------|
| shell | shell.cc | c |
| hello | hello.c (C) | c |
| malloctest | malloctest.c (C) | c |

Terminal 和各驱动不链接 libc，直接使用 syscall 原语。

## 磁盘映像生成

### mkdisk.sh

生成 `build/disk.img`（64MB, 131072 扇区）：

1. 创建零填充映像
2. dd 写入 6 个 ELF 到裸 LBA 区域（每槽 100 扇区 = 50KB）：

| LBA | ELF |
|-----|-----|
| 1-100 | disk_driver.elf |
| 101-200 | kbd_driver.elf |
| 201-300 | kms_driver.elf |
| 301-400 | terminal.elf |
| 401-500 | shell.elf |
| 501-600 | fs_driver.elf |

3. `sfdisk` 创建 MBR 分区表：
   - 分区1: LBA 1-600, type=0xDA（裸 ELF 存储）
   - 分区2: LBA 601-131071, type=0x0C（FAT32）
4. 提取分区2 → `mkfs.fat -F 32 -s 8` 格式化（4KB 簇）
5. `mmd` + `mcopy` 创建 FHS 目录结构并复制文件：

```
/                    根目录
├── README           测试用文本文件
├── boot/
│   ├── bin/         init（refactor_boot 后）
│   └── driver/      disk.dev, fs.dev
├── driver/          kbd.dev, kms.dev
├── usr/
│   ├── bin/         terminal, shell
│   └── lib/         libc.a
└── local/           hello.elf, malloc.elf
```

6. 写回 FAT32 分区到 disk.img

### mkimg.sh

生成 `build/boot.img`（128MB FAT32），包含 `\EFI\BOOT\BOOTX64.EFI` + `myos.elf`，供 QEMU UEFI 启动。

## 添加新源文件

- **内核**：在对应目录 CMakeLists.txt 中调用 `add_kernel_object(lib_name SOURCES ... ASM_SOURCES ...)`。
- **用户态库**：调用 `add_user_lib(lib_name SOURCES ...)`。
- **用户态 ELF**：调用 `add_user_elf(name [C] SOURCES ... [LINK_LIBS ...])`。`C` 标记表示使用 C 编译器。
