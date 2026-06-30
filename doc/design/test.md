# 测试框架

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 测试框架 | Unity + UNITY_EXCLUDE_SETJMP | 最成熟嵌入式框架，单文件集成，setjmp/longjmp 完成后移除排除 |
| 2 | 组织方式 | 每子系统一个测试 ELF | 单进程崩溃不影响其他模块 |
| 3 | 结果输出 | 串口 log（逐 case 详情）+ 退出码（0=全PASS, 1=有FAIL） | 和现有 malloctest 一致 |
| 4 | 测试运行 | test_runner.elf 依次 spawn + waitpid | 实现简单，支持全跑和单模块跑 |
| 5 | 构建集成 | build.sh --test 启用 -DTEST=1 | 条件编译，不影响正式构建 |
| 6 | 自动执行 | init 进程在全部服务就绪后 spawn test_runner.elf | TEST 宏控制 |
| 7 | 磁盘部署 | FAT32 /test/ 目录 | mkdisk.sh 加 mcopy |
| 8 | 多进程测试 | spawn 自己（子进程根据 mmap marker 走不同逻辑） | 不需额外 ELF，不需 IPC 协议约定 |
| 9 | 退出码 | _exit(0)=全PASS, _exit(1)=有FAIL | Unity UnityEnd() 根据 FailCount 决定 |

### 构建集成

build.sh --test 传 -DTEST=1 给 CMake → 顶层 CMakeLists.txt if(TEST) add_subdirectory(user/test) → init/init.c #ifdef TEST spawn test_runner.elf。

Unity 库：user/lib/unity/ — unity.c + unity.h + unity_internals.h，CMake target unity（STATIC），编译时定义 UNITY_EXCLUDE_SETJMP。

测试 ELF 声明：add_user_elf(name C SOURCES test_name.c LINK_LIBS c unity)。

mkdisk.sh TEST 构建时额外 mcopy 测试 ELF 到 /test/。

### 测试 ELF

| 子系统 | ELF 名 | 覆盖的 syscall |
|--------|--------|---------------|
| unistd | pipe.elf | getpid, yield, pipe, write, read, close |
| fcntl | fcntl.elf | install_fd, dup2, fcntl, lseek |
| string | string.elf | 纯 libc，无 syscall |
| malloc | malloc.elf | mmap, munmap |
| stdio | stdio.elf | write/read |
| mmap | mmap.elf | mmap, munmap, memfd_create, ftruncate |
| IPC | ipc.elf | memfd_create, ftruncate, SCM_RIGHTS, notify, req, resp, msg, msg_resp |
| socket | socket.elf | socket, bind, listen, accept, connect, socketpair, sendmsg, recvmsg, shutdown |
| process | process.elf | fork, execve, waitpid, exit |
| signal | signal.elf | kill, sigaction, sigreturn |
| poll | poll.elf | poll |
| PCI | pci.elf | pci_dev_info, ioperm, block_async |

### test_runner

test_runner.elf — 测试管理进程：
- 全跑模式：/test/test_runner.elf → 依次 spawn 所有测试 ELF，收集 waitpid 退出码，输出汇总
- 单模块模式：/test/test_runner.elf pipe → 只跑指定模块
- 输出格式：[PASS] name.elf (exit 0) / [FAIL] name.elf (exit 1)，最后 Summary 行

test_runner 需从 FAT32 加载 ELF → open + read + sys_spawn。

### 测试优先级（依赖关系）

1. unistd（pipe/read/write/close） ← 基础
2. fcntl（open/close/dup2/fcntl/lseek）
3. string/stdlib/stdio ← C 库
4. mmap ← malloc 底层
5. IPC ← 共享内存
6. socket ← IPC
7. process ← 进程管理
8. signal ← 信号
9. poll ← Wayland 前置
10. PCI ← 硬件

### 关键源码位置

- 测试 ELF：user/test/ 目录各 test_*.c
- test_runner：user/test/test_runner.c
- Unity：user/lib/unity/
- CMake 测试目标：user/test/CMakeLists.txt
- init TEST 条件编译：init/init.c #ifdef TEST

### 与其他模块的关系

| 模块 | 说明 |
|------|------|
| 构建系统 | build.sh --test + mkdisk.sh TEST 部署。详见 [cmake.md](cmake.md) |
| 启动流程 | init 进程拉起 test_runner。详见 [boot.md](boot.md) |
| libc | 测试 ELF 链接 libc.a + libunity.a。详见 [libc.md](libc.md) |
| syscall | 测试覆盖 59 个 syscall。详见 [syscall.md](syscall.md) |

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| setjmp/longjmp | x86-64 约 50 行汇编，解锁 Unity 测试隔离（FAIL 后继续执行），完成后移除 UNITY_EXCLUDE_SETJMP | 高 |
| 现有临时测试迁移 | malloctest.c/writetest.c/test_pcie.c 迁移到 Unity 框架，删除原文件 | 中 |
| pthread 测试 | clone/futex/arch_prctl 等 syscall 实现后新增 Layer 11 | 低 |
| gtest 迁移 | 需 C++ stdlib + exceptions + pthread，断言宏换名 | 低 |
| Linux ABI 兼容 | 对齐 syscall 编号和 struct 后可直接用 Linux 交叉编译测试 | 低 |
