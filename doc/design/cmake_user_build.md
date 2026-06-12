# CMake 用户态构建重构

## 目标

将 build.sh 中手工 shell 编译的用户态 ELF 和 libc.a 迁入 CMake，新增 `add_kernel_object` / `add_user_elf` 封装函数集中管理编译规则，新增 `build_script/mkdisk.sh` 独立负责 disk.img 打包。

## 现状

- CMake 仅管内核（arch/x64, kernel, common）+ EFI bootloader → `myos.elf` + `BOOTX64.EFI`
- build.sh 手工 shell 编译：libc.a（4 个 .o → ar）+ 6 个驱动 ELF + 2 个测试 ELF（约 50 行）
- build.sh 手工 shell 打包 disk.img（约 40 行）
- mkimg.sh 负责 boot.img

## 改动概览

| 文件 | 变更 |
|------|------|
| `build_script/cmake/kernel_rules.cmake` | 新增，定义 `add_kernel_object()` |
| `build_script/cmake/user_rules.cmake` | 新增，定义 `add_user_lib()` + `add_user_elf()` |
| `CMakeLists.txt` | include rules 文件 + add_subdirectory(driver/shell/user) |
| `driver/CMakeLists.txt` | 新增，5 个 add_user_elf |
| `shell/CMakeLists.txt` | 新增，1 个 add_user_elf |
| `user/CMakeLists.txt` | 新增，add_user_lib + 2 个 add_user_elf |
| `arch/x64/CMakeLists.txt` | 改用 add_kernel_object |
| `kernel/CMakeLists.txt` | 改用 add_kernel_object |
| `kernel/mem/CMakeLists.txt` | 改用 add_kernel_object |
| `common/CMakeLists.txt` | 改用 add_kernel_object |
| `build_script/mkdisk.sh` | 新增，disk.img 打包 |
| `testdata/README` | 新增，FAT32 测试文件 |
| `build.sh` | 精简为 cmake + make + mkdisk.sh + mkimg.sh，删除用户态编译和 disk.img 打包 |

## CMake 封装函数

### add_kernel_object

```cmake
add_kernel_object(lib_name source1 source2 ...)
```

封装当前内核 OBJECT library 的通用设置：
- C++17
- POSITION_INDEPENDENT_CODE OFF
- CXX flags: `-ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fPIE -mno-red-zone -mno-sse -mno-sse2 -mno-mmx`
- C flags: `-ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -mno-sse -mno-sse2 -mno-mmx`
- ASM flags: `-m64`
- include: `${CMAKE_SOURCE_DIR}`

### add_user_lib

```cmake
add_user_lib(lib_name SOURCES source1 source2 ...)
```

封装用户态静态库（如 libc.a）：
- CXX flags: `-m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-sse2 -mno-mmx`
- C flags: 同上（gcc）
- include: `-I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/user/include`
- 输出: `build/lib${lib_name}.a`（CMake 自动加 `lib` 前缀，target 名 `c` → `libc.a`）

### add_user_elf

```cmake
add_user_elf(name
    [C]
    SOURCES source1 source2 ...
    [LINK_LIBS lib1 lib2 ...]
)
```

封装用户态 ELF 编译三步管线：

1. **compile**: `.cc`/`.c` → `build/name.o`（C 关键字时用 gcc，否则用 g++）
2. **objcopy**: `name.o` → `name.stripped.o`（`--remove-section .note.gnu.property`）
3. **ld**: `name.stripped.o` [+ libs] → `build/name.elf`（`ld -m elf_x86_64 -Ttext 0x400000`）

编译 flags 同 add_user_lib。LINK_LIBS 声明依赖（如 `c`），通过 DEPENDS 保证构建顺序，ld 命令中通过 `${CMAKE_BINARY_DIR}/lib${lib}.a` 引用。

## 子目录 CMakeLists.txt

### driver/CMakeLists.txt

```cmake
add_user_elf(disk_driver    SOURCES disk_driver.cc)
add_user_elf(kbd_driver     SOURCES kbd_driver.cc)
add_user_elf(kms_driver     SOURCES kms_driver.cc)
add_user_elf(terminal       SOURCES terminal.cc)
add_user_elf(fs_driver      SOURCES fs_driver.cc)
```

### shell/CMakeLists.txt

```cmake
add_user_elf(shell SOURCES shell.cc LINK_LIBS c)
```

### user/CMakeLists.txt

```cmake
# CMake add_library(STATIC c) 输出 libc.a
add_user_lib(c
    SOURCES lib/start.cc lib/stdio.cc lib/string.cc lib/malloc.cc
)

add_user_elf(hello  C SOURCES hello.c  LINK_LIBS c)
add_user_elf(malloc C SOURCES malloctest.c LINK_LIBS c)
```

注：`malloc` 的输出 ELF 名为 `malloc.elf`，源文件保持 `malloctest.c` 不改名。

## build_script/mkdisk.sh

独立脚本负责 disk.img 打包，从 `build/` 读取已编译 ELF。LBA 映射硬编码：

| ELF | LBA | 扇区数 |
|-----|-----|--------|
| disk_driver.elf | 1 | 100 |
| kbd_driver.elf | 101 | 100 |
| kms_driver.elf | 201 | 100 |
| terminal.elf | 301 | 100 |
| shell.elf | 401 | 100 |
| fs_driver.elf | 501 | 100 |

FAT32 分区（LBA 601 起）内容：
- `testdata/README`（静态文件）
- `build/hello.elf`（编译产物）
- `build/malloc.elf`（编译产物）

## build.sh 改造后

```bash
#!/bin/bash
set -e
BUILD_TYPE=Release
if [[ "$1" == "-d" ]]; then
    BUILD_TYPE=Debug
fi
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../build_script/cmake/toolchain-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=$BUILD_TYPE ..
make
cd ..
./build_script/mkdisk.sh
./mkimg.sh
```

## top-level CMakeLists.txt 结构

```cmake
cmake_minimum_required(...)
project(myos C CXX ASM)

# 基本设置

include(build_script/cmake/kernel_rules.cmake)
include(build_script/cmake/user_rules.cmake)

# 内核
add_subdirectory(boot)
add_subdirectory(arch/x64)
add_subdirectory(kernel)
add_subdirectory(common)

# 用户态
add_subdirectory(driver)
add_subdirectory(shell)
add_subdirectory(user)

# 内核链接（现有 link custom target）
```

## 后续优化

| 项目 | 说明 |
|------|------|
| ld `--remove-section` 替代 objcopy | 在 ld 命令中加 `--remove-section .note.gnu.property`，省掉中间 .stripped.o 文件 |
| 用户态放开 red zone / SSE | 内核实现 FXSAVE/RXRSTORE 后，用户态可移除 `-mno-red-zone -mno-sse -mno-sse2 -mno-mmx` |
