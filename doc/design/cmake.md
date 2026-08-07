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
./build.sh --perf   # 性能测试构建（同时启用 TEST）
./build.sh --sanitizer # 启用 sanitizer 构建
```

build.sh 流程：CMake configure + ninja → install-headers.sh + install-libs.sh（发布 sysroot）
→ 增量构建 libc++、Mesa、wlroots → TEST 模式按需构建 smoke ELF → mkdisk.sh。

### 工具链

build_script/cmake/toolchain-x86_64.cmake：

- CMAKE_SYSTEM_NAME = Generic（裸机，无 OS）
- 编译器：默认 clang / clang++ / clang（ASM）；`-DOS_COMPILER=gcc`（或 `build.sh --gcc`）切回 gcc / g++。两套工具链均支持。
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

顶层 CMakeLists.txt 用 add_executable(myos.elf) + target_link_libraries(myos.elf
PRIVATE kernel_obj) 托管链接，覆写 CMAKE_C_LINK_EXECUTABLE 强制裸 ld：

ld -m elf_x86_64 --no-relax -T build_script/linker.ld <objects> -o build/myos.elf

六个内核 OBJECT 库经层间 PUBLIC 依赖图聚合（kernel_obj → driver_obj → bsd_obj →
xcore_obj → xcore_mem → arch_x64）。OBJECT 库的传递聚合不递归（不像 STATIC 库），
故 myos.elf 显式 target_sources($<TARGET_OBJECTS:...>) 列全六库对象兜底；依赖图同时
传递层间 include/flag 的 usage requirement。--no-relax：内核 -fPIE 小码模型 +
higher-half VMA 在 GOTPCREL→LEA 松弛窗口外，必须保留（见 reface_cmake.md §4.1）。

链接脚本 build_script/linker.ld：VMA=0xFFFFFF8000100000（KERNEL_VMA_BASE = VMA_BASE + 0x100000），LMA 用 AT(ADDR(.section) - VMA_BASE) 指定。段顺序 .text → .rodata → .data → .got → .bss。导出 kernel_end。

### 用户态编译规则

build_script/cmake/user_rules.cmake — add_user_lib() 和 add_user_elf()

公共 flags：-m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-sse2 -mno-mmx

与内核区别：-fno-pie（用户态绝对地址）而非 -fPIE。

构建类型 flags：CMake 目标（内核 OBJECT lib、static libc.a）自动继承
`CMAKE_<LANG>_FLAGS_<CONFIG>`；但 add_user_elf / add_user_ldso / SHARED libc.so /
add_user_dyn_elf 用 `add_custom_command` 裸调 `${CMAKE_C_COMPILER}`，不继承这些 flags。user_rules.cmake
据此定义 `USER_BUILD_FLAGS` 按 CMAKE_BUILD_TYPE 显式补齐（Release=-O3 -DNDEBUG，
Debug=-g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG，RelWithDebInfo/MinSizeRel 同 CMake
默认），并注入每个裸编译器命令。crt0.S 是纯汇编，不参与。

add_user_lib(lib_name SOURCES ...) — 创建 STATIC library（如 libc.a，target 名 c → libc.a）。

add_user_elf(name [C] SOURCES ... [LINK_LIBS ...]) — 三步管线：compile → objcopy → ld
- C 标记选择 C/C++ 编译器（`${CMAKE_C_COMPILER}` / `${CMAKE_CXX_COMPILER}`）
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

### libc++ 构建（默认组件，单独编译）

libc++（给 Mesa 交叉编译用的 C++ 标准库）是 LLVM **runtimes 子构建**（`cmake -S runtimes`），源文件列表由 LLVM 生成，不能像 musl 那样在主 CMake 里列源文件编；且强依赖 sysroot 就绪（子 cmake `--sysroot=build/sysroot`，需 crt + stub .so + 头 + libc.so 导出符号全到位）。因此由默认构建流程在 sysroot 发布后单独增量构建，不进主 CMake 依赖图。

**构建脚本** `build_script/build_libcxx.sh`（默认由 `./build.sh` 调用，也可裸调）：
1. **前置校验** `check_sysroot()` 逐项检查 crt（crt1/Scrt1/crti/crtn.o）+ libc.so + ld-musl 解释器 + 5 个 stub .so（librt/libdl/libpthread/libresolv/libxnet）+ 头树，缺项明确报 `run ./build.sh first`；再校验 `clang++`（runtimes 必须 clang-18）与 `third_party/llvm-project/runtimes/` 子模块（缺则引导 `git submodule update --init`）。
2. **增量编译并装回 sysroot**：每次进入 `cmake -G Ninja -S runtimes` + `ninja` + `ninja install`，由 Ninja 判定需要重编的目标。参数包括 `-DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind"`、`--sysroot`、`-nodefaultlibs -lc`、`-nostdinc++`、`-DLIBCXX_HAS_MUSL_LIBC=ON` 和 `CMAKE_INSTALL_PREFIX=sysroot/usr`。产物为 `sysroot/usr/lib/{libc++,libc++abi,libunwind}.so{,.1,.1.0}`、`.a` 与 `include/c++/v1/*`。

**增量性**由 libc++ 子构建的 Ninja 依赖图负责；默认流程每次都会进入该构建，源文件和 sysroot 未变化时保持 up-to-date。**强制重建**：删除 `build/libcxx-build` 及对应 sysroot 产物后重新运行 `./build.sh`。

**mkdisk 分发**（`build_script/mkdisk.sh`，在 `/usr/include` 整树拷之后、写回 FAT32 之前）：要求 libc++/libc++abi/libunwind 已安装，并将各自 `.so.1.0`、开发名 `.so` 和 soname `.so.1` 复制进 img `/lib`（FAT32 无 symlink，因此三个名字均为真实文件副本）；头 `include/c++/v1/*` 随 `sysroot/usr/include` 整树进入 img `/usr/include/c++/v1`。缺少任一默认运行库时制盘直接失败。

**回归冒烟 ELF** `user/test/libcxx_smoke.cpp`（TEST 构建启用）：覆盖历史上反复断链的四条路径——`<vector>`+`std::string`（基础 STL）、`throw std::runtime_error`+`catch`（异常 → libc++abi → TLS `__cxa_thread_atexit`）、`std::filesystem`（int128 `__divti3`/`__muloti4` compiler runtime 路径）、`std::messages` facet（catgets 路径）。任一步失败 `_exit` 非零，test_runner 报 `[FAIL]`。

接入点（`user/CMakeLists.txt`，`if(LIBCXX)` 块）：**不走 `add_user_dyn_elf`**——那条路径硬编码 `-nostdlib -nodefaultlibs` 链接，无法注入 `-stdlib=libc++` + libc++/libc++abi/libunwind 运行时（`terminal` 能用它只因零 STL/异常）。照 `ld_test_*` stub 模式用手写 `add_custom_command`：
- **编译**：`clang++ --sysroot=sysroot -stdlib=libc++`（自然解析 sysroot 的 `c++/v1` 头 + musl C 头）。**不用 `USER_FREESTANDING_FLAGS`**——其 `-nostdinc` 会同时禁掉 sysroot 的 `c++/v1` 搜索路径（`<cstdio>` 找不到），其 `-ffreestanding` 会让 clang 把 `main` 当普通函数 mangling（freestanding 下 main 不特殊）→ `Scrt1.o` 的 `_start_c` 找不到 C 链接的 `main`。本程序是 hosted 动态 ELF（有 PT_INTERP、libc、main），故去掉 `-ffreestanding`；只用 C++ 标准库头、无 OS 专有头依赖，故无需项目 `-I` 路径（`user/include` shim 反会抢先匹配 `<stdio.h>`）。`--sysroot` 已把搜索限制在 sysroot 内（无 host `/usr/include` 泄漏，已验证）。
- **链接**：`clang++ -fno-pie -no-pie -Wl,--dynamic-linker,/lib/ld-musl-x86_64.so.1 -Wl,--no-as-needed -Wl,--allow-shlib-undefined --sysroot=sysroot -stdlib=libc++ -nostdlib -nodefaultlibs` + `Scrt1.o`/`crti.o` + smoke.o + `-L sysroot/usr/lib -lc++ -lc++abi -lunwind` + `libc.so` + `crtn.o`。`--no-as-needed` 保留 libc++ DT_NEEDED；`--allow-shlib-undefined` 容忍 libc++ → libc++abi → libunwind 传递依赖。默认流程保证 sysroot 中的 `libc++*.so` 已就绪。
- **时序**：libc++ 由 `build_libcxx.sh` 在主 `ninja` **之后**装入 sysroot；TEST 构建随后显式请求非 ALL 的 `libcxx_smoke_elf` target，再运行 mkdisk。
- **test_runner 表**：`libcxx_smoke` 固定注册；`./build.sh --test` 将 smoke ELF 放入 img `/test/`，test_runner 统一执行。

### 磁盘映像生成

mkdisk.sh 生成单盘两分区 build/disk.img（192MB）：
- sfdisk 创建 MBR 分区表（分区1: ESP type 0xEF FAT16 1MB 对齐，分区2: 根 type 0x0C FAT32）
- mkfs.fat 格式化两个分区（ESP FAT16，根 FAT32 512B 簇）
- 产物清单由 CMake manifest 驱动：`build/image_manifest.txt`（configure 时由 `os_image_path()` 累积 + `os_write_image_manifest()` 写出，每行 `build_relpath<TAB>image_path<TAB>partition(1=ESP,2=root)`）。mkdisk 读 manifest 做依赖检查、目录骨架派生（按深度升序 mmd）与 mcopy，不再硬编码产物列表。各 helper（add_user_elf/dyn_elf/ldso/lib、add_third_party_lib）经 `IMAGE_PATH`/`NO_IMAGE`/`IMAGE_ARTIFACT`/`IMAGE_PARTITION` 参数登记镜像归属，`add_user_dyn_elf` 默认 `test/<name>.elf`、SHARED 默认 `lib/lib<out>.so`。详见 `build_script/cmake/image_rules.cmake` 与 reface_cmake.md §6。
- ESP 放 \EFI\BOOT\BOOTX64.EFI + myos.elf + init.elf（stub 把 init.elf 读进内存传给内核，initrd-style；三者经 manifest partition=1 登记）
- 根分区放 /driver、/usr/bin、/usr/lib、/lib、/local、/test 等用户态文件（经 manifest partition=2 登记）
- 非构建产物（libinput quirks、README）保持显式 mcopy（不经 manifest），mkdisk 注释标明

### 添加新源文件

- 内核：add_kernel_object(lib_name SOURCES ... ASM_SOURCES ...)
- 用户态库：add_user_lib(lib_name SOURCES ...)
- 用户态 ELF：add_user_elf(name [C] SOURCES ... [LINK_LIBS ...])

### 关键源码位置

- 工具链：build_script/cmake/toolchain-x86_64.cmake
- 内核规则：build_script/cmake/kernel_rules.cmake
- 用户态规则：build_script/cmake/user_rules.cmake
- 第三方规则：build_script/cmake/third_party_rules.cmake
- 磁盘镜像 manifest：build_script/cmake/image_rules.cmake
- 链接脚本：build_script/linker.ld
- 磁盘映像：build_script/mkdisk.sh
- libc++ 默认增量构建：build_script/build_libcxx.sh
- 顶层构建：CMakeLists.txt / build.sh

第三方库 per-lib 构建规则住 `build_script/third_party/<lib>/`，从 `user/CMakeLists.txt`
经 `include()` 按序引入（与内联时同作用域，target/变量行为不变）：

- libinput：build_script/third_party/libinput/libinput.cmake
- libdrm：build_script/third_party/libdrm/libdrm.cmake（config 模板 `config.h`/`libdrm.map` 同目录）
- libffi：build_script/third_party/libffi/libffi.cmake（config 模板 `fficonfig.h` 同目录）
- libexpat：build_script/third_party/libexpat/libexpat.cmake（config 模板 `expat_config.h`/`expat_config_host.h` 同目录）
- wayland：build_script/third_party/wayland/wayland.cmake（config 模板 `config.h` 同目录）
- musl：build_script/third_party/musl/musl_rules.cmake（入口：顶部 `musl_generate_headers()`
  + `MUSL_DIR` 别名；loader/crt + `musl_libc` 聚合 target；末尾 include 14 个模块文件）；
  modules/{unistd,fcntl,socket,dl,dirent,mman,stdio,multibyte,wchar,pthread,string,math,
  stdlib,time}.cmake（编进 libc 的 14 个 musl OBJECT 库，从 user/CMakeLists.txt 迁出）。
  顶层 CMakeLists.txt 仍只 include musl_rules.cmake（单入口），与内联时同作用域。

include 顺序约束：libffi → libexpat → wayland。wayland 的 host wayland-scanner 在
configure 期读 `LIBEXPAT_DIR`（由 libexpat.cmake 设置）拼 host-scanner flags 与 DEPENDS，
故 libexpat 必须先 include；wayland 的 .so 经 `SO_LINK_LIBS ffi` 依赖 libffi 的 `ffi` target。
libdrm 的 include 须早于同文件内留存的 `drm_test_link` 测试 ELF（其链接 `drm`/`drm_so`）。


## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| install-headers/libs.sh manifest 化 | 同 mkdisk manifest 思路，install 脚本消费 CMake manifest 而非硬编码头/库清单 | 低 |
| ld --remove-section 替代 objcopy | ld 命令中加 --remove-section .note.gnu.property，省掉中间 stripped.o | 低 |
| 用户态放开 red zone / SSE | 内核实现 FXSAVE/RXRSTORE 后，用户态可移除 -mno-red-zone -mno-sse -mno-sse2 -mno-mmx | 中 |
| 缺少 -mcmodel=kernel | C 文件用 -fno-pie 绝对寻址，应改为 -mcmodel=kernel | 低 |
| 链接脚本缺 section 对齐 | linker.ld 无 section 对齐声明，无法设置不同页权限 | 低 |
| disk.img 扩容至 ≥8GB | 当前 64MB 不足 clang/LLVM 构建（~5GB 产物），需解决 FAT32 4GB 单文件上限 | 中 |
