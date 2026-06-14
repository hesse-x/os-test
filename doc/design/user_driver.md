# 用户态驱动与 IPC

> **已实现**。`irq_owner[]` 已改为 atomic 访问（`__atomic_store_n` RELEASE / `__atomic_load_n` ACQUIRE），`trap_dispatch` 和 `sys_notify` 中唤醒进程使用 `scheduler_lock` 保护 run_queue + run_count。

## 架构

键盘驱动、磁盘驱动运行在用户态，作为独立 ELF 进程。内核仅提供最小机制（调度、内存、中断分发、syscall），驱动只做硬件抽象，策略留给消费者。

### 进程启动顺序

内核启动时依次从磁盘加载 3 个 ELF：

| LBA  | ELF             | PID | IOPL | 说明         |
|------|-----------------|-----|------|--------------|
| 1    | disk_driver.elf | 2   | 3    | 磁盘驱动     |
| 33   | kbd_driver.elf  | 3   | 3    | 键盘驱动     |
| 65   | shell.elf       | 4   | 0    | Shell        |

内核启动时 `ata_read_lba` 仅用于加载这 3 个 ELF，之后磁盘 I/O 由用户态 disk_driver 接管。

## 共享页通信

3 个固定虚拟地址页，映射到同一物理页，所有用户进程 PML4 共享：

所有驱动间 IPC 统一使用动态共享内存（sys_shm_create/sys_shm_attach），详见 [dynamic_shm_migration.md](dynamic_shm_migration.md)。

数据结构定义在 `common/shm.h`。

### kbd_shm（0x500000）

```
struct kbd_shm {
    uint32_t head;      // 写入位置（环形缓冲区）
    uint32_t tail;      // 读取位置
    uint8_t  data[4088]; // 按键 ASCII 环形缓冲区
};
```

### disk_req_shm（0x501000）

```
struct disk_req_shm {
    uint32_t cmd;        // READ=0, WRITE=1
    uint32_t lba;
    uint32_t count;      // 扇区数
    uint8_t  data[4076]; // 写请求的数据
};
```

### disk_resp_shm（0x502000）

```
struct disk_resp_shm {
    uint32_t status;     // 0=成功
    uint32_t count;      // 实际传输扇区数
    uint8_t  data[4088]; // 读请求返回的数据
};
```

## 通知机制

共享内存传数据 + syscall 做通知。中断绑定：`sys_irq_bind(irq)` 让内核在 IRQ 发生时直接唤醒绑定进程。

### 中断分发流程

`trap_dispatch`（`kernel/trap.cc`）对硬件 IRQ 的处理：

1. 若 `irq_owner[irq] >= 0`：唤醒绑定进程（`BLOCKED+WAIT_NOTIFY → READY`），发送 EOI，返回
2. 否则查 `irq_handlers[]` 注册表（如 timer_handler）

`irq_owner[]` 数组在 `isr_init()` 中初始化为 -1，`sys_irq_bind` 时写入当前进程 PID。

## I/O 访问

驱动进程 IOPL=3（通过 `proc_t::iopl` 字段设置，在 `build_kstack` 中写入 trapframe.rflags），可直接 `in`/`out`，零 syscall 开销。普通进程 IOPL=0，执行 I/O 指令触发 #GP。

## Syscall（7 个）

| # | 用户态接口            | 内核实现       | 说明                     |
|---|----------------------|----------------|--------------------------|
| 0 | `sys_putc(c)`        | `__sys_putc`   | 输出字符到 fb + serial   |
| 1 | `sys_getpid()`       | `__sys_getpid` | 返回当前进程 PID         |
| 2 | `sys_yield()`        | `__sys_yield`  | 主动让出 CPU             |
| 3 | `sys_getc()`         | `__sys_getc`   | deprecated，返回 -1      |
| 4 | `sys_wait()`         | `__sys_wait`   | 阻塞等待通知             |
| 5 | `sys_notify(pid)`    | `__sys_notify` | 唤醒指定进程             |
| 6 | `sys_irq_bind(irq)`  | `__sys_irq_bind`| 绑定当前进程到 IRQ       |

- `__syscall0`/`__syscall1` 等为底层内联汇编（`arch/x64/utils.h`）
- 语义封装定义在 `common/syscall.h`
- `sys_getc` 保留编号占位，实现为 stub 返回 -1

## 用户态进程

### 键盘驱动（driver/kbd_driver.cc）

1. `sys_irq_bind(33)` 绑定键盘 IRQ（向量 33 = IRQ1）
2. 循环：`sys_wait()` → `inb(0x60)` 读扫描码 → 翻译 ASCII → 写入 `kbd_shm` 环形缓冲区 → `sys_notify(sys_lookup_dev(DEV_TERMINAL))`
3. 仅处理 make code（bit 7 = 0），break code 忽略

### 磁盘驱动（driver/disk_driver.cc）

1. 循环：`sys_wait()` → 读 `disk_req_shm` → ATA PIO LBA28 操作 → 写 `disk_resp_shm` → `sys_notify(sys_lookup_dev(DEV_FS))`
2. 支持 READ（cmd=0），WRITE（cmd=1）暂未实现
3. ATA 驱动字节 0xF0（从盘），与 disk.img 作为 QEMU 第二 IDE 设备对应

### Shell（shell/shell.cc）

- 键盘输入：从 `kbd_shm` 环形缓冲区读取（`getc`），空时 `sys_wait()` 阻塞
- 命令行编辑：`readline` 支持退格，最长 80 字符
- 磁盘操作：写 `disk_req_shm` → `sys_notify(sys_lookup_dev(DEV_DISK))` → `sys_wait()` → 读 `disk_resp_shm`
- 命令：
  - `r LBA [COUNT]` — 读取磁盘扇区，hex dump 显示
  - `h` — 显示帮助

## 已移除的内核态组件

- `driver/kbd.cc` / `driver/kbd.h` — 内核键盘驱动已删除
- `keyboard_handler` — 内核 IRQ33 处理器已删除，不再注册
- `kbd_init()` 调用已删除
- `WAIT_KBD` 状态已从 `wait_event_t` 枚举中删除（仅保留 `WAIT_NONE, WAIT_NOTIFY`）
- `sys_getc` 改为 stub（不再阻塞，直接返回 -1）

## 目录重组

- 用户态驱动进程：`user/` → `driver/`（kbd_driver.cc, disk_driver.cc）
- Shell：`user/` → `shell/`（shell.cc）
- 内核保留驱动：`driver/` → `kernel/`（fb.cc/h, ata.cc/h，仅内核启动加载 ELF 时使用）
- `driver/CMakeLists.txt` 删除，fb/ata 合入 `kernel/CMakeLists.txt`
- `common/syscall.h` 新建：syscall 编号和语义封装从 `arch/x64/utils.h` 抽出，底层 `__syscall0` 等改名带前缀
