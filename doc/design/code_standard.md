# 代码规范

## 总则

- **格式化**:全部目录统一 **LLVM 风格**(2 空格缩进、大括号不换行、列宽 80)。仓库根目录单一 `.clang-format`(`BasedOnStyle: LLVM`),所有目录共用,无需按目录分发。
- **命名**:格式由 clang-format 保证;命名**无自动强制工具**——clang-tidy `readability-identifier-naming` 无法按文件/目录剥离 POSIX 接口与 Linux 内部实现,亦无法处理 `_t` 后缀的 POSIX/Linux 归属冲突,故命名靠**人工遵循 + code review 兜底**(clang-tidy 至多校验纯大小写形式,不作 POSIX/Linux 命名判定)。
- **头文件**:由 CMake 的 `-nostdinc -isystem` 配置决定分类规则(见下文「头文件引用」)。

## 命名约定

### 内核与用户态代码(`arch/` `kernel/` `init/` `boot/` `user/`)

**全部遵循 Linux 命名标准**,不分 C/C++:

> **上位原则:已有惯例命名不受项目编程规范约束。** 凡命名受外部权威规范 / 惯例约束的符号——POSIX 接口(`open`/`fork`/`O_RDONLY`/`pid_t`/`ssize_t`)、C/glibc 标准库符号(`memcpy`/`printf`/`FILE`/`NULL`)、硬件 spec 寄存器 / 字段符号(AHCI `PxCLB`/`PxIS`、PCI/XHCI 配置空间偏移等)、网络协议字段名(ether header `ether_type`、IP header `ihl`/`ttl` 等)——**保留其规范原命名**,不改成项目 snake_case / SCREAMING。字面与规范 / spec / 协议对照有真实价值(读者拿规范对照代码时符号能一一对上),区别于作者自造的命名偏好(后者无外部约束,必须服从项目规范)。具体例外的判定标准与已知清单见下各条。

- 函数 / 变量 / 类型(struct/union/enum):`snake_case`(如 `sys_open`、`nr_threads`、`task_struct`)
- 宏 / 常量:`SCREAMING_SNAKE_CASE`(如 `PAGE_SIZE`、`GFP_KERNEL`)

  **例外:硬件 spec 照搬符号保留原大小写**(与 POSIX 接口 `open`/`O_RDONLY` 同理——受外部权威规范约束,字面对应有真实价值,非作者自造命名偏好):

  - **AHCI 端口寄存器偏移宏 — `kernel/driver/ahci.c` 的 `Px*` 族**(`PxCLB`/`PxIS`/`PxCMD`/`PxTFD`/`PxSIG`/`PxSSTS`/`PxSCTL`/`PxSERR`/`PxSACT`/`PxCI`/...)。`Px` = "Port x"(AHCI spec 1.3 里端口寄存器一章的符号前缀),后半段是字段助记符(`CLB` = Command List Base、`IS` = Interrupt Status、`CMD` = Command and Status)。照搬 spec 原大小写,让驱动代码和 AHCI spec 章节符号字面对应——读者拿 spec 对照寄存器偏移时,`PxCLB 0x00`/`PxIS 0x10` 直接对得上。这些宏只在 `ahci.c` 内部用(传给 MMIO 读写),不跨文件、不对外、无 ABI,改名无收益反失 spec 可对照性。
- 函数指针 typedef:`*_fn` 后缀(如 `irq_handler_fn`、`syscall_fn`、`signal_check_fn`),不用 `_t`
- 非 POSIX 的内核/UAPI 类型**不带 `_t`**(Linux 风格,如 `recv_msg`、`input_event`、`pci_dev_info`、`sockaddr`、`iovec`);POSIX 定义的标准类型(`pid_t`/`ssize_t`/`mode_t`/`sigset_t`/`siginfo_t`/`sa_family_t`/`nfds_t`/`socklen_t`/`sigaction_t`/`tcflag_t`/`cc_t`/`uid_t`/`gid_t`/`dev_t`/`ino_t`/`off_t`/`time_t` 等)按 POSIX 保留 `_t`

  **例外:两类带技术动因的 `_t` 保留**(与 Linux 上游一致,非历史包袱):

  - **访问器封装型 — `atomic_t` / `refcount_t`**:`_t` 是「此类型不可直接当 int 用」的语义信号——外部强制走 `atomic_read()`/`atomic_set()`/`refcount_inc()` 等访问器,不直接取 `v->counter` 字段。去 `_t` 不影响功能,但丢失这层「封装意图」标记。与 Linux `include/linux/types.h` 一致。
  - **sparse `__bitwise` 强类型型 — `phys_addr_t` / `kern_vaddr_t`**:typedef 上带 sparse `__bitwise` 注解,sparse 在编译期报错物理地址 / 虚拟地址混用(把 `phys_addr_t` 当 `kern_vaddr_t` 传给期望虚拟地址的函数会报 warning)。`_t` 与 `__bitwise` 配套(注解在 typedef 上,去 `_t` 不影响 sparse 功能,但与上游 Linux 命名脱钩)。定义在 `kernel/xcore/sparse.h`。

  判定「该去 `_t` 还是保留」的标准:**有无技术动因**。「无技术动因」清单(纯内部、无 ABI、无访问器封装、无 sparse 强类型——如 `gdt_*_t`/`trapframe_t`/`acpi_*_t`/`efi_*_t`/`elf_load_result_t`/`xhci_intr_t`/`trb_t`/`local_switch_frame_t`)全改;上述两类有现实动因,保留。

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

### `include/uapi/xos/` 不是「对外公开 API」

UAPI 这个名字容易误读。`include/uapi/xos/` 的真实角色是**内核↔用户态共享的内部 ABI**——被 `kernel/` 和 `user/` **两侧**同时引用(如 `struct kernel_mem_stats` 内核填、用户态读;`SYS_*` 号用户态发、内核 dispatch 表查)。它的「公开」是相对于「内核内部头」(只内核可见)而言:用户态也能 include,不依赖内核内部机制。

**真正「对外」的边界是 `user/include/`**:那里装的是 `stdio.h`/`string.h`/`fcntl.h` 等 POSIX 名公开头,面向用户程序直接 `#include <stdio.h>`。`include/uapi/xos/` 用 `xos/` 自造前缀,不是给用户程序直接引的,是给内核和 libc 内部共享 ABI 用的。`build_script/install-headers.sh` 把这两棵树都发布到 sysroot(`xos/` → `$DEST/xos/`、`user/include/` → `$DEST/`),用户程序只感知 `user/include/` 那一层。

### UAPI 头只装数据布局/常量,绝不装 C 函数原型

这是 Linux UAPI 铁律,也是 `install-headers.sh` 的 zero-rewrite 设计能成立的前提。UAPI 头装的是跨边界共享的 ABI 契约:struct/enum/typedef/`#define` 常量/`static inline` 函数定义(定义在头里、无外部链接)。**不装 `extern` 函数原型声明**——函数签名是实现细节,会随版本变;数据布局是 ABI 契约,跨版本稳定。

违反这条的典型是混合头:既装共享数据,又装 C 函数原型。例如 `user/include/input.h` 曾把 `enum input_key`/`struct key_event`(共享 ABI)和 `input_client_poll`/`input_driver_run`(libc 函数原型 `extern`)混在一个文件里。修法是按内容拆:UAPI 数据 → `include/uapi/xos/input_key.h`(`<xos/input_key.h>`),函数原型 → 内部头 `"user/include/input_lib.h"`。

### `install-headers.sh` 的 zero-rewrite 依赖源码写法符合 sysroot 布局

`build_script/install-headers.sh` 把头文件 verbatim 拷贝到 sysroot(`include/uapi/xos/` → `$DEST/xos/`、`user/include/` → `$DEST/`、`user/include/sys/` → `$DEST/sys/`),**不重写任何 include 路径**。这能成立,是因为源码里的写法在 sysroot 布局下也能解析:

- `<xos/foo.h>` 尖括号:源码里 `-I include/uapi` 解析到 `include/uapi/xos/foo.h`;sysroot 里 `-I $DEST` 解析到 `$DEST/xos/foo.h`。两边都是「`-I <根>` + `xos/` 子目录」的同构布局,尖括号在两边都对。
- 若用引号 `"xos/foo.h"`:源码里靠 `-I` 也能解析,但语义错(公开头该尖括号),且引号会先查源文件所在目录,同名歧义时易误解析。

所以「公开头一律尖括号 + 逻辑前缀」不只是风格偏好,是 sysroot 发布机制能 zero-rewrite 成立的硬约束。脚本自带的 closure check(对每个 published header 预处理探针)是这条约束的回归守卫——源码若混入引号公开头或 UAPI 函数原型,该 check 会 FAIL。

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
- 安装 clang-tidy(工具链为 gcc,clang-tidy 仅做语义解析不链接,需实测 `-ffreestanding -nostdlib -fPIE -mno-red-zone -std=gnu17` 及自定义 `-I` 的误报率)。

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

## include 检查(include-what-you-use)

由 `build_script/iwyu_check.sh` 执行(`check.sh --filter iwyu` 调用),全仓 `.c/.cc` 覆盖,排除 `third_party/`、跳过 `.S`。映射文件 `build_script/iwyu.imp`,工具内置 glibc 归属表与本项目 `-nostdinc + 自造 libc` 架构的冲突由脚本解析层处理。

### 判定标准(机器可判,无需肉眼)

iwyu 的每条 `should add` 建议按以下规则二分,不靠人工权衡:

- **不可解析 = 噪音,自动滤掉**:iwyu 建议 `+ #include <H>`,但 `<H>` 在该 TU 搜索路径下编译器解析不到。这类建议源自 iwyu 内置 glibc 符号归属表(把 `struct timeval`→`<sys/time.h>`、`pid_t`→`<sys/types.h>` 等),在本项目里那些 glibc 公开头多数不存在。`iwyu_check.sh` 的解析层按 TU 类别滤除:
  - 内核 TU(搜索路径仅 freestanding 四件套 + `<xos/*>`):任何非 freestanding、非 `<xos/*>` 的尖括号公开头都判不可解析,滤除。
  - 用户态 TU(额外有 `user/include/`):仅 `<sys/time.h>`、`<inttypes.h>` 等无 wrapper 的头判不可解析,滤除;`<sys/types.h>`、`<unistd.h>` 等 wrapper 存在的头**不滤**(见下)。
- **可解析 = 真建议,照报**:iwyu 建议 `+ #include <H>` 且 `<H>` 可解析。这是真实建议,处理方式二选一,都是机械动作:
  1. **照做**:加上该 include(如 `sys_wait.cc` 用 `pid_t`,加 `#include <sys/types.h>`)。
  2. **豁免**:现状合理但不想改,在该 include 行加 `// IWYU pragma: keep`(见下「豁免」)。

`should remove`(删未用 include)一律照报,不滤——但注意 iwyu 偶有「因建议了 bogus 头而误判某 include 没用」的连锁误删(如建议 bogus `<sys/time.h>` 后误判 `<time.h>` 无用)。这类 `-` 行若涉及 `struct timeval` 等仍在用的符号,判为 bogus 连锁孤儿,手动忽略。

### 豁免规则(`// IWYU pragma: keep` / `no_include`)

以下两类 iwyu 建议虽「合法」但违背项目设计,**明确豁免**,不算肉眼妥协:

- **外部聚合头不拆**:来自 host 的聚合头(`<efi.h>`、`<efilib.h>` 等 gnu-efi 头;`<stdint.h>` 等 freestanding 头)设计上只引一个、内部拉齐子头。iwyu 会要求拆成子头(如 `stub.c` 拆出 `efiapi.h/efibind.h/...`)——**不拆**。处理:聚合头行加 `// IWYU pragma: keep`;若 iwyu 仍建议子头,在文件头加 `// IWYU pragma: no_include "子头.h"` 显式拒绝(见 `boot/stub.c` 范例)。
- **传递包含够用**:某符号已通过现有 include 传递获得,iwyu 仍要求显式引规范头。若项目接受传递包含,在被传递的 include 行加 `// IWYU pragma: keep` 说明依赖关系。

### 映射文件 `iwyu.imp` 的边界

`iwyu.imp` 用 header→header 改写映射(如 `["<signal.h>" → "<xos/signal.h>"]`),**仅对「iwyu 想改写已 include 的头」生效**;对「iwyu 新增建议某头」(本项目主要冲突点)基本无效——后者由 `iwyu_check.sh` 解析层的「不可解析即滤」规则兜底,不靠映射。映射只在「外部库头被误归到 glibc 公开头、需改写到项目头」时少量有用,新增需实测验证是否生效,勿盲目堆砌。

