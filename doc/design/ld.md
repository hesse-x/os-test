# ld.so 动态链接方案设计

本文是 `ld.md` 技术选型的工程落地方案。选型理由见 `ld.md`，本文只聚焦**怎么实现**。

## 1 项目概述

### 1.1 背景与目标

支持动态链接，让自研 OS 能跑动态 ELF + 共享 libc.so，为 Wayland/gcc 验收铺路。选型路线 B：内核加载主 ELF + ld.so 两个镜像，auxv 传递信息，ld.so 在用户态做重定位/GOT/PLT/TLS，最后跳主 ELF `main`。内核改动最小，复用现有 `PT_LOAD` 路径。

完整背景与 23 条选型决策见 `ld.md`。

### 1.2 范围边界

**阶段一做：**

- 内核 execve 解析 `PT_INTERP`、加载 ld.so、建 argc/argv/envp/auxv 栈
- 自研 ld.so：自举重定位 → 加载 libc.so → eager 重定位 → 跳主 ELF
- libc.a + libc.so 同源双产物，`#if DYNAMIC` 编译期分流
- `__libc_start_main` 统一启动路径（静态/动态共用），TLS 模板发现双源汇入 `tls_info`
- 统一 `_start`（静态 hello 顺便修 argc/argv 缺陷）
- `sys_arch_prctl` + `__trapret`/`syscall_fast_entry` 写 FS_BASE MSR
- hello_dyn 动态验证 + 测试 ELF 全改动态

**阶段一不做：**

- `dlopen`/`dlsym`/`dlerror`/`dlclose`（阶段二 Wayland/gcc 真需要时再加）
- ASLR/PIE（固定基址，无随机化基础设施）
- lazy binding（全 eager，省掉 `_dl_runtime_resolve` 汇编）
- 符号版本（`GNU_symver`）
- libgcc.a 链接（撞到 undefined symbol 再补，届时静态链进主 ELF）
- 文件映射 mmap（ld.so 用 read + 匿名映射加载 .so，未来切换不影响 ABI）

### 1.3 核心约束

| 约束 | 值 | 来源 |
|------|-----|------|
| 主 ELF 基址 | `0x400000`（非 PIE） | ld.md #3 |
| ld.so 基址 | `0x7FFFFF000000` 附近固定高位 | ld.md #3 |
| TLS 布局 | variant II | ld.md #12 |
| 重定位策略 | eager binding | ld.md #10 |
| 符号查找 | DT_GNU_HASH only | 本方案补充 |
| 重定位类型 | 标准 PIC 全集 9 种 | 本方案补充 |
| 验证阶段 | 插入阶段 2a bootstrap-only | 本方案补充 |
| 工具链 | 自研 gcc + 自研 libc.so（不用宿主机 glibc） | ld.md #7 |

### 1.4 决策索引

| 决策 | 来源 | 本文定位 |
|------|------|---------|
| 路线 B（内核加载 + 用户态重定位） | ld.md #1 | §2.1, §3.1 |
| 自研 ld.so | ld.md #2（**偏离**：自带最小 libc，不链 libc.a，§7.1） | §3.2-3.3 |
| 固定基址无 ASLR | ld.md #3 | §1.3 |
| read + 匿名映射加载 .so | ld.md #4 | §3.3 |
| libc.a + libc.so 同源双产物 | ld.md #5 | §3.4 |
| 镜像只打包 libc.so | ld.md #6 | §7.2 |
| 动态程序范围 | ld.md #7 | §6.5-6.6 |
| auxv 协议与栈布局 | ld.md #8 | §4.1 |
| ld.so 自举重定位 | ld.md #9 | §3.2 |
| 全 eager binding | ld.md #10 | §3.3 |
| 统一启动路径 | ld.md #11 | §3.5, §3.7 |
| TLS 布局由 `__libc_start_main` 算 | ld.md #12 | §3.5, §3.6 |
| `tls_info` 桥接结构 | ld.md #13 | §3.6, §4.3 |
| 先做 pthread 再做 ld.so | ld.md #14 | §6.1 |
| `_dl_link_map` 全局变量 | ld.md #15 | §3.3, §4.4 |
| 阶段一不做 dlopen | ld.md #16 | §1.2 |
| `#if DYNAMIC` 编译期分流 | ld.md #17 | §3.4, §3.5 |
| 阶段 2+3 合并 | ld.md #18 | §6.4（拆为 2a + 2b+3） |
| 动态 ELF 用 gcc 驱动链接 | ld.md #19 | §7.1 |
| libgcc 暂不处理 | ld.md #20 | §1.2 |
| 调试可观测（内核+ld.so 双打印） | ld.md #21 | §5.5 |
| ld.so 不用 TLS 变量 | ld.md #22 | §3.2 |
| 不用 `-Bsymbolic` | ld.md #23 | §3.4 |
| 重定位类型支持范围 | 本方案补充 | §3.3 |
| 符号哈希表选型 | 本方案补充 | §3.3 |
| 插入 bootstrap-only 验证 | 本方案补充 | §6.3 |

## 2 总体架构设计

### 2.1 分层架构图

```
┌─────────────────────────────────────────────────────────────┐
│ 用户态                                                       │
│                                                              │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐ │
│  │ hello_dyn│   │ 测试 ELF │   │ init/    │   │ pthread  │ │
│  │ (动态)   │   │ (动态)   │   │ shell    │   │ 库       │ │
│  │          │   │          │   │ (静态)   │   │ (.a)     │ │
│  └────┬─────┘   └────┬─────┘   └────┬─────┘   └────┬─────┘ │
│       │              │              │              │        │
│       └──────┬───────┴──────┬───────┘              │        │
│              │              │                      │        │
│         ┌────▼─────┐  ┌─────▼──────┐               │        │
│         │ libc.so  │  │ libc.a     │               │        │
│         │ (动态)   │  │ (静态)     │               │        │
│         │ -DDYNAMIC│  │ -DNO_DYNAMIC│              │        │
│         │  =1      │  │            │               │        │
│         └────┬─────┘  └─────┬──────┘               │        │
│              │              │                      │        │
│         ┌────▼─────┐        │                      │        │
│         │ ld.so    │        │                      │        │
│         │ (静态链  │        │                      │        │
│         │  libc.a) │        │                      │        │
│         └────┬─────┘        │                      │        │
│              │              │                      │        │
│              │   tls_info ◄─┴──────────────────────┘        │
│              │   _dl_link_map                                │
│              │                                              │
└──────────────┼──────────────────────────────────────────────┘
               │ syscall
┌──────────────┼──────────────────────────────────────────────┐
│ 内核态       │                                              │
│  ┌───────────▼─────────────┐  ┌─────────────────────────┐  │
│  │ bsd/proc.c sys_execve   │  │ xcore/sched.c           │  │
│  │  - 解析 PT_INTERP       │  │  - __trapret            │  │
│  │  - 加载主 ELF + ld.so  │  │    写 FS_BASE MSR        │  │
│  │  - 建 argc/argv/envp/  │  │  - syscall_fast_entry    │  │
│  │    auxv 栈              │  │    写 FS_BASE MSR        │  │
│  │  - rip = ld.so entry    │  │                         │  │
│  │    (有 PT_INTERP)       │  │  xtask_t.fs_base 字段   │  │
│  └───────────┬─────────────┘  └─────────────────────────┘  │
│              │                                              │
│  ┌───────────▼─────────────┐  ┌─────────────────────────┐  │
│  │ bsd/syscall.c           │  │ bsd/elf_loader.c        │  │
│  │  - sys_arch_prctl       │  │  - PT_LOAD 加载路径     │  │
│  │    (ARCH_SET_FS/GET_FS) │  │  （复用，加载两个镜像） │  │
│  └─────────────────────────┘  └─────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 动态启动链路时序

```
用户敲 shell 命令
        │
        ▼
sys_execve(path="/bin/hello_dyn", argv, envp)
        │
        ├─ 1. 加载主 ELF（复用现有 elf_load PT_LOAD 路径）
        │     基址 0x400000，记录 entry、phdr、phnum、phent
        │
        ├─ 2. 解析 PT_INTERP → "/lib/ld.so"
        │
        ├─ 3. 加载 ld.so（复用 elf_load，固定基址 0x7FFFFF000000）
        │     记录 ld_base、ld_entry
        │
        ├─ 4. 建新用户栈：argc/argv/envp/auxv（标准 SysV ABI）
        │     auxv 含 AT_PHDR/PHENT/PHNUM/ENTRY/BASE/PAGESZ/RANDOM/EXECFN/NULL
        │
        ├─ 5. rip = ld_entry（有 PT_INTERP）/ 主 ELF entry（无，静态路径）
        │
        └─ 6. 内核侧打印 "exec: ld.so @ 0x..., entry 0x..., main @ 0x..."
                │
                ▼
ld.so _start（rsp 指向 argc）
        │
        ├─ 7. __dls3 bootstrap（纯算术，不依赖 GOT/printf）
        │     遍历自身 .rela.dyn，应用 R_X86_64_RELATIVE 自重定位
        │     dl_puts("dl: self-relocate done")
        │
        ├─ 8. 解析主 ELF .dynamic（via AT_PHDR/PHENT/PHNUM）
        │     找 DT_NEEDED → "libc.so"
        │
        ├─ 9. 加载 libc.so
        │     open("/lib/libc.so") → read 进用户态 buffer
        │     → mmap(MAP_PRIVATE|MAP_ANONYMOUS, len) → memcpy 段
        │     → BSS 清零
        │     dl_puts("dl: loaded libc.so @ 0x...")
        │
        ├─ 10. 重定位 libc.so
        │     遍历 .rela.dyn（RELATIVE/64/GLOB_DAT/...）
        │     遍历 .rela.plt（JUMP_SLOT，eager binding）
        │     符号查找：DT_GNU_HASH（bloom + bucket chain）
        │     dl_puts("dl: relocated N symbols")
        │
        ├─ 11. 构造 link_map 链表（主 ELF + libc.so）
        │     存全局 _dl_link_map
        │
        ├─ 12. 跳 AT_ENTRY（主 ELF _start）
        │     dl_puts("dl: jump to entry 0x...")
                │
                ▼
主 ELF _start（rsp 仍指向 argc，ld.so 已重定位好 GOT）
        │
        ├─ 13. 读栈：argc/argv/envp/auxv
        │
        ├─ 14. __libc_start_main(main, argc, argv, envp)
        │     （来自 libc.so，已重定位可调）
        │
        ├─ 15. __libc_start_main 内部：
        │     ├─ #if DYNAMIC：遍历 _dl_link_map 合并 PT_TLS → 填 tls_info
        │     │  #else：读链接器符号填 tls_info（单对象）
        │     ├─ arch_prctl(ARCH_SET_FS, tls_block_addr)
        │     ├─ 跑 .init_array
        │     ├─ 注册 .fini_array 到 atexit
        │     └─ main(argc, argv, envp)
                │
                ▼
main 执行
        │
        ▼
exit(status)
        │
        ├─ 跑 atexit handlers（含 .fini_array）
        └─ SYS_exit_group(status)
```

### 2.3 模块职责矩阵

| 职责 | 内核 execve | ld.so | libc.so | libc.a | 主 ELF | pthread |
|------|------------|-------|---------|--------|--------|---------|
| 加载主 ELF | **做** | — | — | — | — | — |
| 加载 ld.so | **做** | — | — | — | — | — |
| 建 argc/argv/envp/auxv 栈 | **做** | — | — | — | — | — |
| 自举重定位 ld.so | — | **做** | — | — | — | — |
| 加载 libc.so | — | **做** | — | — | — | — |
| 重定位 libc.so | — | **做** | — | — | — | — |
| eager binding | — | **做** | — | — | — | — |
| 构造 link_map | — | **做** | — | — | — | — |
| TLS 模板发现（动态） | — | — | **做**（`__libc_start_main`） | — | — | — |
| TLS 模板发现（静态） | — | — | — | **做**（`__libc_start_main`） | — | — |
| 设 FS_BASE MSR | — | — | **做**（arch_prctl） | **做**（arch_prctl） | — | — |
| 写 FS_BASE MSR（上下文切换） | **做**（`__trapret`） | — | — | — | — | — |
| 跑 .init_array | — | — | **做** | **做** | — | — |
| 跳 main | — | — | **做** | **做** | — | — |
| 提供 `__libc_start_main` | — | — | **做**（-DDYNAMIC=1） | **做**（-DDYNAMIC=0） | — | — |
| 分配新线程 TLS | — | — | — | — | — | **做**（读 tls_info） |
| clone(CLONE_SETTLS) | — | — | — | — | — | **做** |

### 2.4 静态/动态路径共存策略

**分流点：`PT_INTERP` 有无。**

```
sys_execve
    │
    ├─ 解析 ELF header
    ├─ 加载主 ELF（PT_LOAD，复用现有路径）
    │
    ├─ 遍历 PHDR 找 PT_INTERP？
    │   ├─ 有 → 动态路径
    │   │   ├─ 读 PT_INTERP 字符串 → "/lib/ld.so"
    │   │   ├─ 加载 ld.so（PT_LOAD，固定高位基址）
    │   │   ├─ 建 auxv 栈（含 AT_PHDR/ENTRY/BASE/...）
    │   │   └─ rip = ld_entry
    │   │
    │   └─ 无 → 静态路径
    │       ├─ 建 auxv 栈（auxv 仍建，静态 _start 阶段 1 不读，阶段 2+3 统一后读）
    │       └─ rip = 主 ELF entry（现状不变）
    │
    └─ 共同：argc/argv/envp 都建（ld.md #8，不半截实现）
```

**关键约束：auxv 改动对静态程序无感。**

阶段 1 内核改造后，静态 hello/测试仍正常——静态 `_start` 不读栈顶数据，auxv 多了当没看见。阶段 2+3 统一 `_start` 后静态才真正读栈。两阶段解耦，不破坏静态。

**回归验证点：**

- 阶段 1 完成：静态 hello + 全部静态测试 ELF 仍正常
- 阶段 2+3 完成：静态 hello argc/argv 正确（修掉既有缺陷）
- 阶段 4 完成：hello_dyn 跑通 + 静态路径仍正常

## 3 核心模块详细设计

### 3.1 内核 execve 改造

#### 3.1.1 设计目标

`sys_execve` 增量支持动态 ELF：

1. 解析 `PT_INTERP`，识别动态程序
2. 加载 ld.so（复用现有 `PT_LOAD` 路径，固定高位基址）
3. 构建 `argc/argv/envp/auxv` 栈（标准 SysV ABI）
4. 静态/动态分流入口点

**不引入文件映射 mmap**，ld.so 用 read + 匿名映射加载 .so（ld.md #4）。

#### 3.1.2 PT_INTERP 解析与 ld.so 加载

**前置：现有 `elf_load` 签名不匹配，需改造。** 现有 `elf_load(const uint8_t *data, uint64_t size, uint64_t *new_pml4)` 接收**已 kmalloc 的内核 buffer**（`sys_execve` 先读整个文件进 `kmalloc`，proc.c:741-745），返回 `elf_load_result_t {entry, success}`——**不含 PHDR 信息**。本方案需：

1. **扩展 `elf_load_result_t`** 加 `phdr_vaddr`/`phnum`/`phent` 字段（非 PIE 主 ELF：`phdr_vaddr = 0x400000 + e_phoff`，因首个 `PT_LOAD` 把 ELF header 映射在 vaddr 0）
2. **新增 `elf_load_at(data, size, new_pml4, base)`** 把 `PT_LOAD` 段映射到 `base + p_vaddr`（ld.so 是 `-shared -fPIC`，`p_vaddr` 是相对偏移，必须 base-offset 加载）

```c
// kernel/bsd/elf_loader.h 增量
typedef struct elf_load_result {
    uint64_t entry;
    uint64_t phdr_vaddr;   // 新增：PHDR 表用户态地址（AT_PHDR）
    uint64_t phnum;        // 新增（AT_PHNUM）
    uint64_t phent;        // 新增（AT_PHENT）
    bool     success;
} elf_load_result_t;

elf_load_result_t elf_load(const uint8_t *data, uint64_t size, uint64_t *new_pml4);
elf_load_result_t elf_load_at(const uint8_t *data, uint64_t size,
                              uint64_t *new_pml4, uint64_t base);
```

```c
// kernel/bsd/proc.c: sys_execve 增量

// 现有：读主 ELF 进 elf_buf（kmalloc），elf_load(elf_buf, size, new_pml4)
// main_lr 含：entry, phdr_vaddr, phnum, phent（扩展后）

// 新增：遍历已加载的 PHDR（elf_buf 内）找 PT_INTERP
char interp_path[PATH_MAX] = {0};
bool is_dynamic = false;
Elf64_Ehdr *eh = (Elf64_Ehdr *)elf_buf;
for (i = 0; i < eh->e_phnum; i++) {
    Elf64_Phdr *ph = (Elf64_Phdr *)(elf_buf + eh->e_phoff + i * eh->e_phentsize);
    if (ph->p_type == PT_INTERP) {
        if (ph->p_filesz >= PATH_MAX) return -ENOENT;  // 防呆
        memcpy(interp_path, elf_buf + ph->p_offset, ph->p_filesz);
        interp_path[ph->p_filesz] = '\0';
        is_dynamic = true;
        break;
    }
}

// 动态路径：加载 ld.so（同样 kmalloc 整文件 + elf_load_at）
elf_load_result_t ld_lr = {0};
if (is_dynamic) {
    int ld_fd = sys_open((int64_t)interp_path, O_RDONLY, 0, 0, 0, 0);
    if (ld_fd < 0) return -ENOENT;
    // 读 ld.so 整文件进 kmalloc
    uint8_t *ld_buf = ...;  // 同主 ELF 路径
    ld_lr = elf_load_at(ld_buf, ld_size, new_pml4, LD_SO_BASE);
    kfree(ld_buf);
    sys_close((int64_t)ld_fd, 0, 0, 0, 0, 0);
}
```

**ld.so 基址选择：**

`0x7FFFFF000000`（栈下方，固定，与栈顶 `0x7FFFFFFFE000` 相距 ~16MB，ld.so <1MB 无冲突）。定义在 `arch/x64/memlayout.h`：

```c
#define LD_SO_BASE   0x7FFFFF000000ULL   // ld.so 固定基址
```

**防呆校验：**

- `PT_INTERP` 字符串长度 < `PATH_MAX`，以 `\0` 结尾
- ld.so ELF magic/class 合法
- ld.so `PT_LOAD` 段（`base + p_vaddr`）不与主 ELF、用户栈地址重叠

#### 3.1.3 argc/argv/envp/auxv 栈构建

**栈布局（标准 SysV ABI，从低地址到高地址）：**

```
低地址
[rsp]            argc          (8 字节)
[rsp+8]          argv[0]       (8 字节 指针)
                 argv[1]
                 ...
                 argv[argc-1]
                 NULL          (8 字节，argv 终止符)
                 envp[0]       (8 字节 指针)
                 envp[1]
                 ...
                 envp[envc-1]
                 NULL          (8 字节，envp 终止符)
                 auxv[0].type  (8 字节)
                 auxv[0].value (8 字节)
                 ...
                 auxv[K].type = AT_NULL    (8 字节)
                 auxv[K].value = 0         (8 字节)
                 (字符串数据: argv/envp/EXECFN 指向这里, NUL 结尾)
                 (16 字节 AT_RANDOM 数据)
高地址
```

**实现步骤：**

**关键路径约束：** `sys_execve` 在切换 CR3 前已用 `map_user_page_direct` 把栈页映射进 `new_pml4`（proc.c:789-797）。argc/argv/envp/auxv 必须在**切换 CR3 前**通过这些页的**内核虚拟地址**（`phys_to_virt(page_phys)`，与现有 `elf_load` 写段内容同路径）写入。若切换 CR3 后再写用户虚拟地址，则需跨地址空间 `copy_to_user`（旧 argv/envp 源在老地址空间，更复杂）。

```c
// kernel/bsd/proc.c: sys_execve 中 build_user_stack 增量

// 现有：user_stack_page = bfc_alloc_page(user_stack_pages);
//        stack_base = 0x7FFFFFFFE000 - user_stack_pages*PAGE_SIZE;
//        map_user_page_direct(new_pml4, stack_base + i*PAGE_SIZE, ...)
// 新增：拿到栈顶页的内核虚拟地址，往下写
uint64_t stack_top = 0x00007FFFFFFFE000ULL;  // 与 proc.c:831 一致
uint64_t stack_top_phys = user_stack_phys + (user_stack_pages - 1) * PAGE_SIZE;
char *sp = (char *)phys_to_virt(stack_top_phys) + PAGE_SIZE;  // 栈顶内核虚拟地址

// 1. 先从老用户空间收集 argv/envp（两级 copy_from_user）
//    argv 是 char** 用户指针，逐个 copy_from_user 读 char*，再 strncpy_from_user 读字符串
char *argv_strings[ARG_MAX]; int argc = 0;
while (argc < ARG_MAX) {
    char *p; if (copy_from_user(&p, argv + argc, sizeof(p))) return -EFAULT;
    if (!p) break;
    if (strncpy_from_user(argv_strings[argc], p, PATH_MAX) < 0) return -EFAULT;
    argc++;
}
// envp 同理

// 2. 从栈顶往下写字符串（高地址→低地址）
sp -= 16; memcpy(sp, random_bytes, 16); void *at_random = stack_top - 16;
sp -= execfn_len + 1; strcpy(sp, pathname); void *at_execfn = stack_top - 16 - execfn_len - 1;
// envp/argv 字符串...
// 记录每个字符串的用户态地址（stack_top - offset）

// 3. 对齐到 16 字节，写 auxv/argv/envp 指针数组/argc（内核虚拟地址写，值为用户态地址）
// auxv 用主 ELF 的 main_lr.phdr_vaddr/phnum/phent + ld_lr.entry/ld_lr.load_base
// 最终 tf->rsp = 用户态 sp 值
```

**关键点：**

- 字符串放最上层，指针数组指向字符串
- `AT_RANDOM` 先填固定值（16 字节全 0），未来 ASLR 时再真随机
- `AT_EXECFN` 指向 execve path 字符串副本
- argv/envp 从老用户栈两级 `copy_from_user`（指针数组 + 每个字符串）
- 写入路径：内核虚拟地址（`phys_to_virt`）→ 切换 CR3 → 用户态可见

**防呆校验：**

- argc/envc 上限（≥0，≤`ARG_MAX` = 128KB 总字符串）
- argv/envp 每个字符串长度 ≤ `PATH_MAX`
- 栈大小不超过已分配栈页（2048 页 = 8MB）
- `copy_from_user`/`strncpy_from_user` 失败返回 `-EFAULT`

#### 3.1.4 静态/动态分流入口

```c
// sys_execve 末尾
if (is_dynamic) {
    task->thread.rip = ld_info.entry;   // ld.so entry
    task->thread.rsp = (uintptr_t)sp;   // 指向 argc
    // 内核侧可观测
    printk("exec: ld.so @ 0x%lx, entry 0x%lx, main @ 0x%lx\n",
           ld_info.load_base, ld_info.entry, main_info.entry);
} else {
    task->thread.rip = main_info.entry; // 主 ELF entry，现状不变
    task->thread.rsp = (uintptr_t)sp;
}
```

### 3.2 ld.so 自举重定位（bootstrap）

#### 3.2.1 设计目标

ld.so 用 `gcc -shared -fPIC` 构建，自身是共享 ELF，有自己的 `.rela.dyn`。entry 不能直接跑依赖 GOT/全局指针的代码——GOT 未重定位，间接调用即崩。必须先用 bootstrap 函数，纯算术自重定位，然后才能正常跑。

#### 3.2.2 bootstrap 阶段约束清单

**禁用项（在 self-relocate 完成前）：**

| 禁用 | 原因 | 替代手段 |
|------|------|---------|
| 全局变量访问（`.bss`/`.data` 中已初始化的指针） | 未重定位，读到相对地址 | 用局部变量或立即数 |
| 函数调用（经 GOT/PLT） | GOT 未重定位 | 只调本地 `static` 函数（直接 `call`，RIP-relative） |
| `printf`/`malloc`/libc | 依赖 GOT 且 libc 还没加载 | `dl_puts` 直接 `SYS_write` |
| `thread_local`/`__thread` | TLS 没设（FS_BASE 未配） | 全局变量（重定位后可用） |
| 数组/结构体初始化（含指针字段） | `.data` 未重定位 | 运行时逐字段赋值 |

**允许（比初版宽松，实际验证）：**

- 局部变量（栈上，FS_BASE 暂未设但栈通过 RSP 访问）
- 立即数运算
- `syscall` 指令（直接 `asm volatile("syscall"...)`）
- 通过 `AT_BASE`（ld.so 自己的基址）+ 段内偏移计算地址
- **字符串字面量（`const char*`）**：x86-64 用 RIP-relative `lea` 取址，是位置无关的，**不依赖 GOT/重定位**，bootstrap 早期可用（`dl_puts("...")` 直接能用）。`.rodata` 的 RELATIVE 重定位只影响 `.rodata` 内部的指针数据，不影响取字符串字面量地址本身
- **`_DYNAMIC` 链接器符号**：RIP-relative 取址，bootstrap 早期可用

#### 3.2.3 _start → __dls3 → self-relocate

```c
// user/ldso/start.S（汇编入口，避免任何 C 运行时依赖）

.section .text
.hidden __dls3          // 阶段 2a 实测：避免 call 走 PLT（bootstrap 前 GOT 未填）
.global _start
_start:
    // rsp 指向 argc，不动栈
    // 跳 C 函数 __dls3，传 rsp 作为参数
    mov %rsp, %rdi
    call __dls3
    // __dls3 不返回，跳主 ELF entry
    // 若返回则挂掉
1:  hlt
    jmp 1b
```

```c
// user/ldso/dls3.c

// bootstrap 阶段：不能依赖 GOT/全局指针/字符串字面量
// 用 __attribute__((no_pic)) 避免 RIP-relative 访问 GOT
// 或用纯算术 + AT_BASE 计算地址

extern Elf64_Dyn _DYNAMIC[];  // 链接器提供的 .dynamic 起始符号

// 阶段 2a 实测：用 asm 取 _DYNAMIC 运行时地址，避免 C 代码经 GOT 读取
// （gcc -shared -fPIC 下 extern 数组取址走 GOT，bootstrap 前 GOT 未填）
static Elf64_Dyn *get_dynamic(void) {
    Elf64_Dyn *p;
    __asm__("leaq _DYNAMIC(%%rip), %0" : "=r"(p));
    return p;
}

// bootstrap 入口
void __dls3(uintptr_t *sp) {
    // 1. 从 auxv 取 AT_BASE（自己的基址）
    //    sp 指向 argc，跳过 argc/argv/envp 找 auxv
    uintptr_t ld_base = find_auxv(sp, AT_BASE);

    // 2. 定位 .dynamic（用 asm RIP-relative 取运行时地址，不走 GOT）
    Elf64_Dyn *dyn = get_dynamic();

    // 3. **glibc/musl bootstrap 顺序**：
    //    先遍历 .dynamic raw（d_ptr 是 link-time 相对 vaddr，不被 RELATIVE 修复），
    //    找 DT_RELA/DT_RELASZ/DT_RELAENT；
    //    再应用 R_X86_64_RELATIVE；
    //    最后读 .dynamic 其他项（DT_STRTAB/DT_SYMTAB 等 d_ptr 同样是相对 vaddr，
    //    用时 + ld_base）。
    //    关键：DT_RELA 的 d_ptr 不依赖 RELATIVE 重定位（链接器直接写相对偏移），
    //    先读后重定位是正确的；若颠倒顺序，RELATIVE 可能改写 d_ptr 导致语义依赖工具链。
    Elf64_Rela *rela = NULL;
    size_t rela_sz = 0;
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_RELA:  rela = (Elf64_Rela *)(ld_base + d->d_un.d_ptr); break;
        case DT_RELASZ: rela_sz = d->d_un.d_val; break;
        }
    }

    // 4. 遍历 .rela.dyn 应用 R_X86_64_RELATIVE
    //    *addr = ld_base + addend
    for (size_t i = 0; i < rela_sz / sizeof(Elf64_Rela); i++) {
        Elf64_Rela *r = &rela[i];
        if (ELF64_R_TYPE(r->r_info) == R_X86_64_RELATIVE) {
            uintptr_t *addr = (uintptr_t *)(ld_base + r->r_offset);
            *addr = ld_base + r->r_addend;
        }
        // 其他类型暂不处理（bootstrap 只做 RELATIVE）
    }

    // 5. 自重定位完成，可正常用全局变量/GOT
    dl_puts("dl: self-relocate done");

    // 6. 进入 __dls_init（普通 C 代码，可用全局变量）
    __dls_init(sp, ld_base);
}
```

**关键技术点：**

- `_DYNAMIC` 是链接器符号，`gcc -shared` 导出。**阶段 2a 实测**：C 代码 `extern Elf64_Dyn _DYNAMIC[]` 取址会走 GOT（`mov .got(%rip), %rax`），bootstrap 前 GOT 未填会读到 0。改用 `asm("leaq _DYNAMIC(%rip), %0")` 直接 RIP-relative 取运行时地址，绕过 GOT。
- `AT_BASE` 是内核传来的 ld.so 基址，bootstrap 用它计算绝对地址。
- bootstrap **只处理 `R_X86_64_RELATIVE`**（自重定位），其他类型（GLOB_DAT/JUMP_SLOT）依赖符号查找，留给 `__dls_init` 处理。
- `find_auxv` 用纯算术遍历栈，不依赖任何未重定位数据。
- **阶段 2a 实测**：`start.S` 的 `call __dls3` 默认走 PLT（`call __dls3@plt`），PLT entry 跳 GOT 槽（bootstrap 前 GOT 未填）。给 `__dls3` 加 `.hidden` 标记，gcc 改用直接 `call`（RIP-relative），消除 PLT。同理 `dl_puts`/`dl_put_hex` 用 `__attribute__((visibility("hidden")))` 避免走 PLT。

#### 3.2.4 dl_puts 调试输出

```c
// user/ldso/dl_puts.c

#include <stddef.h>
#include <stdint.h>
#include "common/syscall_nums.h"
// 阶段 2a 实测：用 syscall_nums.h 而非 syscall.h
// syscall.h 拉 arch/x64/utils.h（含内核态 cli/sti/cr3 等），ld.so 用户态不需要
// 只需 SYS_WRITE/SYS_EXIT 常量

// bootstrap 阶段可用：纯 syscall，不依赖 GOT/printf
// fd=2 是 stderr，execve 后 fd 表保留 stdin/stdout/stderr
// hidden visibility：跨文件可见但不导出动态符号表，避免走 PLT（bootstrap 前 GOT 未填）
__attribute__((visibility("hidden")))
void dl_puts(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    // 直接 syscall，不走 PLT
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_WRITE), "D"(2), "S"(s), "d"(len)
                 : "rcx", "r11", "memory");
}

// dl_put_hex：打印寄存器值/地址，bootstrap 早期可用
// hexdigits 用局部数组（栈上），不依赖 .rodata 重定位
// （字符串字面量本身 RIP-relative 可用，但局部数组更直观）
__attribute__((visibility("hidden")))
void dl_put_hex(uint64_t val) {
    char buf[17];
    const char hexdigits[] = "0123456789abcdef";  // 局部数组，栈上
    for (int i = 15; i >= 0; i--) {
        buf[i] = hexdigits[val & 0xf];
        val >>= 4;
    }
    buf[16] = '\n';
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_WRITE), "D"(2), "S"(buf), "d"(17)
                 : "rcx", "r11", "memory");
}
```

**注意：** `hexdigits[]` 是局部数组，初始化在栈上（运行时逐字节写），不依赖 `.rodata` 重定位。`const char *hexdigits = "..."` 才会依赖 `.rodata`，bootstrap 早期禁用。

### 3.3 ld.so 主流程

#### 3.3.1 设计目标

自重定位完成后，ld.so 进入 `__dls_init`，可正常用全局变量/GOT。主流程：

1. 解析主 ELF `.dynamic`（via AT_PHDR）
2. 加载 libc.so（read + 匿名映射）
3. **重定位 libc.so**（其 `.rela.dyn` 含 RELATIVE/GLOB_DAT 等；其 `.rela.plt` JUMP_SLOT 解析为 libc.so 内部符号）
4. **重定位主 ELF**（其 `.rela.dyn` 含 `R_X86_64_GOTPCREL`/GLOB_DAT；其 `.rela.plt` JUMP_SLOT 如 `printf` 此时解析为 libc.so 符号——必须在 libc.so 加载+重定位之后）
5. eager binding（JUMP_SLOT 立即解析；步骤 3/4 已含各模块的 eager bind）
6. 构造 link_map 链表
7. 跳主 ELF entry

**顺序约束：** 主 ELF 的 JUMP_SLOT 依赖 libc.so 符号，故 libc.so 必须先加载+重定位。libc.so 自身的 JUMP_SLOT 多为内部互调（libc.so 不 DT_NEEDED 别的 .so），可在加载后立即重定位。

#### 3.3.2 解析主 ELF .dynamic

```c
// user/ldso/dls_init.c

void __dls_init(uintptr_t *sp, uintptr_t ld_base) {
    // 1. 从 auxv 取主 ELF 信息
    auxv_t *av = find_auxv_start(sp);
    uintptr_t phdr = get_auxv(av, AT_PHDR);
    size_t phent = get_auxv(av, AT_PHENT);
    size_t phnum = get_auxv(av, AT_PHNUM);
    uintptr_t entry = get_auxv(av, AT_ENTRY);

    // 2. 遍历 PHDR 找 PT_DYNAMIC
    Elf64_Dyn *main_dyn = NULL;
    for (size_t i = 0; i < phnum; i++) {
        Elf64_Phdr *p = (Elf64_Phdr *)(phdr + i * phent);
        if (p->p_type == PT_DYNAMIC) {
            main_dyn = (Elf64_Dyn *)p->p_vaddr;
            break;
        }
    }
    if (!main_dyn) {
        dl_puts("dl: FATAL: main ELF has no PT_DYNAMIC");
        sys_exit(1);
    }

    // 3. 解析 main_dyn，找 DT_NEEDED
    const char *needed[DT_NEEDED_MAX] = {0};
    int needed_cnt = 0;
    for (Elf64_Dyn *d = main_dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED) {
            // DT_NEEDED 的 d_val 是 .dynstr 的偏移
            needed[needed_cnt++] = main_dynstr + d->d_un.d_val;
        }
    }

    // ... 继续 §3.3.3 加载 libc.so
}
```

#### 3.3.3 加载 libc.so（read + 匿名映射）

```c
// user/ldso/load_so.c

// 加载 .so 到用户态内存
// 返回加载基址（同时也是 PT_LOAD 最高 vaddr 的最低地址）
static void *load_so(const char *path) {
    int fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) {
        dl_puts("dl: FATAL: cannot open ");
        dl_puts(path);
        sys_exit(1);
    }

    // 1. 读 ELF header
    Elf64_Ehdr ehdr;
    sys_read(fd, &ehdr, sizeof(ehdr));
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        dl_puts("dl: FATAL: bad ELF magic");
        sys_exit(1);
    }

    // 2. 读所有 PHDR
    Elf64_Phdr *phdrs = malloc(ehdr.e_phnum * ehdr.e_phentsize);
    sys_lseek(fd, ehdr.e_phoff, SEEK_SET);
    sys_read(fd, phdrs, ehdr.e_phnum * ehdr.e_phentsize);

    // 3. 计算总加载大小（min vaddr ~ max vaddr，页对齐）
    uintptr_t min_vaddr = UINTPTR_MAX, max_vaddr = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            min_vaddr = min(min_vaddr, phdrs[i].p_vaddr);
            max_vaddr = max(max_vaddr, phdrs[i].p_vaddr + phdrs[i].p_memsz);
        }
    }
    size_t load_sz = PAGE_ALIGN(max_vaddr) - PAGE_DOWN(min_vaddr);

    // 4. 匿名映射（MAP_PRIVATE | MAP_ANONYMOUS）
    void *base = sys_mmap(NULL, load_sz,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // 注：现有 mmap 支持匿名映射

    // 5. 逐 PT_LOAD 段：lseek + read + memcpy 到映射区
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        void *seg_dst = (char *)base + (phdrs[i].p_vaddr - min_vaddr);
        sys_lseek(fd, phdrs[i].p_offset, SEEK_SET);
        sys_read(fd, seg_dst, phdrs[i].p_filesz);
        // BSS 清零（memsz > filesz 部分）
        if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
            memset((char *)seg_dst + phdrs[i].p_filesz, 0,
                   phdrs[i].p_memsz - phdrs[i].p_filesz);
        }
    }

    // 6. 设置段权限（mprotect）
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        void *seg = (char *)base + PAGE_DOWN(phdrs[i].p_vaddr - min_vaddr);
        size_t sz = PAGE_ALIGN(phdrs[i].p_memsz);
        int prot = 0;
        if (phdrs[i].p_flags & PF_R) prot |= PROT_READ;
        if (phdrs[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (phdrs[i].p_flags & PF_X) prot |= PROT_EXEC;
        sys_mprotect(seg, sz, prot);
    }

    close(fd);
    free(phdrs);
    return base;
}
```

**防呆校验：**

- ELF magic 校验
- `e_phnum` 合理上限（≤ `PN_XNUM`）
- `PT_LOAD` 段 `p_filesz ≤ p_memsz`（否则 ELF 损坏）
- `min_vaddr < max_vaddr`
- mmap 不返回 `MAP_FAILED`

**注：** 不做页共享（每个 .so 整文件读进内存再拷贝），与主 ELF 现状一致（ld.md #4）。未来做文件映射 mmap 时切换为纯 ld.so 内部实现，不影响内核/ld.so 的 ABI。

#### 3.3.4 重定位类型支持（标准 PIC 全集 9 种）

`gcc -shared -fPIC` 在 x86-64 产出的 `.so` 包含的重定位类型：

| 类型 | 值 | 用途 | 处理方式 |
|------|----|----|---------|
| `R_X86_64_RELATIVE` | 8 | 数据段绝对地址（基址 + addend） | `*addr = base + addend` |
| `R_X86_64_64` | 1 | 绝对地址 64 位（S + A） | `*addr = sym + addend` |
| `R_X86_64_PC32` | 2 | RIP-relative 32 位（S + A - P） | `*addr = sym + addend - addr` |
| `R_X86_64_PLT32` | 4 | PLT 调用（L + A - P，等价 PC32） | `*addr = sym + addend - addr` |
| `R_X86_64_GOTPCREL` | 9 | GOT 间接（G + GOT + A - P） | `*addr = got_entry_addr` |
| `R_X86_64_GLOB_DAT` | 6 | GOT 全局数据（S） | `*addr = sym` |
| `R_X86_64_JUMP_SLOT` | 7 | PLT 跳转（S，eager） | `*addr = sym` |
| `R_X86_64_32` | 10 | 绝对地址 32 位零扩展（S + A） | `*addr = (uint32_t)(sym + addend)` |
| `R_X86_64_32S` | 11 | 绝对地址 32 位符号扩展（S + A） | `*addr = (int32_t)(sym + addend)` |

**实现：**

```c
// user/ldso/relocate.c

static void apply_relocation(Elf64_Rela *r, void *base,
                             struct link_map *lmap) {
    uint32_t type = ELF64_R_TYPE(r->r_info);
    uint32_t sym_idx = ELF64_R_SYM(r->r_info);
    uintptr_t *addr = (uintptr_t *)((char *)base + r->r_offset);
    int64_t addend = r->r_addend;

    switch (type) {
    case R_X86_64_RELATIVE:
        *addr = (uintptr_t)base + addend;
        break;

    case R_X86_64_64: {
        void *sym = lookup_symbol(sym_idx, lmap);
        if (!sym) goto unresolved;
        *addr = (uintptr_t)sym + addend;
        break;
    }

    case R_X86_64_PC32:
    case R_X86_64_PLT32: {
        void *sym = lookup_symbol(sym_idx, lmap);
        if (!sym) goto unresolved;
        *(uint32_t *)addr = (uint32_t)((uintptr_t)sym + addend - (uintptr_t)addr);
        break;
    }

    case R_X86_64_GOTPCREL: {
        // 正确语义（两步）：
        // (1) 定位 GOT entry：r_offset 指向指令中的 4 字节 disp 字段，
        //     GOT entry 地址由 .got 段布局决定（link_map 记录 .got 段位置，
        //     entry 与 sym_idx 一一对应，由链接器分配）。
        // (2) 填 GOT entry = sym（被间接访问的目标地址）。
        // (3) 写 disp = GOT_entry_addr + addend - addr（PC-relative 指向 GOT entry）。
        //     执行 mov foo@GOTPCREL(%rip), %rax 时：CPU 计算 rip+disp = GOT_entry_addr，
        //     从 GOT entry 取出 sym 地址。
        void *sym = lookup_symbol(sym_idx, lmap);
        if (!sym) goto unresolved;
        uintptr_t got_entry = find_or_get_got_entry(lmap, sym_idx);  // lmap->got + offset
        *(uintptr_t *)got_entry = (uintptr_t)sym;
        *(uint32_t *)addr = (uint32_t)(got_entry + addend - (uintptr_t)addr);
        break;
    }

    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT: {
        void *sym = lookup_symbol(sym_idx, lmap);
        if (!sym) goto unresolved;
        *addr = (uintptr_t)sym;
        break;
    }

    case R_X86_64_32: {
        void *sym = lookup_symbol(sym_idx, lmap);
        if (!sym) goto unresolved;
        *(uint32_t *)addr = (uint32_t)((uintptr_t)sym + addend);
        break;
    }

    case R_X86_64_32S: {
        void *sym = lookup_symbol(sym_idx, lmap);
        if (!sym) goto unresolved;
        *(int32_t *)addr = (int32_t)((uintptr_t)sym + addend);
        break;
    }

    default:
        dl_puts("dl: FATAL: unknown reloc type ");
        dl_put_hex(type);
        sys_exit(1);
    }
    return;

unresolved:
    dl_puts("dl: FATAL: unresolved symbol idx ");
    dl_put_hex(sym_idx);
    sys_exit(1);
}
```

**关键策略：**

- **未知类型 hard-fail**：直接报错 + `sys_exit(1)`，不静默跳过。掩盖真实链接错误比加载失败更难调。
- **未解析符号加载时立即报错**：eager binding 的优势（ld.md #10）。
- `R_X86_64_PLT32` 等价 `R_X86_64_PC32`（gcc 在 x86-64 把 PLT 调用编译成 PC32 + PLT32 标记，链接器合并处理）。

#### 3.3.5 符号查找（DT_GNU_HASH）

`gcc -shared` 默认产 `DT_GNU_HASH`（不产老式 `DT_HASH`，除非 `-Wl,--hash-style=sysv`）。本方案仅支持 GNU hash（本方案补充决策）。

**GNU hash 结构：**

```
.symtab      符号表
.dynsym      动态符号表（仅导出符号）
.gnu.hash    GNU hash 表
  ├─ header   (nbuckets, symoffset, bloom_size, bloom_shift)
  ├─ bloom    (bloom_size 个 uint64_t, 位过滤)
  ├─ buckets  (nbuckets 个 uint32_t, 链头索引)
  └─ chain    (符号 hash 低位 + 1 位结束标记)
```

**查找算法：**

```c
// user/ldso/symtab.c

// GNU hash 查找
// 返回符号地址，未找到返回 NULL
static void *gnu_hash_lookup(const char *name,
                              Elf64_Sym *symtab,
                              const char *strtab,
                              uint32_t *gnu_hash_table) {
    // 1. 计算 hash
    uint32_t h = gnu_hash(name);

    // 2. bloom filter 快速排除
    uint32_t bloom_size = gnu_hash_table[2];
    uint32_t bloom_shift = gnu_hash_table[3];
    uint64_t *bloom = (uint64_t *)&gnu_hash_table[4];
    uint64_t word = bloom[(h / 64) % bloom_size];
    uint64_t mask = (1ULL << (h % 64))
                  | (1ULL << ((h >> bloom_shift) % 64));
    if ((word & mask) != mask) return NULL;  // bloom 排除

    // 3. bucket 找链头
    uint32_t *buckets = (uint32_t *)((char *)bloom + bloom_size * 8);
    uint32_t symidx = buckets[h % gnu_hash_table[0]];
    if (symidx == 0) return NULL;  // 空桶

    // 4. chain 遍历
    uint32_t *chain = buckets + gnu_hash_table[0];
    uint32_t chain_h;
    do {
        chain_h = chain[symidx - symoffset];  // symoffset = gnu_hash_table[1]
        if ((h | 1) == (chain_h | 1)) {  // 忽略最低位（结束标记）
            Elf64_Sym *sym = &symtab[symidx];
            if (strcmp(strtab + sym->st_name, name) == 0) {
                return (void *)sym->st_value;  // 调用方加 base
            }
        }
        symidx++;
    } while ((chain_h & 1) == 0);  // 最低位 1 表示链结束

    return NULL;
}

// GNU hash 函数
static uint32_t gnu_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = (h << 5) + h + *s++;
    return h;
}
```

**符号查找顺序（ld.md #10）：**

```
主 ELF → libc.so（按 DT_NEEDED 出现顺序）
先找到的赢，主 ELF 优先（用户可覆盖 libc 行为）
```

```c
// user/ldso/symtab.c

// 遍历 link_map 查找符号
// lmap 链表顺序：主 ELF → libc.so → ...
void *lookup_symbol_in_link_map(const char *name, struct link_map *lmap) {
    for (struct link_map *l = lmap; l; l = l->l_next) {
        void *sym = gnu_hash_lookup(name, l->symtab, l->strtab, l->gnu_hash);
        if (sym) return (char *)l->base + (uintptr_t)sym;
    }
    return NULL;
}
```

#### 3.3.6 eager binding

```c
// user/ldso/relocate.c

// eager binding：遍历 .rela.plt，立即解析每个 JUMP_SLOT
static void eager_bind(struct link_map *l) {
    Elf64_Rela *plt_rela = l->rela_plt;
    size_t plt_sz = l->rela_plt_sz;
    int count = 0;

    for (size_t i = 0; i < plt_sz / sizeof(Elf64_Rela); i++) {
        Elf64_Rela *r = &plt_rela[i];
        if (ELF64_R_TYPE(r->r_info) != R_X86_64_JUMP_SLOT) continue;

        uint32_t sym_idx = ELF64_R_SYM(r->r_info);
        const char *name = l->strtab + l->symtab[sym_idx].st_name;
        void *sym = lookup_symbol_in_link_map(name, l->l_prev);  // 从前驱查
        // 注：libc.so 的 JUMP_SLOT 解析需在主 ELF + libc.so 都加载后做
        // 所以 eager binding 在所有 .so 加载完后统一做

        if (!sym) {
            dl_puts("dl: FATAL: unresolved symbol ");
            dl_puts(name);
            sys_exit(1);
        }

        uintptr_t *got_entry = (uintptr_t *)((char *)l->base + r->r_offset);
        *got_entry = (uintptr_t)sym;
        count++;
    }

    dl_puts("dl: relocated N symbols");  // N 替换为 count
}
```

**关键点：**

- eager binding 顺序：所有 .so 加载完 → 统一 eager bind（保证符号查找能跨模块）
- 不设 `.got.plt` trampoline（省掉 `_dl_runtime_resolve` 汇编，ld.md #10）
- 未解析符号加载时立即报错，调试更直接

#### 3.3.7 link_map 构造 + _dl_link_map

```c
// user/ldso/link_map.c

struct link_map {
    uintptr_t base;            // 加载基址
    Elf64_Dyn *dynamic;        // .dynamic 段地址
    struct link_map *l_next;   // 链表
    struct link_map *l_prev;
    // 符号查找辅助
    Elf64_Sym *symtab;
    const char *strtab;
    uint32_t *gnu_hash;
    Elf64_Rela *rela_dyn;
    size_t rela_dyn_sz;
    Elf64_Rela *rela_plt;
    size_t rela_plt_sz;
    // .got 段位置（GOTPCREL 重定位用，§3.3.4）
    void *got_addr;
    size_t got_size;
    // PT_TLS（供 __libc_start_main 合并）
    void *tls_template;
    size_t tls_tdata_size;
    size_t tls_tbss_size;
    size_t tls_align;
};

// 全局变量，__libc_start_main 读取
struct link_map *_dl_link_map = NULL;

// 构造 link_map 链表
void build_link_map(uintptr_t main_base, Elf64_Dyn *main_dyn,
                    uintptr_t libc_base, Elf64_Dyn *libc_dyn) {
    static struct link_map main_map, libc_map;  // 静态分配（ld.so 不用 malloc 太早）

    fill_link_map(&main_map, main_base, main_dyn);  // 解析 .dynamic 填 symtab/strtab/gnu_hash/rela_dyn/rela_plt/got_addr/PT_TLS
    fill_link_map(&libc_map, libc_base, libc_dyn);  // 同上

    main_map.l_next = &libc_map;
    libc_map.l_prev = &main_map;

    _dl_link_map = &main_map;
}

// __libc_start_main 通过 extern 读取
// extern struct link_map *_dl_link_map;  // 在 libc.so/libc.a 头文件
```

**关键点：**

- link_map 静态分配（ld.so 早期 malloc 可能未就绪）
- `_dl_link_map` 是全局变量，ld.so 导出，`__libc_start_main` 通过 `extern` 读取
- 链表顺序：主 ELF → libc.so（符号查找顺序匹配 ld.md #10）

#### 3.3.8 跳主 ELF entry

```c
// user/ldso/dls_init.c 末尾

void __dls_init(uintptr_t *sp, uintptr_t ld_base) {
    // ... 加载 libc.so、重定位、构造 link_map ...

    // 跳主 ELF entry
    uintptr_t entry = get_auxv(find_auxv_start(sp), AT_ENTRY);
    dl_puts("dl: jump to entry 0x...");
    dl_put_hex(entry);

    // 栈仍指向 argc（ld.so 没动栈）
    // 直接跳转，不动 rsp
    __asm__ volatile("mov %0, %%rsp\n"
                     "jmp *%1\n"
                     :
                     : "r"(sp), "r"(entry));
    // 不返回
}
```

### 3.4 libc.so 同源双产物

#### 3.4.1 设计目标

`user/lib/*` 一份源码，`libc` target 产 `.a`（`-fno-pie`，现状不动），`libc_so` target 用 `gcc -shared -fPIC` 产 `libc.so`。`-fPIC` 只加在 `.so` 构建上。

#### 3.4.2 构建系统改动

```cmake
# build_script/cmake/user_rules.cmake 增量

# 现有：libc.a
add_user_lib(c
    SOURCES ${LIBC_SOURCES}
    C
    FLAGS "-fno-pie -DDYNAMIC=0"
)
# libc.a 保持 -fno-pie（静态 ELF 不需要 PIC）

# 新增：libc.so
add_user_lib(c_so
    SOURCES ${LIBC_SOURCES}   # 同源
    C
    FLAGS "-fPIC -DDYNAMIC=1"
    SHARED
    OUTPUT_NAME libc
)
# gcc -shared -fPIC，导出全局符号
# -DDYNAMIC=1 切换 __libc_start_main 的 TLS 模板发现路径
```

**新增 `SHARED` 选项到 `add_user_lib`：**

```cmake
# user_rules.cmake: add_user_lib 增量

function(add_user_lib name)
    # ... 现有参数解析 ...

    set(option_args SHARED)
    set(multi_args SOURCES FLAGS)
    cmake_parse_arguments(ARG "${option_args}" "" "${multi_args}" ${ARGN})

    if (ARG_SHARED)
        # 共享库
        add_library(${name} SHARED ${ARG_SOURCES})
        set_target_properties(${name} PROPERTIES
            OUTPUT_NAME ${ARG_OUTPUT_NAME}
            POSITION_INDEPENDENT_CODE ON
        )
        target_compile_options(${name} PRIVATE ${ARG_FLAGS})
        target_link_options(${name} PRIVATE
            -shared
            -nostdlib -nodefaultlibs
            -Wl,--hash-style=gnu    # 强制 GNU hash（本方案补充）
            -Wl,-soname,libc.so
        )
        # 不链 crt0，libc.so 自身不需要 _start
    else()
        # 静态库（现有路径）
        add_library(${name} OBJECT ${ARG_SOURCES})
        # ... 现有 ...
    endif()
endfunction()
```

#### 3.4.3 符号导出策略

`gcc -shared` 默认导出所有全局符号（不加 `-fvisibility=hidden`），`printf`/`malloc`/`open` 等自动进动态符号表（ld.md #5）。

**不引入 `-Bsymbolic`**（ld.md #23）：libc.so 内部 `printf` 调 `write` 走 PLT/GOT，ld.so 重定位时解析为内部地址。保持符号可覆盖语义（主 ELF 可 hook libc 行为，调试常用）。

#### 3.4.4 新增构建函数：`add_user_dyn_elf` + `add_user_ldso`

现有 `add_user_elf` 用 `ld -Ttext 0x400000` 直接链接（user_rules.cmake:104），**不走 gcc driver**，无法生成 `PT_INTERP`/`DT_NEEDED`。本方案新增两个专用函数：

```cmake
# build_script/cmake/user_rules.cmake 增量

# add_user_dyn_elf: 动态主 ELF，gcc driver 链接
#   gcc driver 自动生成 PT_INTERP（via --dynamic-linker）+ DT_NEEDED（via -lc）
function(add_user_dyn_elf name)
    cmake_parse_arguments(ARG "C" "" "SOURCES;LINK_LIBS" ${ARGN})
    set(ELF_FILE ${CMAKE_BINARY_DIR}/${name}.elf)
    set(COMPILE_CMD ${CMAKE_C_COMPILER})  # 动态 ELF 强制 C
    set(COMPILE_FLAGS ${USER_COMPILE_FLAGS} -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/user/include)

    # 编译每个源（-fno-pie，非 PIE）
    set(OBJ_FILES "")
    set(idx 0)
    foreach(src ${ARG_SOURCES})
        set(src_obj ${ELF_FILE}.${idx}.o)
        add_custom_command(OUTPUT ${src_obj}
            COMMAND ${COMPILE_CMD} ${COMPILE_FLAGS} -c ${src} -o ${src_obj}
            DEPENDS ${src})
        list(APPEND OBJ_FILES ${src_obj})
        math(EXPR idx "${idx} + 1")
    endforeach()

    # gcc driver 链接：crt0.o + 主 ELF obj + -lc（记 DT_NEEDED，不提取符号）
    set(LD_ARGS ${CMAKE_BINARY_DIR}/crt0.o ${OBJ_FILES})
    if(ARG_LINK_LIBS)
        foreach(lib ${ARG_LINK_LIBS})
            list(APPEND LD_ARGS -L${CMAKE_BINARY_DIR} -l${lib})
        endforeach()
    endif()
    # 阶段 2a 实测：-nostdlib + 无动态依赖时 ld 丢弃 --dynamic-linker 指定的 .interp 段
    # 生成空 stub 共享库 + --no-as-needed 强制动态链接，保留 PT_INTERP + PT_DYNAMIC
    set(STUB_SO ${CMAKE_BINARY_DIR}/libdyn_stub.so)
    add_custom_command(OUTPUT ${STUB_SO}
        COMMAND gcc -shared -fPIC -nostdlib -o ${STUB_SO} -x c /dev/null
        COMMENT "Generating dyn stub shared library")
    add_custom_command(OUTPUT ${ELF_FILE}
        COMMAND gcc -fno-pie -no-pie
                -Wl,--dynamic-linker,/lib/ld.so
                -Wl,--hash-style=gnu
                -Wl,--no-as-needed
                -nostdlib -nodefaultlibs
                -o ${ELF_FILE} ${LD_ARGS} -L${CMAKE_BINARY_DIR} -ldyn_stub
        DEPENDS ${OBJ_FILES} ${CMAKE_BINARY_DIR}/crt0.o ${ARG_LINK_LIBS} ${STUB_SO}
        COMMENT "Linking dynamic ${name}.elf")
    add_custom_target(${name}_dyn_elf ALL DEPENDS ${ELF_FILE})
endfunction()

# add_user_ldso: ld.so 专用（-shared -fPIC，自带 minilibc，不链 libc.a）
function(add_user_ldso name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    set(ELF_FILE ${CMAKE_BINARY_DIR}/${name}.elf)
    set(COMPILE_FLAGS -m64 -ffreestanding -nostdlib -fno-builtin
                      -fPIC -fno-stack-protector -mno-red-zone
                      -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/user/include)
    set(OBJ_FILES "")
    set(idx 0)
    foreach(src ${ARG_SOURCES})
        set(src_obj ${ELF_FILE}.${idx}.o)
        add_custom_command(OUTPUT ${src_obj}
            COMMAND gcc ${COMPILE_FLAGS} -c ${src} -o ${src_obj}
            DEPENDS ${src})
        list(APPEND OBJ_FILES ${src_obj})
        math(EXPR idx "${idx} + 1")
    endforeach()
    add_custom_command(OUTPUT ${ELF_FILE}
        COMMAND gcc -shared -fPIC -nostdlib -nodefaultlibs
                -Wl,-e,_start -Wl,--hash-style=gnu
                -o ${ELF_FILE} ${OBJ_FILES}
        DEPENDS ${OBJ_FILES}
        COMMENT "Linking ld.so (${name}.elf)")
    add_custom_target(${name}_elf ALL DEPENDS ${ELF_FILE})
endfunction()
```

**使用：**

```cmake
add_user_dyn_elf(hello_dyn C SOURCES hello.c LINK_LIBS c)  # hello_dyn
add_user_ldso(ldso SOURCES
    user/ldso/start.S user/ldso/dls3.c user/ldso/dls_init.c
    user/ldso/load_so.c user/ldso/relocate.c user/ldso/symtab.c
    user/ldso/link_map.c user/ldso/minilibc.c)             # ld.so
add_user_lib(c_so SHARED SOURCES ${LIBC_SOURCES} FLAGS "-fPIC -DDYNAMIC=1" OUTPUT_NAME libc)
```

**与现有 `add_user_lib`/`add_user_elf` 关系：** 静态主 ELF 仍用 `add_user_elf`（现状不动）；libc.a 仍用 `add_user_lib`（现状不动）；libc.so 用 `add_user_lib` 加 `SHARED` 选项（§3.4.2）；动态主 ELF 用 `add_user_dyn_elf`；ld.so 用 `add_user_ldso`。

### 3.5 __libc_start_main 统一启动

#### 3.5.1 设计目标

`_start` → `__libc_start_main(main, argc, argv, envp)` → TLS 初始化 + `.init_array` + `main` + `exit` + `.fini_array`。静态动态共用，差异只在 TLS 模板发现路径（`#if DYNAMIC`）。

#### 3.5.2 同源双产物实现

```c
// user/lib/start.cc

// 统一启动函数，同源双产物
// libc.a 编译 -DDYNAMIC=0
// libc.so 编译 -DDYNAMIC=1
int __libc_start_main(int (*main)(int, char**, char**),
                      int argc, char **argv,
                      void (*init)(void), void (*fini)(void),
                      void *rtld_fini, void *stack_end) {
    // 1. TLS 模板发现
    struct tls_info ti;
#if DYNAMIC
    // 动态：遍历 _dl_link_map 合并 PT_TLS
    ti = collect_tls_from_link_map(_dl_link_map);
#else
    // 静态：读链接器符号
    ti = collect_tls_from_linker_symbols();
#endif

    // 2. 分配主线程 TLS 块（variant II 布局）+ TCB
    //    TLS 块在低地址，TCB 在高地址端（fs_base 处）。
    //    TCB 结构同 thread.md #550：{ self, tid, clear_tid_addr, cancel_state, cancel_type, tsd[128] }
    //    tcb.self = &tcb（%fs:0 返回 TCB 地址，pthread_self 依赖此）
    //    主线程 tid = getpid()，clear_tid_addr = 0（主线程无 set_tid_address）
    void *tls_block = allocate_tls_block(&ti);
    void *fs_base = (char *)tls_block + ti.size;  // FS_BASE 指向 TLS 块末尾
    struct tcb *tcb = (struct tcb *)fs_base;
    tcb->self = tcb;
    tcb->tid = sys_getpid();
    tcb->clear_tid_addr = NULL;
    tcb->cancel_state = PTHREAD_CANCEL_ENABLE;
    tcb->cancel_type = PTHREAD_CANCEL_DEFERRED;

    // 3. 设 FS_BASE（内核写 xtask_t.fs_base，__trapret 加载 MSR）
    sys_arch_prctl(ARCH_SET_FS, (uintptr_t)fs_base);

    // 4. 跑 .init_array（C++ 全局构造器等）
    run_init_array(&ti);

    // 5. 注册 .fini_array 到 atexit（exit 时逆序跑）
    if (fini) atexit(fini);
    if (rtld_fini) atexit(rtld_fini);  // ld.so 的 fini（本方案 ld.so 不注册，传 NULL）

    // 6. 跑 main
    char **envp = argv + argc + 1;
    int ret = main(argc, argv, envp);

    // 7. exit → 跑 atexit handlers（含 .fini_array）→ SYS_exit_group
    exit(ret);
    // 不返回
}
```

**TCB 结构定义（user/include/pthread.h，与 thread.md #560 一致）：**

```c
struct tcb {
    struct tcb *self;          // %fs:0 返回此地址
    pid_t tid;                 // 线程 ID
    void *clear_tid_addr;      // set_tid_address 的地址（主线程 NULL）
    int cancel_state;          // PTHREAD_CANCEL_ENABLE
    int cancel_type;           // PTHREAD_CANCEL_DEFERRED
    void *tsd[128];            // TSD values (pthread_key)
};
```

#### 3.5.3 TLS 模板发现双源

```c
// user/lib/tls.cc

// 静态路径：读链接器符号（单对象）
static struct tls_info collect_tls_from_linker_symbols(void) {
    struct tls_info ti = {0};
    extern char __tls_template_start[], __tls_template_end[];
    extern char __tls_tdata_size[], __tls_tbss_size[];
    extern char __tls_align[];
    ti.tdata_template = __tls_template_start;
    ti.tdata_size = (size_t)__tls_tdata_size;
    ti.tbss_size = (size_t)__tls_tbss_size;
    ti.alignment = (size_t)__tls_align;
    ti.size = ti.tdata_size + ti.tbss_size;  // 已按对齐 padding（linker.ld 保证）
    return ti;
}

// 动态路径：遍历 _dl_link_map 合并 PT_TLS（不保留 per-object 偏移）
static struct tls_info collect_tls_from_link_map(struct link_map *lmap) {
    struct tls_info ti = {0};
    size_t offset = 0;
    // 第一遍：计算总 size、最大对齐
    for (struct link_map *l = lmap; l; l = l->l_next) {
        if (l->tls_tdata_size + l->tls_tbss_size == 0) continue;
        ti.alignment = max(ti.alignment, l->tls_align);
        offset += ALIGN_UP(l->tls_tdata_size + l->tls_tbss_size, l->tls_align);
    }
    ti.size = offset;
    // 第二遍：分配合并模板，按对齐拼接各对象 tdata（tbss 区域在 pthread_create 分配时清零）
    ti.tdata_template = malloc(ti.size);
    size_t off = 0;
    for (struct link_map *l = lmap; l; l = l->l_next) {
        if (l->tls_tdata_size + l->tls_tbss_size == 0) continue;
        memcpy((char *)ti.tdata_template + off, l->tls_template, l->tls_tdata_size);
        off += ALIGN_UP(l->tls_tdata_size + l->tls_tbss_size, l->tls_align);
    }
    return ti;
}
```

### 3.6 TLS 桥接层

#### 3.6.1 tls_info 结构定义

```c
// user/include/sys/tls.h

// 阶段一裁剪到最小：pthread_create 只需拷贝合并后的模板，
// 不需 per-object 偏移（num_objects/obj_offsets）。
// 未来 dlopen + __tls_get_addr（动态 TLS 访问）再加 per-object 信息。
struct tls_info {
    void *tdata_template;       // TLS 模板（合并后的 tdata 拷贝源）
    size_t tdata_size;          // tdata 总大小
    size_t tbss_size;           // tbss 总大小
    size_t alignment;           // 最大对齐
    size_t size;                // 总大小（tdata + tbss + padding，variant II 块大小）
};

// 全局单例，pthread_create 读取
extern struct tls_info __g_tls_info;
```

#### 3.6.2 静态/动态填充路径

```
静态程序：
  __libc_start_main (-DDYNAMIC=0)
    └─ collect_tls_from_linker_symbols()
         └─ 读 __tls_template_start/end 等链接器符号
         └─ 单对象，直接填 __g_tls_info

动态程序：
  __libc_start_main (-DDYNAMIC=1)
    └─ collect_tls_from_link_map(_dl_link_map)
         └─ 遍历主 ELF + libc.so 的 PT_TLS
         └─ 合并 tdata 模板（按对齐拼接）+ 计算总 size/tbss
         └─ 填 __g_tls_info（不保留 per-object 偏移）
```

#### 3.6.3 pthread_create 消费接口

```c
// user/lib/pthread/pthread_create.cc

// pthread_create 只读 __g_tls_info，不区分静态/动态
int pthread_create(pthread_t *tid, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
    struct tls_info *ti = &__g_tls_info;

    // 1. 分配新线程 TLS 块 + TCB（variant II，与主线程一致）
    size_t block_sz = ALIGN_UP(ti->size, ti->alignment);
    void *tls_block = malloc(block_sz + sizeof(struct tcb) + ti->alignment);
    void *fs_base = (char *)tls_block + block_sz;
    struct tcb *tcb = (struct tcb *)fs_base;
    tcb->self = tcb;                  // %fs:0 返回 TCB
    tcb->tid = 0;                     // clone 返回后内核填
    tcb->clear_tid_addr = clear_tid;  // CLONE_CHILD_CLEARTID 用
    tcb->cancel_state = PTHREAD_CANCEL_ENABLE;
    tcb->cancel_type = PTHREAD_CANCEL_DEFERRED;

    // 2. 拷贝 tdata 模板，BSS 清零
    memcpy(tls_block, ti->tdata_template, ti->tdata_size);
    memset((char *)tls_block + ti->tdata_size, 0, ti->tbss_size);

    // 3. clone(CLONE_SETTLS | CLONE_CHILD_CLEARTID, ..., fs_base)
    return sys_clone(CLONE_VM | CLONE_FS | CLONE_THREAD | CLONE_SETTLS
                     | CLONE_CHILD_CLEARTID,
                     start, arg, tid, tcb->clear_tid_addr, fs_base);
}
```

**稳定接口：** `pthread_create` 只读 `tls_info`，未来加 dlopen 只是 `__g_tls_info` 重算，pthread 不用改（ld.md #13）。

### 3.7 统一 _start

#### 3.7.1 设计目标

静态/动态共用 `_start`：读栈 → `__libc_start_main`。静态 hello 顺便修掉拿不到 argc/argv 的既有缺陷（ld.md #11）。

**`_start` 归属：独立 `crt0.o`**（本方案补充，glibc/musl 模式）。`crt0.o` 链入每个主 ELF（静态/动态），`libc.a`/`libc.so` **不定义 `_start`**，仅提供 `__libc_start_main`。避免动态主 ELF 从 `libc.so` 取 `_start` 的歧义（`.so` 默认不导出 `_start` 给主 ELF）。

#### 3.7.2 实现

```c
// user/lib/crt0.S（汇编，避免任何 C 运行时依赖）

.section .text
.global _start
_start:
    // 栈布局：[rsp]=argc, [rsp+8]=argv[0], ..., argv[argc]=NULL, envp, ..., auxv
    // 调 __libc_start_main(main, argc, argv, envp, init, fini, rtld_fini, stack_end)
    //   main 由链接器符号提供（main ELF 的 main 函数）
    //   init/fini 由 __libc_init_array/__libc_fini_array 提供（crt0 可不传，由 __libc_start_main 内部跑 .init_array）
    mov %rsp, %rdi        // argc（直接传栈指针，__libc_start_main 内部读）
    lea 8(%rsp), %rsi     // argv
    // envp 在 argv 之后，__libc_start_main 内部算
    call __libc_start_main
1:  hlt
    jmp 1b
```

```c
// user/lib/start.cc（现有，将被 crt0.S 替代，保留过渡）
// 现有 _start 调 main(void) 不传 argc/argv —— 阶段 2b+3 后此文件删除，
// _start 移至 crt0.S，main 签名改为 int main(int, char**, char**)
```

**关键点：**

- `_start` 在 `crt0.o`，链接进每个主 ELF（静态/动态），与 libc 解耦
- `crt0.o` 不读 auxv（auxv 由 `__libc_start_main` 在动态路径读 `_dl_link_map`，不通过栈）
- 静态路径 argc/argv 修复：以前 `_start` 调 `main(void)`，现在 `crt0.o` → `__libc_start_main` → `main(int, char**, char**)`
- 动态路径：ld.so 跳到主 ELF `_start` 时栈仍指向 argc（ld.so 没动栈），逻辑一致

**`main` 签式迁移（本方案补充）：** 现有所有用户程序是 `int main(void)`（hello.c + 14 个 test_*.c + init.c + terminal.cc + test_runner.c）。统一 `_start` → `__libc_start_main` → `main(int argc, char** argv, char** envp)` 后，**所有用户程序的 `main` 签名需迁移**。x86-64 ABI 下调用方传 3 参、被调方不读不会崩，但 `main(void)` 拿不到 argc/argv。统一迁移到标准签名，未用参数忽略（`(void)argc;` 或直接不命名）。

## 4 接口设计

### 4.1 auxv 协议

**auxv 集合（ld.md #8）：**

| `AT_*` | 值 | 用途 | 必须性 |
|--------|----|------|--------|
| `AT_PHDR` | 3 | 主 ELF program header 表用户态地址 | 必须 |
| `AT_PHENT` | 4 | program header 条目大小 | 必须 |
| `AT_PHNUM` | 5 | program header 条目数 | 必须 |
| `AT_ENTRY` | 9 | 主 ELF 真实入口 | 必须 |
| `AT_BASE` | 7 | ld.so 加载基址 | 必须 |
| `AT_PAGESZ` | 6 | 4096 | 必须 |
| `AT_RANDOM` | 25 | 16 字节随机数据指针 | 建议加，先填固定值 |
| `AT_EXECFN` | 31 | 程序路径字符串指针 | 建议加，调试便利 |
| `AT_NULL` | 0 | 终止符 | 必须 |

不加 `AT_PLATFORM`，没有消费者（ld.md #8）。

**栈布局（标准 SysV ABI，从低地址到高地址）：**

```
低地址
[rsp]            argc            (8 字节)
[rsp+8]          argv[0]         (8 字节 指针)
                 argv[1]
                 ...
                 argv[argc-1]
                 NULL            (8 字节, argv 终止符)
                 envp[0]         (8 字节 指针)
                 envp[1]
                 ...
                 envp[envc-1]
                 NULL            (8 字节, envp 终止符)
                 auxv[0].type    (8 字节)
                 auxv[0].value   (8 字节)
                 ...
                 auxv[K].type = AT_NULL   (8 字节)
                 auxv[K].value = 0        (8 字节)
                 (字符串数据: argv/envp/EXECFN 指向这里, NUL 结尾)
                 (16 字节 AT_RANDOM 数据)
高地址
```

### 4.2 新增/改动 syscall

#### 4.2.1 sys_arch_prctl

```c
// kernel/bsd/syscall.c

// arch_prctl(ARCH_SET_FS, addr) / arch_prctl(ARCH_GET_FS, &addr)
long sys_arch_prctl(int code, uintptr_t addr) {
    xtask_t *t = current_task();
    switch (code) {
    case ARCH_SET_FS:
        t->fs_base = addr;
        // 立即生效（wrmsr 由 __trapret 在返回用户态时做）
        return 0;

    case ARCH_GET_FS: {
        // 读当前 FS_BASE
        uintptr_t fs = t->fs_base;
        return copy_to_user((void *)addr, &fs, sizeof(fs)) ? -EFAULT : 0;
    }

    default:
        return -EINVAL;
    }
}
```

**常量定义（`common/syscall.h`）：**

```c
#define ARCH_SET_FS  0x1002
#define ARCH_GET_FS  0x1003
```

#### 4.2.2 __trapret / syscall_fast_entry 写 FS_BASE MSR

**offset 导出策略（codebase 模式）：** `xtask_t.fs_base` 在结构体末尾（offset 很大，因 `recv_buf[16][64]` 等大数组在前）。现有汇编用硬编码数字 + C 侧 `_Static_assert` 校验（如 `switch_to` 用 `8(%rdi)`/`24(%rsi)`，`xtask.h` 已有 offset 0/4/8/16/24 的 `_Static_assert`）。本方案沿用此模式：

```c
// kernel/xcore/xtask.h 增量
// 编译后用 offsetof 确认具体值，填入 _Static_assert + 汇编常量
// （plan_thread_3.md:424 已标记此为"待确认"，本方案落实）
STATIC_ASSERT(offsetof(xtask_t, fs_base) == XTASK_FS_BASE_OFF,
              "fs_base offset drift");
// XTASK_FS_BASE_OFF 由编译后 offsetof 计算填入
```

```c
// arch/x64/trapentry.S: __trapret 增量

__trapret:
    # ... 现有：恢复通用寄存器 ...

    # 新增：写 FS_BASE MSR（从 xtask_t.fs_base）
    movq %gs:CPU_CURRENT, %rax       # 当前 xtask_t 指针
    movq $XTASK_FS_BASE_OFF, %rdx    # 硬编码 offset（_Static_assert 校验）
    movq (%rax,%rdx), %rdx           # rdx = current_task->fs_base
    movq $0xC0000100, %rcx           # MSR_FS_BASE
    movq %rdx, %rax
    shrq $32, %rdx
    wrmsr

    # ... 现有：iretq/sysretq ...
```

**关键点：**

- `xtask_t.fs_base` 字段已存在（xtask.h:66），`task_reap` 清零（sched.c:430），只是 `__trapret` 不写 MSR
- offset 用 `_Static_assert` + 硬编码数字（codebase 模式，与 `switch_to` 的 `8/24` 一致），不用符号常量避免链接器解析
- 上下文切换时统一写 FS_BASE，所有场景受益（pthread + ld.so + 静态 TLS）
- `syscall_fast_entry` 同理（SYSCALL 入口也要写 FS_BASE，防止首次 syscall 前 FS_BASE 未设）
- 性能：wrmsr 是 serializing 指令，开销可接受（每次返回用户态一次）

### 4.3 tls_info 结构

见 §3.6.1。

**ABI 稳定性：** `tls_info` 是 pthread 和 ld.so 的桥梁，结构稳定后不改（ld.md #13）。`pthread_create` 只读 `tls_info`，不区分静态/动态。

### 4.4 link_map 最小结构

见 §3.3.7。

**最小字段：** `base` / `dynamic` / `l_next` / `l_prev` + 符号查找辅助 + PT_TLS 信息。

`_dl_link_map` 全局变量由 ld.so 导出，`__libc_start_main` 通过 `extern` 读取。

### 4.5 ld.so 内部函数接口

| 函数 | 阶段 | 约束 |
|------|------|------|
| `_start` | bootstrap | 汇编入口，传 rsp 给 __dls3 |
| `__dls3(sp)` | bootstrap | 纯算术，不用 GOT/全局指针/字符串字面量 |
| `dl_puts(s)` | bootstrap+ | SYS_write 直写，可用 |
| `dl_put_hex(val)` | bootstrap+ | 局部数组转十六进制，可用 |
| `__dls_init(sp, ld_base)` | post-bootstrap | 可用全局变量/GOT |
| `load_so(path)` | post-bootstrap | 返回加载基址 |
| `apply_relocation(r, base, lmap)` | post-bootstrap | 9 种类型 + hard-fail |
| `gnu_hash_lookup(name, symtab, strtab, hash)` | post-bootstrap | 返回符号地址 |
| `eager_bind(lmap)` | post-bootstrap | 遍历 JUMP_SLOT |
| `build_link_map(...)` | post-bootstrap | 填 _dl_link_map |
| `lookup_symbol_in_link_map(name, lmap)` | post-bootstrap | 链表顺序查找 |

## 5 风险与稳定性方案

### 5.1 bootstrap 调试难点与对策

**难点：** bootstrap 阶段不能用 `printf`/`malloc`/经 GOT 的间接调用，错误即静默崩溃，难定位。

**对策：**

1. **dl_puts 早期可用**：`SYS_write` 直写 stderr，不依赖 GOT。字符串字面量在 x86-64 用 RIP-relative `lea` 取址，是位置无关的，**bootstrap 早期即可用**（不依赖 `.rodata` 的 RELATIVE 重定位——那只影响 `.rodata` 内部的指针数据，不影响取字符串地址本身）。
2. **dl_put_hex 早期可用**：局部数组转十六进制，不依赖 `.rodata`。用于打印寄存器值/地址。
3. **阶段 2a 独立验证**：插入 bootstrap-only 验证阶段（本方案补充），自重定位 + dl_puts + SYS_exit(0)，不加载 libc.so。缩小排障范围。
4. **GDB 远程调试**：`./run.sh -s` + gdb attach，在 `__dls3` 设断点，观察寄存器和内存。

**阶段 2a 验证代码：**

```c
// user/ldso/dls3.c: __dls3 末尾（阶段 2a）

void __dls3(uintptr_t *sp) {
    uintptr_t ld_base = find_auxv(sp, AT_BASE);
    // ... self-relocate R_X86_64_RELATIVE ...

    dl_puts("dl: self-relocate done");
    dl_puts("dl: ld_base = ");
    dl_put_hex(ld_base);

    // 阶段 2a：到此为止，验证自举成功
    sys_exit(0);
}
```

**阶段 2a 验收标准：**

- 串口看到 `dl: self-relocate done` + `dl: ld_base = 0x7FFFFF000000`
- 进程正常 exit（无崩溃）
- GDB 观察 `.data`/`.rodata` 全局变量值正确（被 RELATIVE 重定位）

### 5.2 ELF 畸形文件容错

**所有外部输入校验（防呆）：**

| 输入 | 校验 | 失败处理 |
|------|------|---------|
| 主 ELF ELF header | magic、`e_ident[EI_CLASS]` = 64、`e_type` = ET_EXEC/ET_DYN | -ENOEXEC |
| 主 ELF PHDR | `e_phnum` ≤ `PN_XNUM`、`p_offset + p_filesz ≤ 文件大小` | -ENOEXEC |
| `PT_INTERP` 字符串 | 长度 ≤ `PATH_MAX`、以 `\0` 结尾 | -ENOENT |
| ld.so 文件 | 存在、ELF magic 合法 | -ENOENT |
| ld.so `PT_LOAD` | 不与主 ELF 地址重叠 | -EINVAL |
| libc.so ELF header | magic、class、type | dl_puts + sys_exit(1) |
| libc.so `PT_LOAD` | `p_filesz ≤ p_memsz` | dl_puts + sys_exit(1) |
| `.dynamic` 表 | 遍历到 `DT_NULL` 终止 | dl_puts + sys_exit(1) |
| `DT_NEEDED` 字符串 | `.dynstr` 偏移在范围内 | dl_puts + sys_exit(1) |
| 重定位条目 | `r_offset` 在段范围内 | dl_puts + sys_exit(1) |
| 符号查找 | hash 表边界、`st_name` 在 `.dynstr` 范围内 | dl_puts + sys_exit(1) |
| auxv 栈 | 指针落在用户栈范围 | -EFAULT |

**原则：** 内核侧返回 `-errno`；ld.so 侧 `dl_puts` 报错 + `sys_exit(1)`。都不静默跳过。

### 5.2.1 构建系统风险：`add_library(SHARED)` + freestanding 工具链

**风险：** toolchain 设置 `BUILD_SHARED_LIBS OFF` + `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY`（toolchain-x86_64.cmake:9-10），偏向静态。`add_library(SHARED)` 虽能覆盖 `BUILD_SHARED_LIBS`，但 CMake 的 `SHARED` 链接阶段可能尝试链接默认 crt0/libc，与 `-nostdlib -nodefaultlibs` 冲突。CLAUDE.md 警告 `add_library(OBJECT)` 不能设 `POSITION_INDEPENDENT_CODE ON`（破坏 RIP-relative），`SHARED` 不同（`-fPIC` 正确），但需验证。

**对策：** 阶段 2b+3 早期先验证 `add_library(SHARED)` + `-fPIC` + `target_link_options(-shared -nostdlib -nodefaultlibs -Wl,--hash-style=gnu)` 能产出合法 `libc.so`（有 `.dynsym`/`.gnu.hash`/`PT_LOAD`/`PT_DYNAMIC`，无未定义符号依赖宿主 libc）。若 `add_library(SHARED)` 失败，回退到 `add_custom_command` 直接调 `gcc -shared`（与 `add_user_ldso` 同模式，§3.4.4）。

### 5.3 静态/动态共存双向验证

**回归验证矩阵：**

| 阶段 | 验证项 | 期望 |
|------|--------|-----|
| 阶段 1 完成 | 静态 hello 跑通 | 正常 |
| 阶段 1 完成 | 全部静态测试 ELF 跑通 | 正常 |
| 阶段 1 完成 | 静态 hello argc/argv | 仍不读栈（_start 未统一） |
| 阶段 2+3 完成 | 静态 hello argc/argv | 正确读取（_start 统一后） |
| 阶段 2a 完成 | ld.so bootstrap-only | `self-relocate done` + exit(0) |
| 阶段 2b+3 完成 | hello_dyn 跑通 | 调用 libc.so 符号 |
| 阶段 4 完成 | hello_dyn + 静态 hello 共存 | 都正常 |

### 5.4 重定位失败处理

- **未知重定位类型**：hard-fail，`dl_puts` 报错 + `sys_exit(1)`。不静默跳过（掩盖真实链接错误）。
- **未解析符号**：加载时立即报错（eager binding 优势），`dl_puts` 打印符号名 + `sys_exit(1)`。
- **GOT/PLT 越界写**：`r_offset` 校验在段范围内，越界报错。

### 5.5 调试可观测

**内核侧 + ld.so 侧双打印（ld.md #21）：**

**内核侧（execve）：**

```
exec: ld.so @ 0x7FFFFF000000, entry 0x7FFFFF001234, main @ 0x401000
```

**ld.so 侧（各阶段）：**

```
dl: self-relocate done
dl: ld_base = 0x7FFFFF000000
dl: loading libc.so
dl: loaded libc.so @ 0x7FFFFF100000
dl: relocated 234 symbols
dl: jump to entry 0x401000
```

**若 fd=2 不到串口：** 加 `SYS_DBGPRINT` 专用 syscall（内核直接 `serial_puts`），ld.so bootstrap 阶段可用。备选方案，先假设 fd=2 可用。

## 6 开发里程碑与迭代计划

### 6.1 阶段 0：共享前置（pthread 产物）

**目标：** FS_BASE/arch_prctl/tls_info 框架落地，pthread 和 ld.so 共同复用。

**任务：**

1. `__trapret` / `syscall_fast_entry` 写 FS_BASE MSR（`wrmsr 0xC0000100`），从 `xtask_t.fs_base` 加载
2. `sys_arch_prctl(ARCH_SET_FS/ARCH_GET_FS)` syscall
3. `tls_info` 结构定义 + `__libc_tls_init` 读链接器符号填 `tls_info`（单对象，静态路径）
4. clone + futex + exit_group + tgkill + ...
5. `pthread_create` 读 `tls_info` 分配 TLS + `clone(CLONE_SETTLS)`
6. pthread 库（mutex/cond/...）

**验证：**

- 静态程序用 `thread_local` 变量正确
- `pthread_create` 创建线程，TLS 变量隔离

**产物复用 ld.so：** FS_BASE wrmsr ✓ / sys_arch_prctl ✓ / tls_info 框架 ✓

### 6.2 阶段 1：内核 execve 改造

**目标：** 内核支持动态 ELF 加载（auxv/PT_INTERP/ld.so），静态路径不破坏。

**任务：**

1. `sys_execve` 解析 `PT_INTERP`
2. 加载 ld.so（复用 `PT_LOAD` 路径，固定高位基址 `0x7FFFFF000000`）
3. 建 argc/argv/envp/auxv 栈（标准 SysV ABI）
4. 静态/动态分流入口（有 `PT_INTERP` → ld.so entry；无 → 主 ELF entry）
5. 内核侧打印 `exec: ld.so @ 0x..., entry 0x..., main @ 0x...`

**验证：**

- 静态 hello + 全部静态测试 ELF 仍正常
- 加载一个最小的 ld.so（只 SYS_exit(0)）能跑通
- auxv 改动不破坏静态路径

### 6.3 阶段 2a：ld.so bootstrap-only 验证（插入）

**目标：** 验证 ld.so 自举重定位正确，缩小排障范围。

**任务：**

1. `user/ldso/start.S`：`_start` → `__dls3`
2. `user/ldso/dls3.c`：`__dls3` 实现
   - 从 auxv 取 `AT_BASE`
   - 遍历自身 `.rela.dyn` 应用 `R_X86_64_RELATIVE`
   - `dl_puts("dl: self-relocate done")`
   - `dl_put_hex(ld_base)`
   - `sys_exit(0)`
3. 构建产物 `/lib/ld.so`，打包进 `disk.img`

**验证：**

- 串口看到 `dl: self-relocate done` + `dl: ld_base = 0x7FFFFF000000`
- 进程正常 exit（无崩溃）
- GDB 观察 `.data`/`.rodata` 全局变量值正确

**阶段 2a 实施偏离（实测修正，回写 §3.2/§3.4.4）：**

1. **`start.S` 加 `.hidden __dls3`**：`gcc -shared` 默认把 `call __dls3` 编成 `call __dls3@plt`，PLT entry 跳 GOT 槽，bootstrap 前 GOT 未填导致跳 0x1016 page fault。加 `.hidden` 后 gcc 改用直接 `call`（RIP-relative），消除 PLT。
2. **`dl_puts`/`dl_put_hex` 加 `visibility("hidden")`**：同上，避免 `__dls3` 调它们走 PLT。不能用 `static`（跨文件），`hidden` 是"文件内可见但不导出动态符号表"。
3. **`dls3.c` 用 `asm("leaq _DYNAMIC(%rip)")` 取 `.dynamic` 地址**：C 代码 `extern Elf64_Dyn _DYNAMIC[]` + `(char *)_DYNAMIC` 在 `-fPIC` 下走 GOT（`mov .got(%rip), %rax`），bootstrap 前 GOT 未填读到 0。改 asm 直接 RIP-relative 取运行时地址。
4. **`dl_puts.c`/`dls3.c` include `common/syscall_nums.h` 而非 `common/syscall.h`**：`syscall.h` 拉 `arch/x64/utils.h`（含内核态 cli/sti/cr3/load_cr3 等），ld.so 用户态不需要；只需 `SYS_WRITE`/`SYS_EXIT` 常量。
5. **`add_user_dyn_elf` 加 stub 共享库**：`-nostdlib -nodefaultlibs` + 无动态依赖时，即使传 `-Wl,--dynamic-linker,/lib/ld.so`，ld 也丢弃 `.interp` 段（不生成 PT_INTERP）。生成空 `libdyn_stub.so` + `-Wl,--no-as-needed -ldyn_stub` 强制动态链接，保留 PT_INTERP + PT_DYNAMIC。
6. **内核 `elf_load_internal` 非页对齐段映射 bug 修复**（plan_ld1 遗留）：原 `page_off = (page_addr - base) - p_vaddr` 对 `p_vaddr` 非页对齐的段（如 ld.so 的 `.dynamic`/`.data` 段 `p_vaddr=0x3f20`）算出负值（uint64 下溢），导致 `copy_len=0`，段所在页映射为全 0，bootstrap 读 `.dynamic` 全 0 找不到 DT_RELA。改为按 `p_offset & ~0xFFF` 页对齐基准 + 整页拷贝（页内段前部分属于其他段或文件头，整页拷贝保留正确内容）。此修复属于 plan_ld1 内核 execve 范围，但阶段 2a 首次触发（静态 ELF 段都页对齐，未暴露）。

**实测结果：**

```
exec: ld.so @ 0x7fffff000000, entry 0x7fffff001000, main @ 0x401000
dl: self-relocate done
dl: ld_base = 00007fffff000000
dl: self_ptr = 00007fffff00123f   ← 高位 0x7FFFFF 证明 RELATIVE 重定位正确
[PASS] hello_dyn (exit 0)
Summary: PASS=18 FAIL=0 SKIP=0   ← 静态 hello + 17 测试全回归通过
```

### 6.4 阶段 2b+3：ld.so + libc.so + __libc_start_main 完整链路

**目标：** 一次跑通完整动态启动链（ld.md #18 合并，本方案拆为 2a + 2b+3）。

**任务：**

1. `user_rules.cmake` 加 `libc_so` target（`gcc -shared -fPIC`，同源 `user/lib/*`，`SHARED` 选项）
2. `user_rules.cmake` 新增 `add_user_dyn_elf` + `add_user_ldso`（§3.4.4）
3. **crt0.o**（新增，§3.7）：`user/lib/crt0.S`，`_start` → `__libc_start_main`，独立产物
4. **linker.ld 加 `.init_array`/`.fini_array` 段**（新增）：定义 `__init_array_start/end` + `__fini_array_start/end` 符号
5. **libc 实现 `atexit`**（新增）：最多 32 个 handler 的静态数组（无 malloc 依赖）
6. **所有用户程序 `main` 签名迁移**（新增，§3.7）：`int main(void)` → `int main(int argc, char** argv, char** envp)`。涉及 hello.c + 14 个 test_*.c + init.c + terminal.cc + test_runner.c
7. `__libc_start_main` 实现（同源，`libc.a -DDYNAMIC=0` + `libc.so -DDYNAMIC=1`）
   - 读 `tls_info`（静态走链接器符号 / 动态走 `_dl_link_map` 合并 PT_TLS）
   - 分配主线程 TLS 块 + **TCB**（§3.5.2，`tcb.self=&tcb`）
   - `arch_prctl(ARCH_SET_FS)`
   - 跑 `.init_array`
   - 注册 `.fini_array` 到 `atexit`
   - `main(argc,argv,envp)` → `exit`
8. ld.so 完整实现（自带最小 libc，不链 libc.a，偏离 ld.md #2）
   - `_start` → self-relocate（阶段 2a 已验证）
   - 解析主 ELF `.dynamic`（via AT_PHDR）
   - `DT_NEEDED`：加载 libc.so（read + anon mmap + memcpy）
   - **重定位 libc.so**（其 `.rela.dyn`/`.rela.plt`）
   - **重定位主 ELF**（其 `.rela.dyn` 含 GOTPCREL/`.rela.plt` JUMP_SLOT 解析 libc.so 符号）
   - eager 重定位（标准 PIC 全集 9 种，§3.3.4，GOTPCREL 正确两步处理）
   - 构造 link_map，存全局 `_dl_link_map`
   - 跳 AT_ENTRY（主 ELF `_start`，来自 crt0.o）

**验证：**

- 静态 hello 仍正常（统一 `_start` + main 签名迁移不破坏静态，argc/argv 正确）
- 动态 hello（最小测试）能跑通完整链路（先不要求 libc.so 符号全部解析对）

### 6.5 阶段 4：hello_dyn 动态验证

**目标：** hello_dyn 跑通，调用 libc.so 符号。

**任务：**

1. `hello_dyn` 用 gcc 驱动链接（非 PIE @ `0x400000`，`DT_NEEDED libc.so`）
2. 打包 libc.so + ld.so 到 `disk.img /lib/`
3. 运行验证

**构建命令（ld.md #19）：**

```bash
gcc -fno-pie -no-pie -Wl,--dynamic-linker,/lib/ld.so \
    -Wl,--hash-style=gnu \
    -nostdlib -nodefaultlibs \
    -o hello_dyn hello.c -L build -lc
```

**验证：**

- `hello_dyn` 跑通，调用 `printf`（libc.so 符号）正确
- 串口看到完整 ld.so 日志链
- 静态 hello 仍正常

### 6.6 阶段 5：测试 ELF 全改动态，迁移其余用户程序

**目标：** 测试 ELF 全改动态，init/shell/kbd_driver/terminal 留静态（ld.md #7）。

**任务：**

1. 测试 ELF 全改动态链接
2. 验证全部测试通过
3. 评估迁移其余用户程序（init/shell 暂留静态）

### 6.7 依赖关系图

```
[当前] pthread Phase 2-4（阶段 0）
  ├─ FS_BASE wrmsr in __trapret + syscall_fast_entry
  ├─ sys_arch_prctl(ARCH_SET_FS/GET_FS)
  ├─ TLS: __libc_tls_init 读链接器符号填 tls_info（单对象）
  ├─ clone + futex + exit_group + tgkill + ...
  ├─ pthread_create 读 tls_info 分配 TLS + clone(CLONE_SETTLS)
  └─ pthread 库（mutex/cond/...）
       │
       ▼
[阶段 0 产物] FS_BASE/arch_prctl/tls_info 框架 ✓
       │
       ▼
[阶段 1] 内核 execve 改造
  ├─ 解析 PT_INTERP
  ├─ 加载 ld.so（固定高位基址）
  ├─ 建 argc/argv/envp/auxv 栈
  ├─ rip = ld.so entry（有 PT_INTERP）/ 主 ELF entry（无）
  └─ 验证：静态 hello/测试仍正常
       │
       ▼
[阶段 2a] ld.so bootstrap-only 验证（插入）
  ├─ _start → __dls3 纯算术自举
  ├─ R_X86_64_RELATIVE 自重定位
  ├─ dl_puts + dl_put_hex
  └─ sys_exit(0)  ← 不加载 libc.so，仅验证自举
       │
       ▼
[阶段 2b+3] ld.so + libc.so + __libc_start_main 完整链路
  ├─ user_rules.cmake 加 libc.so 产物
  ├─ __libc_start_main 同源双产物
  ├─ 统一 _start（静态动态共用）
  ├─ ld.so 完整实现（加载 libc.so + 重定位 + link_map）
  └─ 验证：静态 hello 仍正常
       │
       ▼
[阶段 4] 动态 ELF 验证
  ├─ hello_dyn（gcc 驱动链接，非 PIE @ 0x400000）
  ├─ 打包 libc.so + ld.so 到 disk.img /lib/
  └─ 验证：hello_dyn 跑通（调用 libc.so 符号）
       │
       ▼
[阶段 5] 测试 ELF 全改动态，迁移其余用户程序
```

**依赖点确认：**

- 阶段 1 的 auxv 改动对静态程序无感——静态 `_start` 不读栈，栈顶多了数据当没看见，等阶段 2+3 统一 `_start` 后才真正读栈。✓
- 阶段 0 的 FS_BASE/arch_prctl/tls_info 是 pthread 和 ld.so 的共同地基，先做 pthread 时落地，ld.so 直接复用。✓
- 阶段 2a 在阶段 2b+3 之前，验证自举正确后再接完整链路，缩小排障范围。✓

## 7 软硬件环境与工具链

### 7.1 构建工具链

**关键偏离 ld.md #2：** ld.so **不链 libc.a**，自带最小 libc（`memcpy`/`memset`/`strcmp`/`malloc` via `mmap`）。原因：ld.so 是 `-shared -fPIC`，libc.a 是 `-fno-pie`，链接产生 text relocation；自带最小 libc 与 musl/glibc ld.so 一致，且使 ld.so 完全自包含（bootstrap 阶段无 libc GOT 依赖）。ld.md #2 需同步更新此偏离。

**libc.so 构建（ld.md #5，新增 `add_user_lib(SHARED)`）：**

```bash
gcc -shared -fPIC -DDYNAMIC=1 \
    -nostdlib -nodefaultlibs \
    -Wl,--hash-style=gnu \
    -Wl,-soname,libc.so \
    -o build/libc.so \
    user/lib/*.c
# 不含 _start（crt0.o 提供），仅导出 __libc_start_main/printf/malloc/...
```

**libc.a 构建（现状不动）：**

```bash
gcc -fno-pie -DDYNAMIC=0 \
    -nostdlib -nodefaultlibs \
    -c user/lib/*.c -o build/libc.a
# 不含 _start（crt0.o 提供）
```

**crt0.o 构建（新增，_start 归属）：**

```bash
gcc -ffreestanding -fno-pie -c user/lib/crt0.S -o build/crt0.o
# _start 在此，静态/动态主 ELF 都链 crt0.o
# libc.a/libc.so 不定义 _start
```

**ld.so 构建（新增 `add_user_ldso`，偏离 ld.md #2）：**

```bash
gcc -shared -fPIC -nostdlib -nodefaultlibs \
    -Wl,-e,_start -Wl,--hash-style=gnu \
    -o build/ld.so \
    user/ldso/*.c user/ldso/*.S user/ldso/minilibc.c
# 不链 libc.a，自带最小 libc
# -shared -fPIC 产 .so（有 .rela.dyn 需自重定位）
# -e _start 设入口
# 无 PT_INTERP（自己是 interpreter）
# 导出 _dl_link_map 供 libc.so 的 __libc_start_main 解析
```

**静态主 ELF 链接（现状，扩展现有 ld 命令加 crt0.o）：**

```bash
ld -m elf_x86_64 -Ttext 0x400000 \
    build/crt0.o hello.o build/libc.a -o hello.elf
# _start 来自 crt0.o，调 libc.a 的 __libc_start_main
```

**动态主 ELF 链接（新增 `add_user_dyn_elf`，gcc driver，ld.md #19）：**

```bash
gcc -fno-pie -no-pie \
    -Wl,--dynamic-linker,/lib/ld.so \
    -Wl,--hash-style=gnu \
    -nostdlib -nodefaultlibs \
    -o hello_dyn build/crt0.o hello.c \
    -L build -lc
# gcc driver 自动生成 PT_INTERP + DT_NEEDED libc.so
# crt0.o 提供 _start，调 libc.so 的 __libc_start_main
# -lc 让 ld 记 DT_NEEDED libc.so，但不提取符号（printf 引用 undefined，运行时 ld.so 解析）
```

**关键参数：**

- `-fno-pie -no-pie`：非 PIE 固定基址 @ `0x400000`
- `-Wl,--dynamic-linker,/lib/ld.so`：指定动态链接器（gcc driver 生成 `PT_INTERP`）
- `-Wl,--hash-style=gnu`：强制 GNU hash（本方案补充）
- `-nostdlib -nodefaultlibs`：不链宿主机 crt0/libc
- ld.so 的 `-shared -fPIC`：产共享 ELF，需 `elf_load_at` base-offset 加载 + bootstrap 自重定位

### 7.2 镜像打包

**`disk.img` 内容（ld.md #6）：**

```
/lib/ld.so        ← ld.so（自带最小 libc，-shared -fPIC）
/lib/libc.so      ← libc.so（动态）
/bin/hello        ← 静态 hello（现状）
/bin/hello_dyn    ← 动态 hello
/bin/test_*       ← 测试 ELF（阶段 5 改动态）
/init             ← init（静态，现状）
/bin/shell        ← shell（静态）
```

**`libc.a` 不入镜像**：静态 ELF 已把 libc 代码链进自己，`libc.a` 仅构建时用。

### 7.3 调试环境

**QEMU + GDB + tmux + 串口日志：**

```bash
# 1. 启动 QEMU + GDB server
rm -f log.txt
tmux new-session -d -s qemu './run.sh -s 2>&1'
tmux new-session -d -s serial 'socat -,rawer UNIX-CONNECT:/tmp/qemu-serial.sock'
tmux new-session -d -s gdb 'gdb -ex "target remote localhost:1234" build/myos.elf'

# 2. 在 ld.so 关键点设断点
tmux send-keys -t gdb 'break __dls3' Enter
tmux send-keys -t gdb 'break __dls_init' Enter
tmux send-keys -t gdb 'break __libc_start_main' Enter
tmux send-keys -t gdb 'continue' Enter

# 3. 观察串口日志
tail -f log.txt

# 4. 用户态地址解析
addr2line -e build/ld.so -f -C 0x7FFFFF001234
addr2line -e build/libc.so -f -C 0x7FFFFF100567
addr2line -e build/hello_dyn -f -C 0x401000

# 5. 清理
tmux kill-session -t gdb; tmux kill-session -t serial; tmux kill-session -t qemu
```

### 7.4 bootstrap 阶段调试技巧

**bootstrap 阶段无符号打印，靠寄存器观察：**

```bash
# 在 __dls3 入口设断点，观察 rsp（指向 argc）
tmux send-keys -t gdb 'break __dls3' Enter
tmux send-keys -t gdb 'continue' Enter
tmux send-keys -t gdb 'info registers rsp rdi' Enter

# 单步执行观察 RELATIVE 重定位
tmux send-keys -t gdb 'next' Enter
tmux send-keys -t gdb 'x/gx 0x7FFFFF001234' Enter  # 检查重定位后的值

# 查看 auxv
tmux send-keys -t gdb 'x/20gx $rdi' Enter  # rsp 指向 argc，后面是 argv/envp/auxv
```

**阶段 2a 缩小排障范围：**

- 自重定位错误 → 阶段 2a 就崩（不涉及 libc.so）
- 阶段 2a 通过但阶段 2b+3 崩 → 问题在 libc.so 加载/重定位/link_map
- 阶段 2b+3 通过但 hello_dyn 崩 → 问题在主 ELF 链接或 `__libc_start_main`

**串口日志预期（阶段 4 完成后跑 hello_dyn）：**

```
exec: ld.so @ 0x7FFFFF000000, entry 0x7FFFFF001234, main @ 0x401000
dl: self-relocate done
dl: ld_base = 0x7FFFFF000000
dl: loading libc.so
dl: loaded libc.so @ 0x7FFFFF100000
dl: relocated 234 symbols
dl: jump to entry 0x401000
hello, world
```
