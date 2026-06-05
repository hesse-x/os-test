# CMake 重构实录

> **历史文档**：这是 x86-32 阶段的 CMake 重构记录。当前代码已迁移至 x86-64。
>
> 当前构建体系概要：
> - toolchain: `cmake/toolchain-x86_64.cmake`（`-m64`）
> - CXX_FLAGS: `-mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx`（非 `-fPIE`/GOTOFF）
> - 架构目录: `arch/x64/`（非 `arch/x86/`）
> - 链接: `ld -m elf_x86_64 -T arch/x64/linker.ld`
> - 产物: `build/myos.elf`（非 myos.bin）
> - `POSITION_INDEPENDENT_CODE OFF` 仍不可设（会加 `-fPIC`，破坏 `-mcmodel=kernel`）
> - 参见 CLAUDE.md 了解当前架构

## 目标

将当前扁平目录结构拆分为分层架构，用 CMake 替代 build.sh 中的硬编码编译命令。

## 目录结构

```
cmake/
  toolchain-i686.cmake
  do_link.cmake
CMakeLists.txt
build.sh
mkiso.sh
run.sh
grub.cfg
.clang-format
.gitignore
CLAUDE.md

arch/x86/
  CMakeLists.txt
  start.S
  vectors.S
  trapentry.S
  linker.ld
  boot.cc
  paging.cc / paging.h
  trap.cc / trap.h
  utils.h
  multiboot2.h

kernel/
  CMakeLists.txt
  kernel.cc / kernel.h
  mem/
    CMakeLists.txt
    alloc.cc / alloc.h
  trap.cc / trap.h
  serial.cc / serial.h

driver/
  CMakeLists.txt
  kbd.cc / kbd.h
  fb.cc / fb.h

common/
  common.h
  macro.h
```

## 文件拆分规则

### mem.cc → arch/x86/paging.cc + kernel/mem/alloc.cc

| 原位置 | 目标文件 | 内容 |
|---|---|---|
| `enable_page`, 页表定义, GDT设置, `page_directory/page_table` 声明, VMA/LMA 常量, `bump_alloc`, `bump_init_phys` | `arch/x86/paging.cc/h` | 物理地址运行的初始分页，GDT，地址常量，bump 分配器（arch 层因为需要 PHY_ADDR） |
| `init_mem`, BFC 分配器, `Page`/`BFCAllocator` | `kernel/mem/alloc.cc/h` | 与架构无关的分配逻辑 |

`init_mem` 中直接操作 `page_directory[]` 的代码（扩展 higher-half 映射、flush TLB）抽到 `arch/x86/paging.cc` 暴露接口：
- `extend_mapping(uint64_t max_phys_addr)` — 扩展 higher-half 映射 + 计算 device_vma_base
- `flush_tlb()` — 刷新 TLB
- `bump_end_phys()` — 查询 bump 分配器的物理结束地址

`kernel/mem` 只调接口不碰 arch 细节。

### isr.cc → arch/x86/trap.cc + kernel/trap.cc

| 原位置 | 目标文件 | 内容 |
|---|---|---|
| IDT 设置, PIC 重映射, PIT 初始化, `idt_gate_t`/`idt_register_t`/`trapframe_t` | `arch/x86/trap.cc/h` | 硬件中断基础设施 |
| `trap()` 分发逻辑 | `kernel/trap.cc/h` | handler 注册表 + `trap_dispatch(trapframe*)` |

分发机制采用注册表：
```cpp
// kernel/trap.h
typedef void (*irq_handler_t)(trapframe_t*);
void register_irq(int vec, irq_handler_t fn);
void trap_dispatch(trapframe_t* tf);
```

arch 层入口汇编（trapentry.S）组装 trapframe 后调用 `trap_dispatch`，kernel 层查表调用 handler + 处理 EOI。定时器/键盘在 `isr_init` 中通过 `register_irq` 注册。

### 头文件去重

- `outb`/`inb` → 统一到 `arch/x86/utils.h`，删除 `serial.cc` 和 `isr.h` 中的重复定义
- `KERNEL_CS`/`L16`/`H16` → 统一到 `arch/x86/utils.h`，删除 `mem.h` 和 `isr.h` 中的重复定义
- `mem_layout.h` → 废弃删除，常量已入 `arch/x86/paging.h`
- `mem.h` 拆分后剩余部分 → `kernel/mem/alloc.h`

### kbd 和 serial 的归属

- `kbd.cc/h` → `driver/`，arch 依赖（`inb(0x60)`、scancode 表）暂不拆，换架构时再抽 I/O 层
- `serial.cc/h` → `kernel/`，作为 debug 基础设施而非普通驱动

### 其他文件

- `boot.cc` → `arch/x86/`，Multiboot2 头 + 引导栈
- `start.S` → `arch/x86/`
- `vectors.S` + `trapentry.S` → `arch/x86/`
- `linker.ld` → `arch/x86/`
- `multiboot2.h` → `arch/x86/`
- `common.h` + `macro.h` → `common/`
- `kernel.cc/h` → `kernel/`
- `fb.cc/h` → `driver/`

## CMake 构建体系

### Toolchain file

```cmake
# cmake/toolchain-i686.cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_ASM_COMPILER gcc)
set(CMAKE_C_FLAGS "-m32" CACHE STRING "")
set(CMAKE_CXX_FLAGS "-m32" CACHE STRING "")
set(CMAKE_ASM_FLAGS "-m32" CACHE STRING "")

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
```

`CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` 必须——没有 32 位 libc，默认的编译器检测会失败。

### add_library(OBJECT) 结构

各子目录使用 `add_library(OBJECT)` 编译目标文件：

```cmake
# arch/x86/CMakeLists.txt 示例
add_library(arch_x86 OBJECT
    start.S vectors.S trapentry.S
    boot.cc paging.cc trap.cc
)
target_include_directories(arch_x86 PRIVATE ${CMAKE_SOURCE_DIR})
```

**不要设置 `POSITION_INDEPENDENT_CODE ON`！** 这会加 `-fPIC`，破坏 GOTOFF 机制导致 `enable_page` 等物理地址运行的函数崩溃。只用顶层 `CMAKE_CXX_FLAGS` 中的 `-fPIE`。

### 链接

`$<TARGET_OBJECTS:...>` 生成器表达式产生分号分隔的列表，直接用在 `add_custom_command(COMMAND ...)` 中会被 shell 解释为命令分隔符。解决方案是使用 CMake 脚本中转：

```cmake
# CMakeLists.txt
add_custom_command(
    OUTPUT ${BIN_FILE}
    COMMAND ${CMAKE_COMMAND}
        -DOBJ_LIST="$<TARGET_OBJECTS:arch_x86>;..."
        -DLINKER_SCRIPT=...
        -DBIN_FILE=...
        -P ${CMAKE_SOURCE_DIR}/cmake/do_link.cmake
    DEPENDS ...
)
```

```cmake
# cmake/do_link.cmake
string(REPLACE " " ";" OBJ_LIST "${OBJ_LIST}")  # 归一化空格→分号
execute_process(COMMAND ld -m elf_x86_64 -T ${LINKER_SCRIPT} ${OBJ_LIST} -o ${BIN_FILE})
```

### 产物路径

CMake 构建产物输出到 `build/` 目录。`myos.bin` 和 `myos.iso` 均在 `build/` 下。

## 构建脚本

### build.sh

```bash
#!/bin/bash
if [[ "$1" == "clear" ]]; then
    rm -rf build
    exit 0
fi
set -e
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-i686.cmake ..
make
cd ..
./mkiso.sh
```

### mkiso.sh

负责 ISO 生成：创建临时 iso 目录结构 → 复制 `build/myos.bin` → 复制 `grub.cfg` → `grub-mkrescue` → 清理临时目录。

### run.sh

调整路径指向 `build/myos.iso`。

### grub.cfg

静态文件放根目录，`mkiso.sh` 复制到 `iso/boot/grub/grub.cfg`。

## 踩坑记录

1. **`-fPIC` 破坏 GOTOFF**：`POSITION_INDEPENDENT_CODE ON` 给 OBJECT 库加 `-fPIC`，而 x86-32 下 `-fPIC` 用 GOT 间接寻址（不是 GOTOFF），导致物理地址运行阶段访问到错误地址。**只用 `-fPIE`。**
2. **CMake 编译器检测失败**：没有 32 位 libc 时 CMake 默认尝试编译链接可执行文件会失败。设置 `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` 跳过链接步骤。
3. **`$<TARGET_OBJECTS>` 的分号问题**：生成器表达式产生分号分隔列表，在 shell 命令中分号被解释为命令分隔符。用 `-P` 脚本中转解决。
4. **头文件路径**：旧代码用 `#include "multiboot2.h"` 等短路径，移动到子目录后需改为 `#include "arch/x86/multiboot2.h"` 等完整路径（从项目根起算，因为 `include_directories(${CMAKE_SOURCE_DIR})`）。
