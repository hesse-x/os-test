# 启动流程

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 启动方式 | UEFI → EFI stub → 内核 | 详见 [uefi.md](uefi.md) |
| 2 | init 启动顺序 | 硬编码串行：kbd → kms → terminal | 驱动间有依赖，硬编码简单可靠 |
| 3 | kernel_main 行为 | 加载 init 后直接进 idle，不创建 pipe/不等驱动就绪 | 最小内核原则，用户态服务自管理 |
| 4 | 进程加载 | init 从 FAT32 spawn 服务进程 | FAT32 已可用，后续服务不需裸 LBA |
| 5 | 失败处理 | 任何步骤失败直接停住，不降级运行 | 早期系统，降级增加复杂性且难以调试 |
| 6 | I/O 端口权限 | ioperm + IOPM 精细控制 | 替代 IOPL（已删除），每进程独立 IOPM 位图 |
| 7 | framebuffer 映射 | mmap MAP_PHYSICAL | KMS 用户态驱动映射设备物理地址，无需内核参数 |

### 启动流程

```
UEFI → BOOTX64.EFI → myos.elf → kernel_main
  kernel 从裸 LBA 加载 init (PID 3)
  init 从 FAT32 spawn:
    kbd_driver → kms_driver → terminal
  terminal 从 FAT32 spawn:
    shell（通过 pipe 通信）
```

磁盘布局详见 [cmake.md](cmake.md)。

### init 进程

init/init.c : main()，链接 libc.a。

1. 等 DEV_FS 就绪（`device_lookup` 轮询）
2. spawn kbd_driver → 等 DEV_KBD 就绪（轮询 + 计数超时 10000 次 yield）
3. spawn kms_driver → 等 DEV_KMS 就绪
4. spawn terminal
5. `waitpid(-1)` 循环回收子进程

spawn 辅助函数：`stat(path)` → `malloc + open + read` → `sys_spawn(buf, size)` → `free(buf)`。失败时串口打印错误并停住。

### terminal 拉起 shell

terminal 启动后创建两对 pipe 作为 stdin/stdout，dup2 调整 fd 0/1，spawn shell 继承调整后的 fd，dup2 恢复 terminal 自己的 fd 0/1，fcntl 设 terminal fd 0 非阻塞。pipe 是 terminal 和 shell 的内部协议。详见 [terminal.md](terminal.md)。

### ioperm + IOPM

sys_ioperm（syscall #22）语义与 Linux 一致，任何进程都能调用。

proc_t::iopm（uint8_t *），初始 NULL。sys_ioperm 调用时若 iopm==NULL 则 kmalloc(IOPM_SIZE) 初始化全 0xFF（deny all），按参数清零（allow）或置 1（deny）对应位。

上下文切换时 update_tss_iopm()：iopm==NULL 填充全 0xFF 到 TSS IOPM 区，iopm!=NULL 拷贝进程 iopm 到 TSS IOPM 区。proc_reap 时 kfree(iopm)。

### mmap MAP_PHYSICAL

sys_mmap 扩展：flags 含 MAP_PHYSICAL（0x80000000）时不分配新页面，将指定物理地址映射到用户地址空间。KMS 用此映射 framebuffer：sys_fb_info 获取 fb_phys → mmap(addr, size, ..., MAP_PHYSICAL, fb_phys)。

### 孤儿收养

内核全局 init_pid（kernel/proc.h : init_pid），kernel_main 创建 init 后设置。sys_exit 时子进程 parent_pid 改为 init_pid，init 的 waitpid(-1) 循环回收所有后代。详见 [proc.md](proc.md)。

### 与其他模块的关系

| 机制 | 详细文档 |
|------|---------|
| 进程管理（fork/execve/waitpid/spawn） | [proc.md](proc.md) |
| syscall 接口（dup2/fcntl/ioperm/mmap） | [syscall.md](syscall.md) |
| 内存管理（mmap/munmap） | [mem.md](mem.md) |
| UEFI 引导 | [uefi.md](uefi.md) |
| Terminal/Shell | [terminal.md](terminal.md) |

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| 动态启动顺序 | 当前硬编码 kbd→kms→terminal，改为配置文件或依赖声明自动编排 | 低 |
| 驱动就绪通知 | 当前 init 轮询 device_lookup，改为驱动就绪时主动 notify init | 中 |
| sys_exec | 进程映像替换，fork+execve 已部分替代 sys_spawn | 中 |
| 超时降级 | 当前超时直接停住，改为降级运行（如无 KMS 时串口 shell） | 低 |
