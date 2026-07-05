# 代码格式化方案

## 总则

- **格式化**:全部目录统一 **LLVM 风格**(2 空格缩进、大括号不换行、列宽 80)。仓库根目录单一 `.clang-format`(`BasedOnStyle: LLVM`),所有目录共用,无需按目录分发。
- **命名**:格式由 clang-format 保证;命名**无自动强制工具**——clang-tidy `readability-identifier-naming` 无法按文件/目录剥离 POSIX 接口与 Linux 内部实现,亦无法处理 `_t` 后缀的 POSIX/Linux 归属冲突,故命名靠**人工遵循 + code review 兜底**(clang-tidy 至多校验纯大小写形式,不作 POSIX/Linux 命名判定)。
- **头文件**:由 CMake 的 `-nostdinc -isystem` 配置决定分类规则(见下文「头文件引用」)。

## 命名约定

### 内核与用户态代码(`arch/` `kernel/` `init/` `boot/` `user/`)

**全部遵循 Linux 命名标准**,不分 C/C++:

- 函数 / 变量 / 类型(struct/union/enum):`snake_case`(如 `sys_open`、`nr_threads`、`task_struct`)
- 宏 / 常量:`SCREAMING_SNAKE_CASE`(如 `PAGE_SIZE`、`GFP_KERNEL`)
- 函数指针 typedef:`*_fn` 后缀(如 `irq_handler_fn`、`syscall_fn`、`signal_check_fn`),不用 `_t`
- 非 POSIX 的内核/UAPI 类型**不带 `_t`**(Linux 风格,如 `recv_msg`、`input_event`、`pci_dev_info`、`sockaddr`、`iovec`);POSIX 定义的标准类型(`pid_t`/`ssize_t`/`mode_t`/`sigset_t`/`siginfo_t`/`sa_family_t`/`nfds_t`/`socklen_t`/`sigaction_t`/`tcflag_t`/`cc_t`/`uid_t`/`gid_t`/`dev_t`/`ino_t`/`off_t`/`time_t` 等)按 POSIX 保留 `_t`

> 用户态 C++ 代码(`user/driver/`、`user/shell/`、`user/lib/*.cc`)同样用 Linux snake_case,**不**采用 LLVM CamelCase——统一一套命名,降低内核↔用户态边界的心智负担。POSIX/UAPI 公开符号在 C++ 文件中亦保持原名。

### POSIX 接口

**POSIX 接口一律按 POSIX 命名标准命名**——函数名、类型名、宏名、头文件路径全部对齐 POSIX 规范(如 `open`/`read`/`write`/`fork`/`execve`、`pid_t`/`ssize_t`/`mode_t`、`O_RDONLY`、`<sys/types.h>`/`<fcntl.h>`)。无论该接口实现在内核态(`kernel/bsd/`)还是用户态(`user/lib/`),对外符号名必须与 POSIX 一致,不得改成 Linux 内核风格(如不写 `do_open`、`sys_open` 作为公开 API)。

> 内核内部实现可保留 Linux 风格的 `sys_xxx` 分发入口(`sys_open` 等),那是内核实现细节;POSIX 命名要求针对的是**对外暴露的接口符号**。

## 头文件引用

由构建系统统一决定(`CMakeLists.txt` 全局 `-nostdinc -isystem ${GCC_FREESTANDING_INC}`,`user_rules.cmake` 追加 `-I` 指向 `${CMAKE_SOURCE_DIR}` / `include/uapi` / `user/include`):

| 头文件类别 | 引用方式 | 解析路径 | 示例 |
|------|------|------|------|
| **C 标准头**(freestanding) | 尖括号,不加完整路径 | `-isystem ${GCC_FREESTANDING_INC}` | `<stdint.h>` `<stddef.h>` `<stdarg.h>` `<stdbool.h>` |
| **系统头 / UAPI / libc 公开头** | 尖括号,不加完整路径(用其逻辑路径前缀) | `-I include/uapi`、`-I user/include` | `<sys/types.h>` `<sys/mman.h>` `<fcntl.h>` `<unistd.h>` `<xos/errno.h>` `<xos/elf.h>` |
| **项目内部头**(其余一切) | 双引号,加完整路径(相对仓库根) | `-I ${CMAKE_SOURCE_DIR}` | `"arch/x64/utils.h"` `"kernel/xcore/log.h"` `"kernel/bsd/inode.h"` `"user/driver/display.h"` |

规则要点:

- **C 标准头 / 系统头用 `< >`,不写完整路径**:这些是「对外稳定」的接口边界,引用形式应与 POSIX/标准库一致。`<xos/*.h>` 走 UAPI 同一套机制,用 `< >` + `xos/` 逻辑前缀,不写 `include/uapi/xos/...` 物理路径。
- **项目内部头用 `" "`,写完整路径**:相对仓库根的路径(如 `"kernel/xcore/mem/alloc.h"`),与 `-I ${CMAKE_SOURCE_DIR}` 配合。不依赖相邻目录的相对路径,移动文件时不易失联。
- **不混用**:内部头不得用 `< >`,C 标准/系统头不得用 `" "`。

> 历史的「按物理目录决定尖括号/双引号」已过时,以本节 CMake 配置为准。

## 目录划分

| 目录 | 语言 | 格式 | 命名 |
|------|------|------|------|
| `arch/` `kernel/` `init/` `boot/` | C | LLVM | Linux snake_case |
| `user/`(C 与 C++ 全部) | C / C++ | LLVM | Linux snake_case |
| POSIX 接口(跨内核/用户态) | C | LLVM | POSIX 命名标准 |

## 命名检查(clang-tidy,仅校验大小写形式)

> clang-tidy `readability-identifier-naming` **只**能校验符号的纯大小写形式(`lower_case` / `UPPER_CASE`),**无法**判定 POSIX 接口归属、无法剥离 POSIX 接口与 Linux 内部实现、无法处理 `_t` 后缀的 POSIX/Linux 归属冲突。故 clang-tidy 仅作大小写形式的辅助检查,POSIX/Linux 命名归属靠人工 + code review。

### 前提

- CMake 导出编译数据库:`set(CMAKE_EXPORT_COMPILE_COMMANDS ON)`(根 CMakeLists.txt),产出 `build/compile_commands.json`。
- 安装 clang-tidy(本机当前未装;工具链为 gcc,clang-tidy 仅做语义解析不链接,需实测 `-ffreestanding -nostdlib -fPIE -mno-red-zone -std=gnu17` 及自定义 `-I` 的误报率)。

### 策略

- **只查增量,不整改存量**:CI/hook 仅对 `git diff` 触及的文件/行跑 clang-tidy,历史代码不背锅,规避 `--fix` 的语义改写风险。
- **单一根 `.clang-tidy`**:C 与 C++ 统一 Linux snake_case,全仓库一套规则,无需按目录分发。
- **不进实时构建**:identifier-naming 需语义分析,比 clang-format 慢一两个量级,只作提交前/CI 检查,不挂进 `build.sh`。

### `.clang-tidy` 规则(全仓库,根目录)

```yaml
Checks: 'readability-identifier-naming'
HeaderFilterRegex: '.*'
CheckOptions:
  - { key: readability-identifier-naming.FunctionCase,        value: lower_case }
  - { key: readability-identifier-naming.VariableCase,        value: lower_case }
  - { key: readability-identifier-naming.ParameterCase,       value: lower_case }
  - { key: readability-identifier-naming.StructCase,          value: lower_case }
  - { key: readability-identifier-naming.UnionCase,           value: lower_case }
  - { key: readability-identifier-naming.EnumCase,            value: lower_case }
  - { key: readability-identifier-naming.EnumConstantCase,    value: UPPER_CASE }
  - { key: readability-identifier-naming.MacroDefinitionCase, value: UPPER_CASE }
```

> **常量**:不设 `ConstantCase`——枚举常量、宏走 `UPPER_CASE`,其余常量(`const` 变量)随 `VariableCase`(`lower_case`)。
> **类型后缀**:不强制 `_t`——Linux 内核风格 struct/union/enum 本身用 `snake_case`,typedef 名随类型走 `lower_case`;POSIX 定义的 `pid_t`/`ssize_t`/`mode_t` 等由 POSIX 接口约束保留 `_t`,不在此规则覆盖。函数指针 typedef 统一 `*_fn`(见上文命名约定),不走 `_t`。Linux 当下仍保留 `_t` 的少量内核类型(`atomic_t`/`refcount_t` 走访问器封装、`phys_addr_t`/`kern_vaddr_t` 走 sparse `__bitwise` 强类型)有其现实动因,是否保留由具体评估决定,不归 clang-tidy 管。

### 增量运行

```bash
# 只检查本次改动触及的文件(需先 cmake 生成 compile_commands.json)
git diff --name-only --diff-filter=d origin/master...HEAD -- '*.c' '*.h' \
  | run-clang-tidy -p build/compile_commands.json
```

## 待办

无（目录重组 `driver/`→`user/driver/`、`shell/`→`user/shell/` 已完成；全目录统一 LLVM 风格，无需按目录分发的 `format.sh`）。
