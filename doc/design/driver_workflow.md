# 驱动工作流

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | IPC 模型 | 动态共享内存 + 环形缓冲区 + sleeping flag + sys_recv 超时 | 驱动醒着时零内核入口（快速路径不走 syscall）；睡眠时 sys_recv 等待通知或超时 |
| 2 | Sleeping flag 协议 | 驱动设 sleeping=1 后二次检查数据，防止 lost-wakeup | 竞态窗口：发送方在 sleeping==0 时写入，驱动紧接着睡眠；二次检查确保数据不丢失 |
| 3 | kbd_driver 工作模式 | 纯中断驱动（RECV_NOTIFY） | kbd 数据源是内核 xHCI ISR 通知，不需要超时轮询 |
| 4 | kms_driver 位置 | 内核态（非用户进程） | 已迁入内核，详见 [kms.md](kms.md)。本文件不再描述 kms_driver 工作流 |
| 5 | fs_driver 通信 | SHM + sys_notify + sys_recv | fs_driver 与 shell 通过 FS SHM 通信，与 disk_driver 通过 Disk SHM 通信 |
| 6 | 定时等待 | sys_recv(timeout_ms) | timeout_ms=0 无限等待，>0 超时后唤醒；返回 0=收到消息，超时返回空消息 |

### TSC 时钟基础设施

- arch/x64/utils.h : `rdtsc64()` — TSC 读取内联函数
- arch/x64/apic.c : `tsc_freq` / `tsc_per_ms` — TSC 校准值（PIT 10ms 窗口校准）
- arch/x64/apic.c : `sched_clock()` — 返回自启动以来的纳秒数
- kernel/proc.c : `timer_queue_insert` / `timer_queue_remove` — 按 wait_deadline 升序排列的定时等待队列（per-CPU）

### sys_recv(timeout_ms)

- Syscall 号：2（SYS_RECV）
- timeout_ms=0：无限等待
- timeout_ms>0：设 `wait_deadline = sched_clock() + timeout_ms * 1000000`，插入定时队列
- 返回：0=成功收到消息，超时返回空消息（recv_head == recv_tail）

task_t 新增字段（kernel/proc.h : task_t）：
  wait_deadline : uint64_t — sched_clock() 纳秒截止时间，0=无限等待
  wait_timed_out : uint8_t — 1=超时唤醒，0=notify 唤醒

cpu_local_t 新增字段：
  timer_queue : list_node_t — 定时等待队列哨兵（按 wait_deadline 升序排列）

定时器到期检查：`timer_handler()` 中 EOI 之后遍历本 CPU timer_queue，将 `wait_deadline <= sched_clock()` 的进程唤醒（READY + wait_timed_out=1 + 入 run_queue）。队列有序，遇到 `wait_deadline > now` 即停。

### 动态共享内存

- `memfd_create(name, flags)` + `ftruncate(fd, size)` — 创建 SHM 区域，分配物理页，返回 fd
- `open("/dev/xxx")` + `mmap(fd, ...)` — 访问设备关联的 SHM（从 inode->shm 取物理页映射）
- `dev_create(name, dev_type, shm_fd)` — 用户态驱动将 memfd 关联到设备 inode，consumer 通过 open+mmap 访问
- SHM 区域通过 `mm_t->mmap_regions` 链表管理（含 `shm_obj` 引用计数），proc_reap 时递减 s_count，归零释放物理页

### Sleeping Flag 协议

驱动在共享页中声明 `sleeping=1` 后才进入 `sys_recv` 睡眠。发送方写入环形缓冲区后检查 sleeping flag：

- **快速路径**（sleeping==0）：驱动正在处理，会自行发现新数据，无需额外 syscall
- **慢速路径**（sleeping==1）：驱动在睡眠，调用 `sys_notify()` 唤醒

时序安全（双检查协议）：

驱动端：
1. 检查 ring 是否为空
2. 若空：设 sleeping=1（x86 store 可见性由 cache coherence 保证）
3. 再次检查 ring（防止写入方在步骤 1-2 之间写入）
4. 若仍空：sys_recv(0)（深度睡眠）
5. 唤醒后：设 sleeping=0

发送方：
1. 写入 ring（store）
2. 读 sleeping flag（load）
3. 若 sleeping==1：sys_notify()

竞态窗口：发送方在步骤 2 读到 sleeping==0，但驱动紧接着设 sleeping=1 并睡眠。此时数据已在 ring 中，驱动将在下次 sys_recv 超时或 notify 后发现。最坏延迟取决于超时时间。

### kbd_driver 工作流

kbd_driver（driver/kbd_driver.cc）是 IOPL=0 的用户态驱动，通过 RECV_NOTIFY 驱动（非轮询）：

1. `device_register(DEV_KBD)` — 注册设备
2. `open("/dev/usb_hid")` + `mmap(...)` — 映射内核 USB HID SHM
3. `get_keycode_init(hid_shm)` — 初始化
4. 主循环：
   - `sys_recv(&msg, NULL, 0, 0)` — 深度睡眠，等 RECV_NOTIFY（xHCI ISR wake）
   - RECV_NOTIFY：`get_keycode(&ev)` 循环读 SHM ring → key_event → `kbd_push()` → 写 kbd_ring SHM → 检查 consumer_sleeping → notify terminal
   - RECV_REQ：处理 KBD_IOCTL_BIND/UNBIND

kbd_driver 自身不需要轮询窗口（数据源是硬件中断通知）。`kbd_sleeping` flag 预留未来使用。

### fs_driver 工作流

fs_driver 创建 4 页 FS SHM，SCM_RIGHTS 传 fd 给 shell，FAT32 文件系统服务：

1. `memfd_create("fs_shm", 0)` + `ftruncate(shm_fd, 4 * 4096)` — 创建 FS SHM
2. SCM_RIGHTS 传 FS SHM fd 给 shell
3. `device_register(DEV_BLOCK)` — 注册设备
4. 主循环 `sys_recv` 等待 RECV_NOTIFY（shell 通知）：
   - 收到通知后读 `fs_req_shm`，执行 readdir/open/read/close/raw_read/touch/mkdir
   - 与 disk_driver 通信：写 `disk_req_shm` → `sys_notify(disk_pid)` → `sys_recv` 等响应 → 读 `disk_resp_shm`
   - 处理完写 `fs_resp_shm`，检查 client_sleeping，notify shell

### shell 工作流

shell 通过 SCM_RIGHTS 接收 FS SHM fd，通过 fd 0/1 pipe 与 terminal 通信：

- 键盘输入：`sys_read(0, ...)` 从 stdin pipe 读
- 输出：printf → sys_write(1, ...) → stdout pipe → terminal
- 文件操作：写 `fs_req_shm` → notify fs_driver → `sys_recv` 等响应 → 读 `fs_resp_shm`
- 命令：ls/cat/cd/pwd/touch/mkdir/run/malloc/free/mtest/r/h

### 与其他模块的关系

- **IPC/SHM**：动态 SHM 创建/attach 机制。详见 [ipc.md](ipc.md)
- **xHCI**：ISR 通过 USB HID SHM 通知 kbd_driver。详见 [xhci.md](xhci.md)
- **KMS**：已迁入内核，terminal 通过 ioctl(FLIP) 请求 flip。详见 [kms.md](kms.md)
- **用户态驱动**：驱动进程架构总览。详见 [user_driver.md](user_driver.md)

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| DRM/KMS compositor 重构 | 逐字符 KMS_CMD_PUTC 消息模型改为共享文本 buffer + compositor 模型：80x25 char+attr 数组 shell 直接写入（零 syscall），KMS 定时扫描 dirty region 重绘 | 中 |
| disk_driver 移除 | fs_driver 重构完成后移除用户态 disk_driver 进程，fs_driver 直接通过 sys_block_async 访问内核 AHCI | 中 |
| libc driver_loop 框架 | 抽象 poll+process+sleeping flag 循环为通用 driver_loop()，kbd/fs 各自内联 loop 逻辑可复用 | 低 |
