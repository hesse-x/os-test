# 启动流程 — init + 从 FAT32 启动用户态服务

## 概述

内核从裸 LBA 加载 3 个进程（disk_driver + fs_driver + init），其余进程由 init 从 FAT32 启动。IOPL 机制已删除，端口隔离改用 ioperm + IOPM。

## 启动流程

```
UEFI → boot.efi → kernel
kernel 从裸LBA加载:
  - disk_driver (PID1) — 自己 device_register(DEV_DISK)
  - fs_driver   (PID2) — 自己 device_register(DEV_FS)
  - init        (PID3)
init 从 FAT32 启动:
  - kbd_driver → kms_driver → terminal
terminal 从 FAT32 启动:
  - shell（通过 pipe 通信）
```

kernel_main 不创建 pipe，不等待驱动就绪，加载完 3 个 ELF 后直接进 idle。

## IOPL 已删除

- `proc_t` 无 iopl 字段
- `sys_spawn(elf_data, elf_size)` — 2 参数（__syscall2）
- `process_create_elf(elf_data, elf_size)` — 无 iopl/map_fb 参数
- 所有进程 RFLAGS.IOPL=0（rflags = 0x202，仅 IF=1）
- 硬件 I/O 通过 ioperm + IOPM 精细控制

## ioperm + IOPM

### 接口

```c
int ioperm(unsigned long from, unsigned long num, int turn_on);
// syscall: sys_ioperm(from, num, turn_on)
// 编号: 26
```

语义与 Linux ioperm 一致。不检查权限，任何进程都能调用。

### 内核实现

- `proc_t::iopm`（`uint8_t *`），初始 NULL
- `sys_ioperm`：若 iopm==NULL 则 `kmalloc(IOPM_SIZE)` 初始化全 0xFF（deny all）；按参数清零（allow）或置 1（deny）对应位
- `sys_ioperm` 返回前立即更新当前 CPU TSS 的 IOPM 区（确保端口权限立即生效）

### 上下文切换

- `update_tss_iopm()` 在 `schedule()` 中选出 next 后调用（`switch_to` 前，`scheduler_lock` 外）：
  - `next->iopm == NULL`：填充 8KB 全 0xFF 到 TSS IOPM 区
  - `next->iopm != NULL`：拷贝进程 iopm 到 TSS IOPM 区
- 每 CPU TSS 包含 8KB IOPM 区（`tss_t::iopm[IOPM_SIZE]`，IOPM_SIZE=8192），TSS limit 覆盖 IOPM 末尾

### 生命周期

- `proc_reap` 里 `kfree(iopm)` + 置 NULL

### 驱动调用

```
kbd_driver._start():
  ioperm(0x60, 2, 1)       — 开端口（0x60-0x61）
  irq_bind(33)              — 绑中断
  device_register(DEV_KBD)  — 注册设备
  主循环

disk_driver._start():
  ioperm(0x1F0, 8, 1)      — ATA 命令块寄存器
  ioperm(0x3F6, 2, 1)      — ATA 控制块寄存器
  主循环
```

**disk_driver 已自注册** `device_register(getpid(), DEV_DISK)`，与 kbd_driver/fs_driver 一致。

## mmap MAP_PHYSICAL

KMS 通过 mmap MAP_PHYSICAL 映射 framebuffer，不再依赖 process_create_elf 的 map_fb 参数：

```c
// KMS 在 display_backend_init() 中：
struct kfb_info kfb;
sys_fb_info(&kfb);
mmap((void*)0x700000, kfb.fb_size, PROT_READ | PROT_WRITE, MAP_PHYSICAL, kfb.fb_phys);
```

- `sys_mmap` 扩展：若 flags 含 `MAP_PHYSICAL`（0x80000000），不做新页面分配，将指定物理地址映射到用户地址空间
- 不校验物理地址范围

## init 进程

### 位置与构建

- `init/init.c`，C 语言，链接 libc
- `add_user_elf(init C SOURCES init.c LINK_LIBS c)`

### 启动流程

```c
int main() {
    // 1. 等 fs_driver 就绪
    while (device_lookup(DEV_FS) == 0) { yield(); }

    // 2. spawn kbd_driver → 等 DEV_KBD 就绪（超时停住）
    spawn_service("/driver/kbd.dev");
    wait_dev_ready(DEV_KBD);

    // 3. spawn kms_driver → 等 DEV_KMS 就绪（超时停住）
    spawn_service("/driver/kms.dev");
    wait_dev_ready(DEV_KMS);

    // 4. spawn terminal
    spawn_service("/usr/bin/terminal");

    // 5. 收养孤儿 + 回收子进程
    while (1) { waitpid(-1, &status, 0); }
}
```

### 辅助函数

```c
static int spawn_service(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) { ... halt(); }
    void *buf = malloc(st.st_size);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { ... halt(); }
    read(fd, buf, st.st_size);
    close(fd);
    int pid = sys_spawn(buf, st.st_size);
    free(buf);
    if (pid <= 0) { ... halt(); }
    return pid;
}

static void wait_dev_ready(int dev) {
    for (int i = 0; i < 10000; i++) {
        if (device_lookup(dev) != 0) return;
        yield();
    }
    ... halt();
}
```

### 关键决策

- **硬编码启动顺序**：kbd → kms → terminal，串行 spawn
- **超时策略**：轮询 + 计数超时（10000 次 yield），超时串口打印错误并停住
- **失败处理**：任何步骤失败（stat/open/spawn/超时）直接停住，不降级运行
- init 不 exec 成 terminal，terminal 有独立 PID

## terminal 拉起 shell

terminal 链接 libc，使用 `main()` 入口。启动流程：

```
terminal main():
  1. display_client_init()     — 等 DEV_KMS 就绪 + attach 显示 SHM
  2. req(kbd_pid, KBD_REQ_BIND) — 绑定键盘
  3. shm_attach(kbd_pid)       — attach kbd_shm
  4. VT100 初始化 + cell 缓冲区分配
  5. pipe(p stdin) + pipe(p stdout) — 创建两对 pipe
  6. dup2(stdin_w, 1) + dup2(stdout_r, 0) — 调整 fd 0/1 给 shell 用
  7. open("/usr/bin/shell") + read + spawn → shell 继承调整后的 fd 0/1
  8. dup2 恢复 terminal 自己的 fd 0/1
  9. fcntl(0, F_SETFL, O_RDONLY | O_NONBLOCK) — terminal fd 0 非阻塞
  10. 主循环（kbd_ring → fd 1 写，fd 0 读 → VT100 → KMS ring）
```

pipe 是 terminal 和 shell 的内部协议，不由 init 或内核编排。

## dup2

```c
int dup2(int old_fd, int new_fd);
// syscall: sys_dup2(old_fd, new_fd)
// 编号: 27
```

- **内核层** `sys_dup2`：操作 `proc_t::fd_table[]`（关闭 new_fd 旧 pipe + 复制 old_fd 到 new_fd + ref count 调整）
- **libc 层** `dup2()`：调 sys_dup2 后同步更新 libc `fd_table[new] = fd_table[old]`（type/flags/fs_fd/offset）

## fcntl

```c
int fcntl(int fd, int cmd, ...);
// 只实现 F_GETFL / F_SETFL
// syscall: sys_fcntl(fd, cmd, arg)
// 编号: 28
```

- **F_GETFL**：直接读 libc fd_table[fd].flags
- **F_SETFL**：如果是 FD_PIPE，调 `sys_fcntl` 更新内核 flags；然后更新 libc fd_table[fd].flags
- 内核 `sys_read` 检查 fd flags 里的 O_NONBLOCK，内核侧 flags 与 libc 同步

## stat

```c
int stat(const char *path, struct stat *st);
```

- libc 封装，通过 sys_msg 发 FILE_CMD_STAT 给 fs_driver
- fs_driver 返回 file_size 等信息
- `struct stat` 至少包含 `st_size`

## 孤儿收养

- 内核全局变量 `init_pid`（`kernel/proc.h` 声明，`proc.cc` 定义，初始 -1），kernel_main 创建 init 后设置
- `sys_exit` 时：遍历 procs[]，把 `parent_pid == self_pid` 的子进程改为 `parent_pid = init_pid`
- init 的 `waitpid(-1)` 循环负责回收所有后代

## sys_waitpid(-1)

支持 pid=-1：等任意 ZOMBIE 子进程，返回其 PID。无 ZOMBIE 则阻塞（WAIT_CHILD）。

## sys_spawn

- `sys_spawn(elf_data, elf_size)`（2 参数，__syscall2）
- 无 256KB 大小限制：由 kmalloc 自然限制（kmalloc 失败返回 ENOMEM）
- 自动继承 fd 0/1

## kernel_main

- 加载 3 个 ELF：disk_driver（LBA 1）+ fs_driver（LBA 101）+ init（LBA 201）
- 不创建 pipe，不注册设备，不等驱动就绪，直接进 idle

## mkdisk.sh

```
LBA 1:   disk_driver.elf
LBA 101: fs_driver.elf
LBA 201: init.elf
裸 ELF 分区: LBA 1-300（3 slot × 100 扇区）
FAT32 分区: LBA 301 起
```

FAT32 内容：`/driver/kbd.dev`、`/driver/kms.dev`、`/usr/bin/terminal`、`/usr/bin/shell`（无冗余的 disk.dev/fs.dev 副本）

## sys_exec

移至 [todo.md](todo.md) 远期目标。
