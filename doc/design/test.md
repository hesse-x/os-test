# 测试方案设计

## 概述

自上而下 POSIX 功能验证，采用 Unity 测试框架，按子系统分 ELF 组织，串口 log 输出结果。覆盖 50 个 syscall（编号 0-49）中的 39 个通用 syscall 及 libc 用户态库。通过 `build.sh --test` 启用测试构建，init 进程在拉起全部服务后自动运行 test_runner 执行全部测例。

## 决策记录

| 决策 | 选择 | 理由 |
|------|------|------|
| 测试框架 | Unity + `UNITY_EXCLUDE_SETJMP` | 最成熟的嵌入式框架，单文件集成，后续补 setjmp/longjmp 可解锁测试隔离 |
| 组织方式 | 每个 POSIX 子系统一个测试 ELF | 单进程崩溃不影响其他模块，按子系统独立运行和定位问题 |
| 结果输出 | 串口 log（Unity 打印逐 case 详情） + 退出码（0=全PASS, 1=有FAIL） | 和现有 malloctest 模式一致，test_runner 读退出码汇总，开发者看串口 log 定位 |
| 测试运行 | test_runner.elf 依次 spawn + waitpid | 已有 spawn/waitpid，实现简单，支持全跑和单模块跑 |
| 构建集成 | `build.sh --test` 启用 `-DTEST`；Unity 作为独立 libunity.a，LINK_LIBS c unity | 条件编译，不影响正式构建；不污染 libc.a，add_user_elf 已支持 LINK_LIBS |
| 自动执行 | init 进程在全部服务就绪后 spawn test_runner.elf | TEST 宏控制，仅在测试构建时生效，init 拉起全部进程后最后拉起 test |
| 磁盘部署 | FAT32 /test/ 目录 | mkdisk.sh 加 mcopy 到 /test/，不和正式程序混淆 |
| 多进程测试 | spawn 自己（子进程根据 marker 走不同逻辑） | 不需要额外 ELF，不需要 IPC 协议约定 |
| 后续迁移 | Unity → gtest（需 C++ stdlib + exceptions + pthread） | 断言语义相近，测试逻辑不变，只换宏名 |

## 构建集成

### TEST 宏与 build.sh --test

`build.sh` 增加 `--test` 选项，传递 `-DTEST=1` 给 CMake：

```bash
# build.sh 增加参数解析
--test)
    CMAKE_EXTRA="-DTEST=1"
    shift
    ;;
```

CMake 侧根据 `TEST` 变量条件启用测试目标：

```cmake
# 顶层 CMakeLists.txt
if(TEST)
    add_subdirectory(user/test)
endif()
```

init 进程中通过 `#ifdef TEST` 条件编译，在全部服务拉起后自动 spawn test_runner：

```c
// init/init.c
#ifdef TEST
    // 全部服务就绪后，拉起 test_runner 执行全部测例
    spawn_service("/test/test_runner.elf");
#endif
```

mkdisk.sh 在 TEST 构建时额外拷贝测试 ELF 到 `/test/` 目录。

### Unity 库

- 文件：`user/lib/unity/unity.c` + `unity.h` + `unity_internals.h`
- CMake target：`unity`（STATIC library）
- 编译 flags：同 `add_user_lib` 规则（`-m64 -ffreestanding -nostdlib -fno-builtin ...`）
- 编译时定义：`UNITY_EXCLUDE_SETJMP`（setjmp/longjmp 完成后移除）

### 测试 ELF 命名约定

| 子系统 | 测试源文件 | ELF 名 | LINK_LIBS | 现有临时测试 |
|--------|-----------|--------|-----------|------------|
| unistd (pipe/read/write/close) | `user/test/test_pipe.c` | `pipe.elf` | c unity | — |
| string | `user/test/test_string.c` | `string.elf` | c unity | — |
| stdlib (malloc/free/calloc/realloc) | `user/test/test_malloc.c` | `malloc.elf` | c unity | `user/malloctest.c`（printf 验证） |
| stdio (printf/fprintf/fputc/fputs/puts/fflush/fgetc/getchar) | `user/test/test_stdio.c` | `stdio.elf` | c unity | — |
| fcntl (open/close/dup2/fcntl/lseek) | `user/test/test_fcntl.c` | `fcntl.elf` | c unity | `user/writetest.c`（write+read+strcmp 验证） |
| sys/mman (mmap/munmap/memfd_create/ftruncate) | `user/test/test_mmap.c` | `mmap.elf` | c unity | — |
| sys/socket (AF_UNIX + SCM_RIGHTS) | `user/test/test_socket.c` | `socket.elf` | c unity | — |
| signal (kill/sigaction/sigreturn) | `user/test/test_signal.c` | `signal.elf` | c unity | — |
| poll | `user/test/test_poll.c` | `poll.elf` | c unity | — |
| IPC (shm_create/shm_attach/notify/req/resp/msg/msg_resp) | `user/test/test_ipc.c` | `ipc.elf` | c unity | — |
| process (spawn/waitpid/exit) | `user/test/test_process.c` | `process.elf` | c unity | — |
| PCI/device (pci_dev_info/open_dev/ioperm/block_io) | `user/test/test_pci.c` | `pci.elf` | c unity | `user/test_pcie.c`（枚举扫描） |

现有临时测试程序为 printf 手工验证，无断言框架，无退出码约定。迁移到 Unity 后统一为 `user/test/` 目录下的规范测试 ELF。

### CMake 声明示例

```cmake
# user/test/CMakeLists.txt
add_user_elf(pipe C SOURCES test_pipe.c LINK_LIBS c unity)
add_user_elf(string C SOURCES test_string.c LINK_LIBS c unity)
add_user_elf(malloc C SOURCES test_malloc.c LINK_LIBS c unity)
add_user_elf(stdio C SOURCES test_stdio.c LINK_LIBS c unity)
add_user_elf(fcntl C SOURCES test_fcntl.c LINK_LIBS c unity)
add_user_elf(mmap C SOURCES test_mmap.c LINK_LIBS c unity)
add_user_elf(socket C SOURCES test_socket.c LINK_LIBS c unity)
add_user_elf(signal C SOURCES test_signal.c LINK_LIBS c unity)
add_user_elf(poll C SOURCES test_poll.c LINK_LIBS c unity)
add_user_elf(ipc C SOURCES test_ipc.c LINK_LIBS c unity)
add_user_elf(process C SOURCES test_process.c LINK_LIBS c unity)
add_user_elf(pci C SOURCES test_pci.c LINK_LIBS c unity)
add_user_elf(test_runner C SOURCES test_runner.c LINK_LIBS c)
```

### mkdisk.sh 部署

在 mkdisk.sh 中增加测试 ELF 拷贝（TEST 构建时生效）：

```bash
if [ "$TEST" = "1" ]; then
    mmd -i $DISK_IMG ::/test
    for elf in build/pipe.elf build/string.elf build/malloc.elf build/stdio.elf \
               build/fcntl.elf build/mmap.elf build/socket.elf build/signal.elf \
               build/poll.elf build/ipc.elf build/process.elf build/pci.elf \
               build/test_runner.elf; do
        mcopy -i $DISK_IMG $elf ::/test/
    done
fi
```

## 磁盘部署

测试 ELF 统一存放在 FAT32 `/test/` 目录：

```
/test/
  test_runner.elf
  pipe.elf
  string.elf
  malloc.elf
  stdio.elf
  fcntl.elf
  mmap.elf
  socket.elf
  signal.elf
  poll.elf
  ipc.elf
  process.elf
  pci.elf
```

shell 中执行：`/test/pipe.elf` 或 `/test/test_runner.elf`（全跑回归）

## test_runner 设计

test_runner.elf 是测试管理进程，功能：

- **全跑模式**：`/test/test_runner.elf` → 依次 spawn 所有测试 ELF，收集 waitpid 退出码，输出汇总
- **单模块模式**：`/test/test_runner.elf pipe` → 只跑指定模块
- **输出格式**：

```
=== Test Runner ===
[RUN]  pipe.elf          ... running
[PASS] pipe.elf          (exit 0)
[RUN]  string.elf        ... running
[FAIL] string.elf        (exit 1) — check serial log for details
...
=== Summary: PASS=8 FAIL=2 ===
```

内部实现：

```c
// 硬编码测试列表
struct test_entry {
    const char *name;    // "pipe", "string", ...
    const char *path;    // "/test/pipe.elf"
};

static struct test_entry tests[] = {
    {"pipe",    "/test/pipe.elf"},
    {"fcntl",   "/test/fcntl.elf"},
    {"string",  "/test/string.elf"},
    {"malloc",  "/test/malloc.elf"},
    {"stdio",   "/test/stdio.elf"},
    {"mmap",    "/test/mmap.elf"},
    {"ipc",     "/test/ipc.elf"},
    {"socket",  "/test/socket.elf"},
    {"process", "/test/process.elf"},
    {"signal",  "/test/signal.elf"},
    {"poll",    "/test/poll.elf"},
    {"pci",     "/test/pci.elf"},
};

// 主逻辑：spawn → waitpid → 记录退出码 → 汇总打印
```

注意：test_runner 需要 spawn ELF，需通过 fs_driver 从 FAT32 加载 ELF 文件到内存，再调 sys_spawn。这要求 shell 的路径执行功能已可用（spawn 从文件路径加载 ELF）。

## 退出码约定

- `_exit(0)`：所有 case PASS
- `_exit(1)`：有 case FAIL（Unity 的 `UnityEnd()` 根据 FailCount 决定退出码）

Unity 的 `UnityFailBit` 在任何 TEST_ASSERT 失败时置位。`UnityEnd()` 检查此位决定退出码。

无 setjmp/longjmp 时：第一个 FAIL 直接 abort（`_exit(1)`），后续 case 不执行，串口 log 只记录第一个失败。
有 setjmp/longjmp 后：FAIL 后 longjmp 恢复继续执行，所有 case 都跑完，`UnityEnd()` 输出完整报告后 `_exit(0)` 或 `_exit(1)`。

## 多进程测试设计

部分测试（pipe 阻塞语义、spawn inherit）需要子进程协作。采用 **spawn 自己** 模式：

```c
// test_pipe.c 示例
void test_pipe_write_blocks_when_full(void) {
    // 在 mmap 共享区域写 marker，标记自己是测试主进程
    volatile int *role_marker = (int *)mmap(...);
    *role_marker = 0;  // 主进程角色

    int fd[2];
    pipe(fd);

    // spawn 自己（子进程继承 pipe fd 和 mmap 区域）
    // 子进程看到 role_marker==0，设为 1（helper 角色）
    // 子进程：从 fd[0] 读数据（释放 pipe 空间），主进程写入不再阻塞
    // 主进程：写满 pipe → 验证写阻塞 → 子进程读后写不再阻塞

    if (/* 我是 helper 子进程 */) {
        // helper 逻辑
        usleep(100000);  // 等主进程写满
        read(fd[0], buf, 4096);  // 释放 pipe 空间
        _exit(0);
    } else {
        // 主进程测试逻辑
        write(fd[1], big_buf, 4096);  // 写满
        // 验证后续 write 阻塞或返回正确状态
    }
}
```

子进程通过共享 mmap 区域中的 marker 区分角色，不需要额外 ELF 或 IPC 协议约定。

## 测试优先级（按依赖关系从底向上）

```
Layer 1: unistd (pipe/read/write/close)     ← 所有测试的基础
Layer 2: fcntl (open/close/dup2/fcntl/lseek) ← 文件 I/O 基础
Layer 3: string/stdlib/stdio                ← C 库正确性
Layer 4: sys/mman (mmap/munmap/memfd_create/ftruncate) ← malloc 底层
Layer 5: IPC (shm_create/shm_attach/notify/req/resp/msg/msg_resp) ← 共享内存 IPC
Layer 6: sys/socket (AF_UNIX + SCM_RIGHTS)  ← socket IPC
Layer 7: process (spawn/waitpid/exit)       ← 进程管理
Layer 8: signal (kill/sigaction/sigreturn)  ← 信号
Layer 9: poll                               ← Wayland 前置
Layer 10: PCI/device (pci_dev_info/open_dev/ioperm/block_io) ← 硬件驱动接口
```

## Syscall 覆盖范围

当前 50 个 syscall（编号 0-49），按测试子系统分组：

| 测试子系统 | 覆盖的 syscall | 备注 |
|-----------|---------------|------|
| unistd | getpid(0), yield(1), pipe(14), write(15), read(16), close(17) | — |
| fcntl | install_fd(33), dup2(25), fcntl(26), lseek(44) | open 基于 install_fd |
| stdio | 同 fcntl + write/read | — |
| string | 无 syscall（纯 libc 函数） | — |
| stdlib (malloc) | mmap(9), munmap(10) | — |
| mmap | mmap(9), munmap(10), memfd_create(45), ftruncate(46) | — |
| IPC | shm_create(12), shm_attach(13), notify(19), req(3), resp(4), msg(22), msg_resp(23) | — |
| socket | socket(34), bind(35), listen(36), accept(37), connect(38), socketpair(39), sendmsg(40), recvmsg(41), shutdown(42) | — |
| process | spawn(8), waitpid(7), exit(6) | execv/exit_group 未实现 |
| signal | kill(47), sigaction(48), sigreturn(49) | tgkill 未实现 |
| poll | poll(43) | — |
| PCI/device | pci_dev_info(29), open_dev(32), ioperm(24), block_io(30), block_async(31) | — |

**未纳入通用测试的 syscall**（驱动专用或内部使用）：

| syscall | 编号 | 用途 |
|---------|------|------|
| recv | 2 | 内核消息接收，驱动进程内部使用 |
| req | 3 | 同步请求，IPC 测试间接覆盖 |
| resp | 4 | 同步回复，IPC 测试间接覆盖 |
| irq_bind | 5 | IRQ 绑定，驱动进程使用 |
| load_dev | 18 | 设备加载，init 使用 |
| fb_info | 11 | framebuffer 信息，KMS 驱动使用 |
| dma_alloc | 27 | DMA 内存分配，AHCI 驱动使用 |
| dma_free | 28 | DMA 内存释放，AHCI 驱动使用 |
| gettime | 20 | 纳秒时间，libc timespec_get 间接使用 |
| clock | 21 | CPU 时间，libc clock 间接使用 |

**尚未实现的 syscall**（不纳入当前测试，待实现后追加）：

| syscall | 计划编号 | 依赖 |
|---------|---------|------|
| clone | — | 线程创建，需完整 pthread 支持 |
| futex | — | 线程同步原语 |
| arch_prctl | — | 线程本地存储 |
| set_tid_address | — | 线程 ID 管理 |
| gettid | — | 线程 ID 查询 |
| tgkill | — | 线程级信号发送 |
| execv | — | 进程映像替换 |
| exit_group | — | 进程组退出 |

## 执行顺序

```
1. 集成 Unity 到 CMake 构建（libunity.a）          → 3 个文件加入 user/lib/unity/
2. 实现 build.sh --test + CMake 条件编译            → TEST 宏控制测试目标
3. 修改 init 进程支持 TEST 条件 spawn test_runner    → 自动执行测例
4. 实现 test_runner.elf                            → spawn + waitpid 收集退出码
5. Layer 1 测试：test_pipe.c                       → 所有后续测试的基础
6. Layer 2-10 按依赖关系逐层推进
7. 迁移现有临时测试（malloctest/writetest/pcie）到 Unity 框架
8. 实现 setjmp/longjmp（x86-64，~50行汇编）         → 解锁 Unity 测试隔离
```

## Layer 1: test_pipe.c 详细 case 列表

| # | Case 名 | 测什么 | 需要多进程？ |
|---|---------|--------|------------|
| 1 | pipe_create | `pipe()` 返回两个 fd，fd[0] 有读标志 fd[1] 有写标志 | 否 |
| 2 | pipe_write_read | 写 "hello" → 读回来 → strcmp 验证内容一致 | 否 |
| 3 | pipe_write_read_partial | 写 10 字节 → 读 5 字节 → 再读 5 字节，验证部分读取 | 否 |
| 4 | pipe_full_block | 写满 4KB ring buffer → 验证后续 write 阻塞（需子进程消费释放空间） | 是 |
| 5 | pipe_empty_block | 读空 pipe → 验证 read 阻塞（需子进程写入唤醒） | 是 |
| 6 | pipe_close_read_eof | 关写端 → 读返回 0 (EOF) | 否 |
| 7 | pipe_close_write_epipe | 关读端 → 写返回 -EPIPE | 否 |
| 8 | pipe_nonblock_read | O_NONBLOCK 读空 pipe 返回 EAGAIN | 否 |
| 9 | pipe_nonblock_write | O_NONBLOCK 写满 pipe 返回 EAGAIN | 否 |
| 10 | pipe_refcount | close 一端 ref_count--，对端仍可用直到也 close | 否 |
| 11 | pipe_spawn_inherit | spawn 子进程，子进程继承 pipe fd，父子通过 pipe 通信 | 是 |
| 12 | pipe_big_transfer | 写 8KB 数据（超过 PIPE_BUF），验证分批传输和完整性 | 否 |
| 13 | pipe_multiple_write | 多次 write 小块数据，验证顺序和完整性 | 否 |
| 14 | pipe_fd_limit | 创建多对 pipe 验证 fd 分配不冲突（不超过 MAX_FD=32） | 否 |

## Layer 2: test_fcntl.c 详细 case 列表

| # | Case 名 | 测什么 | 需要多进程？ |
|---|---------|--------|------------|
| 1 | open_create_read | `open(O_WRONLY|O_CREAT)` 创建文件 → close → `open(O_RDONLY)` 读回 → strcmp 验证 | 否 |
| 2 | open_nonexist | `open(O_RDONLY)` 不存在的文件返回 -ENOENT | 否 |
| 3 | close_twice | 第二次 close 同一 fd 返回 -EBADF | 否 |
| 4 | dup2_basic | `dup2(old, new)` 复制 fd，新旧 fd 指同一 pipe/file | 否 |
| 5 | dup2_bad_fd | `dup2(-1, 3)` 返回 -EBADF | 否 |
| 6 | fcntl_setfl | `fcntl(fd, F_SETFL, O_NONBLOCK)` 设置/清除 NONBLOCK 标志 | 否 |
| 7 | lseek_set | `lseek(fd, 10, SEEK_SET)` → 读验证偏移位置 | 否 |
| 8 | lseek_cur | `lseek(fd, 5, SEEK_CUR)` → 相对偏移 | 否 |
| 9 | write_read_lseek | 写 20 字节 → lseek 到 10 → 读 10 字节，验证内容 | 否 |

## Layer 3: test_string.c 详细 case 列表

| # | Case 名 | 测什么 |
|---|---------|--------|
| 1 | strlen_basic | `strlen("hello")` == 5 |
| 2 | strlen_empty | `strlen("")` == 0 |
| 3 | strcmp_equal | `strcmp("abc", "abc")` == 0 |
| 4 | strcmp_less | `strcmp("abc", "abd")` < 0 |
| 5 | strcmp_greater | `strcmp("abd", "abc")` > 0 |
| 6 | strncmp_partial | `strncmp("abcdef", "abcxyz", 3)` == 0 |
| 7 | strcpy_basic | `strcpy(buf, "hello")` → strcmp 验证 |
| 8 | strncpy_pad | `strncpy(buf, "hi", 5)` → buf[2..4] == '\0' |
| 9 | strcat_basic | `strcpy(buf, "hi"); strcat(buf, " there")` → strcmp 验证 |
| 10 | strchr_found | `strchr("hello", 'l')` 指向 "llo" |
| 11 | strchr_missing | `strchr("hello", 'z')` == NULL |
| 12 | memcpy_basic | `memcpy(dst, src, 10)` → memcmp 验证 |
| 13 | memset_basic | `memset(buf, 'A', 10)` → 全部为 'A' |
| 14 | memmove_overlap | `memmove(buf+2, buf, 5)` 重叠区域正确拷贝 |

## Layer 3: test_malloc.c 详细 case 列表（迁移自 malloctest.c）

| # | Case 名 | 测什么 |
|---|---------|--------|
| 1 | malloc_small | `malloc(32)` 返回非 NULL，可写入和读取 |
| 2 | malloc_size_classes | `malloc(8/128/1024)` 各返回非 NULL，互不重叠 |
| 3 | malloc_free_reuse | `free(p)` → `malloc(同 size)` 应复用（地址相同或同 size class） |
| 4 | calloc_zero | `calloc(10, sizeof(int))` 返回非 NULL，内容全零 |
| 5 | malloc_large | `malloc(4096)` 走 mmap path，可写入 4096 字节 |
| 6 | realloc_grow | `realloc(p, 128)` 从 64 扩展到 128，原内容保留 |
| 7 | realloc_shrink | `realloc(p, 32)` 从 64 缩减到 32，前 32 字节保留 |
| 8 | free_all | 释放所有分配后不 crash |
| 9 | malloc_null | `malloc(0)` 返回 NULL 或最小分配（POSIX 允许两种） |
| 10 | double_free | `free(p); free(p)` 应不 crash（或返回错误） |

## Layer 3: test_stdio.c 详细 case 列表

| # | Case 名 | 测什么 |
|---|---------|--------|
| 1 | printf_string | `printf("hello %s", "world")` 输出到 stdout |
| 2 | printf_int | `printf("num %d", 42)` 格式化整数 |
| 3 | printf_hex | `printf("hex %x", 0xFF)` 格式化十六进制 |
| 4 | fprintf_stderr | `fprintf(stderr, "err")` 输出到 stderr（无缓冲） |
| 5 | fputc_fputs | `fputc('A', fp)` + `fputs("bc", fp)` → 读回验证 |
| 6 | puts_basic | `puts("hello")` 输出 + 自动换行 |
| 7 | fflush_stdout | 写满行缓冲 → fflush → 数据实际写出 |
| 8 | stdin_fgetc | `fgetc(stdin)` 从 pipe 读一个字符 |
| 9 | stdin_getchar | `getchar()` 等价 fgetc(stdin) |
| 10 | vfprintf_custom | `vfprintf(fp, "%d", ap)` 自定义格式化 |

## Layer 4: test_mmap.c 详细 case 列表

| # | Case 名 | 测什么 | 需要多进程？ |
|---|---------|--------|------------|
| 1 | mmap_anon | `mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)` 返回非 NULL | 否 |
| 2 | mmap_write_read | mmap 页写入数据 → 读回验证 | 否 |
| 3 | mmap_multi_page | mmap 3 页 (12288 字节) → 各页独立读写 | 否 |
| 4 | munmap_basic | `munmap(addr, size)` 释放 → 再访问应 fault（间接验证） | 否 |
| 5 | mmap_addr_hint | `mmap(hint_addr, ...)` 使用地址提示 | 否 |
| 6 | mmap_shm_fd | `shm_create` → `mmap(fd, MAP_SHARED)` → 写入 → 另进程 attach 读回 | 是 |
| 7 | memfd_create | `memfd_create("test", 0)` 返回 fd >= 0 | 否 |
| 8 | memfd_mmap | memfd → ftruncate → mmap → 写读验证 | 否 |
| 9 | ftruncate_grow | `ftruncate(fd, 8192)` 扩大 memfd → mmap 成功 | 否 |
| 10 | mmap_prot_exec | `mmap(PROT_READ|PROT_EXEC)` 代码页可执行（间接验证） | 否 |

## Layer 5: test_ipc.c 详细 case 列表

| # | Case 名 | 测什么 | 需要多进程？ |
|---|---------|--------|------------|
| 1 | shm_create | `shm_create(4096)` 返回有效 fd | 否 |
| 2 | shm_attach | `shm_attach(pid, 0)` attach 到指定进程的 SHM | 否（可用自身 PID） |
| 3 | shm_cross_process | A 创建 SHM → B attach → B 写入 → A 读回验证 | 是 |
| 4 | shm_refcount | 双进程 mmap 同一 SHM → close fd → 对端仍可访问 | 是 |
| 5 | notify_basic | `notify(target_pid)` → 目标 recv 收到 RECV_NOTIFY | 是 |
| 6 | req_resp | A req B → B resp → A 收到回复数据 | 是 |
| 7 | msg_msg_resp | A msg B (变长载荷) → B msg_resp → A 收到回复 | 是 |
| 8 | msg_large | msg 发送 4KB 数据 → 完整传送到对端 | 是 |
| 9 | msg_max_size | msg 发送接近 64KB 限制的数据 | 否 |
| 10 | req_timeout | `recv(buf, NULL, 0, 1000)` 超时返回 | 否 |

## Layer 6: test_socket.c 详细 case 列表

| # | Case 名 | 测什么 | 需要多进程？ |
|---|---------|--------|------------|
| 1 | socket_create | `socket(AF_UNIX, SOCK_STREAM, 0)` 返回 fd >= 0 | 否 |
| 2 | bind_listen | `bind(fd, addr)` + `listen(fd, 4)` 成功 | 否 |
| 3 | connect_accept | A listen → B connect → A accept → 连接建立 | 是 |
| 4 | socketpair_basic | `socketpair(AF_UNIX, SOCK_STREAM, 0, sv)` → 双端可互读写 | 否 |
| 5 | socketpair_write_read | sv[0] 写 "hello" → sv[1] 读回 → strcmp 验证 | 否 |
| 6 | stream_bidirectional | 双端各自同时读写，验证双向字节流 | 否（socketpair） |
| 7 | scm_rights_fd | sendmsg 传递 fd → recvmsg 收到 fd → 新 fd 可读原 pipe 数据 | 是 |
| 8 | shutdown_write | `shutdown(fd, SHUT_WR)` → 对端 read 返回 0 (EOF) | 否（socketpair） |
| 9 | shutdown_read | `shutdown(fd, SHUT_RD)` → 对端 write 返回 -EPIPE | 否（socketpair） |
| 10 | accept_backlog | listen(backlog=2) → 2 个 client connect → accept 依次取出 | 是 |
| 11 | sendmsg_recvmsg_cmsg | sendmsg 带 cmsg 辅助数据 → recvmsg 解析 cmsg 级别和类型 | 否（socketpair） |

## Layer 7: test_process.c 详细 case 列表

| # | Case 名 | 测什么 | 需要多进程？ |
|---|---------|--------|------------|
| 1 | spawn_basic | `spawn(elf_data, elf_size)` 创建子进程，子进程运行并 exit | 是 |
| 2 | waitpid_child | `waitpid(child_pid, &exit_code)` 回收子进程，exit_code 正确 | 是 |
| 3 | waitpid_no_child | `waitpid(-1, NULL)` 无子进程时返回 -ECHILD | 否 |
| 4 | spawn_inherit_fd | 子进程继承 fd 0/1，可通过 pipe 与父进程通信 | 是 |
| 5 | exit_code | 子进程 `exit(42)` → 父进程 waitpid 获得 exit_code=42 | 是 |
| 6 | spawn_orphan | 子进程 spawn 孙进程 → 子进程 exit → init 收养孙进程 | 是 |

注：execv 和 exit_group 尚未实现，待实现后追加对应测试 case。

## Layer 8: test_signal.c 详细 case 列表

| # | Case 名 | 测什么 | 需要多进程？ |
|---|---------|--------|------------|
| 1 | kill_invalid_pid | `kill(-1, SIGTERM)` 无目标进程返回 -ESRCH | 否 |
| 2 | sigaction_register | `sigaction(SIGINT, &act, NULL)` 注册处理函数 | 否 |
| 3 | kill_deliver | A kill B → B 收到信号执行 handler | 是 |
| 4 | sigaction_restore | `sigaction(SIGINT, &new_act, &old_act)` → old_act 保存旧处理函数 | 否 |
| 5 | sigreturn | handler 执行后 sigreturn 恢复上下文 | 是 |

注：tgkill 尚未实现，待实现后追加测试 case。当前信号实现的已知限制：无 EINTR（信号不中断阻塞 syscall）、无 sigprocmask/sigpending、默认动作均为 exit(-1)。

## Layer 9: test_poll.c 详细 case 列表

| # | Case 名 | 测什么 | 需要多进程？ |
|---|---------|--------|------------|
| 1 | poll_pipe_readable | pipe 写入后 → poll(fd, POLLIN) 返回 POLLIN | 否 |
| 2 | poll_pipe_writable | 空 pipe → poll(fd, POLLOUT) 返回 POLLOUT | 否 |
| 3 | poll_pipe_empty | 空 pipe 读端 → poll(fd, POLLIN, timeout=0) 返回 0 | 否 |
| 4 | poll_timeout | poll 无事件 + timeout=1000 → 超时返回 0 | 否 |
| 5 | poll_socketpair | socketpair 双端 → 各 poll POLLIN|POLLOUT | 否 |
| 6 | poll_multiple_fd | 同时 poll 3 个 fd（pipe + socketpair + dev） | 否 |
| 7 | poll_wakeup | A poll 阻塞 → B 写入 → A poll 返回 | 是 |

## Layer 10: test_pci.c 详细 case 列表（迁移自 test_pcie.c）

| # | Case 名 | 测什么 |
|---|---------|--------|
| 1 | pci_dev_info_valid | `pci_dev_info(0,2,0,&info)` 返回 AHCI 设备（vendor=0x8086） |
| 2 | pci_dev_info_invalid | `pci_dev_info(255,0,0,&info)` 返回 -ENOENT（无设备） |
| 3 | pci_bar_info | AHCI 设备至少有一个 BAR（size > 0） |
| 4 | pci_scan_all | 遍历 bus 0 所有设备，找到已知设备（AHCI + bochs-display） |
| 5 | open_dev_kms | `open_dev(DEV_KMS)` 返回有效 fd + target_pid |
| 6 | open_dev_fs | `open_dev(DEV_FS)` 返回有效 fd + target_pid |

## 现有临时测试状态

以下临时测试程序已存在但不使用 Unity 框架，迁移后删除原文件：

| 文件 | 测试内容 | 对应 Unity 测试 | 迁移状态 |
|------|---------|----------------|---------|
| `user/malloctest.c` | malloc/free/calloc/realloc 7 项 | test_malloc.c | 待迁移 |
| `user/writetest.c` | 文件 write+read+strcmp 验证 | test_fcntl.c (case 1) | 待迁移 |
| `user/test_pcie.c` | PCIe 设备枚举扫描 | test_pci.c | 待迁移 |

CMake 中已有 `add_user_elf` 目标（hello/malloc/pcie/write），迁移后统一改为 `user/test/` 目录下的规范测试目标，LINK_LIBS 加入 `unity`。

## setjmp/longjmp 实现设计

x86-64 上 setjmp/longjmp 需保存/恢复 callee-saved 寄存器：

### setjmp

```asm
; int setjmp(jmp_buf env);
; jmp_buf = uint64_t[8]：rbx, rbp, r12, r13, r14, r15, rsp, rip
setjmp:
    mov [rdi+0],  rbx
    mov [rdi+8],  rbp
    mov [rdi+16], r12
    mov [rdi+24], r13
    mov [rdi+32], r14
    mov [rdi+40], r15
    mov [rdi+48], rsp        ; 保存栈指针（调用者的 rsp）
    lea rax, [rip+return_label]  ; 获取返回地址
    mov [rdi+56], rax        ; 保存 rip
    xor eax, eax             ; return 0（首次返回）
return_label:
    ret
```

### longjmp

```asm
; void longjmp(jmp_buf env, int val);
longjmp:
    mov rbx, [rdi+0]
    mov rbp, [rdi+8]
    mov r12, [rdi+16]
    mov r13, [rdi+24]
    mov r14, [rdi+32]
    mov r15, [rdi+40]
    mov rsp, [rdi+48]        ; 恢复栈指针
    mov rax, rsi             ; val
    test rax, rax            ; val 不能为 0
    jnz .jump
    inc rax                  ; val==0 时改为 1（POSIX 规定）
.jump:
    jmp [rdi+56]             ; 跳到 setjmp 保存的 rip（回到 setjmp 返回点）
```

### jmp_buf 定义

```c
// user/include/setjmp.h
typedef uint64_t jmp_buf[8];  // rbx, rbp, r12-r15, rsp, rip

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);
```

### CMake 集成

setjmp/longjmp 为纯汇编文件，通过 `add_user_lib` 加入 libc.a：

```cmake
# user/lib/CMakeLists.txt 增加
add_user_lib(setjmp ASM_SOURCES setjmp.S)
```

完成后移除 Unity 的 `UNITY_EXCLUDE_SETJMP` 定义，解锁测试隔离（FAIL 后继续执行）。

## 未来扩展

- **pthread 测试**：clone/futex/arch_prctl/set_tid_address/gettid/tgkill 等 syscall 实现后，新增 test_pthread.c（Layer 11），覆盖线程创建/join/mutex/futex/TLB 等
- **execv 测试**：execv/exit_group 实现后，在 test_process.c 中追加 execv_replace 和 exit_group case
- **tgkill 测试**：tgkill 实现后，在 test_signal.c 中追加 tgkill_specific case（需 pthread）
- **gtest 迁移**：需要 C++ 标准库（libc++）+ exceptions runtime + pthread。测试逻辑不变，断言宏换名（`TEST_ASSERT_EQUAL_INT` → `EXPECT_EQ`）
- **Linux ABI 兼容**：对齐 syscall 编号和 struct 布局后，可直接用 Linux 交叉编译工具链编译测试程序，无需改测试代码
- **自动化回归**：run.sh 中解析串口 log，检测 `=== Summary ===` 行判断全量 PASS/FAIL
