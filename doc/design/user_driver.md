# 用户态驱动

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 内核态 vs 用户态划分 | 需要内核独有能力（APIC 编程、页表映射、物理连续内存分配、DMA）的驱动放内核态；只需 MMIO 读写的可留用户态 | 与微内核原则一致 |
| 2 | 内核态驱动 | PCIe 枚举、xHCI (USB 主控)、AHCI (磁盘)、KMS (display) | 这些需要 DMA、中断路由、物理连续内存 |
| 3 | 用户态驱动 | kbd_driver（翻译推送）、terminal（VT100+渲染）、fs_driver（FAT32）、shell | 这些只需 SHM + notify 通信 |
| 4 | 设备发现 | `device_register(dev_type)` 注册 + `open("/dev/xxx")` 查询 | dev_table 是内核内部注册表，open 时内核查 dev_table 找到驱动 PID 绑定到 FD_DEV |
| 5 | IOPL | kbd_driver IOPL=0（不再访问 I/O 端口，通过 SHM 接收数据） | PS/2 路径已移除，kbd_driver 不再需要 inb/outb |
| 6 | 中断分发 | `irq_owner[]` 数组：若 irq_owner[irq] >= 0，向绑定进程入 RECV_IRQ + wake；否则查 irq_handlers[] | 用户态驱动通过 sys_irq_bind 绑定 IRQ（kbd_driver 已不使用此路径，改为 RECV_NOTIFY） |

### 驱动架构

| ELF | PID | IOPL | 功能 | SHM 角色 |
|-----|-----|------|------|---------|
| kbd_driver | 动态 | 0 | 键盘翻译推送（USB HID SHM → kbd_ring SHM） | open("/dev/usb_hid")+mmap 读 HID SHM，memfd_create 创建 kbd_ring SHM + dev_create 关联到 /dev/kbd |
| terminal | 动态 | 0 | VT100 终端 + 输入分发 + 显示渲染 | open("/dev/kbd")+mmap 读 kbd SHM |
| fs_driver | 动态 | 0 | FAT32 文件系统 | memfd_create 创建 FS SHM，SCM_RIGHTS 传 fd |
| shell | 动态 | 0 | 命令行交互 | SCM_RIGHTS 接收 FS SHM fd |

设备注册/发现：`device_register(dev_type)` 注册 + `open("/dev/xxx")` 打开设备节点，设备类型定义在 common/dev.h（DEV_KBD=2, DEV_KMS=3, DEV_BLOCK=4, DEV_TERMINAL=5, DEV_SERIAL=6, DEV_PTMX=7, DEV_PTS_SLAVE=8）。

### SHM IPC 拓扑

所有驱动间 IPC 统一使用共享内存，对齐 Linux memfd + 设备 mmap 模型。数据结构定义在 common/shm.h。

| SHM Region | 创建者 | 大小 | 接入方式 | 内容 |
|------------|--------|------|---------|------|
| USB HID SHM | 内核 xhci_init（shm_create_internal + devtmpfs_create） | 1 页 | kbd_driver: `open("/dev/usb_hid")` + `mmap` | usb_hid_shm_header + 4 sub-rings（keyboard/mouse/gamepad/touchpad） |
| KBD SHM | kbd_driver（memfd_create + ftruncate + dev_create） | 1 页 | terminal: `open("/dev/kbd")` + `mmap` | driver_shm_header + kbd_ring(8 slots) + sleeping flags |
| Disk SHM | 内核 AHCI | — | fs_driver | disk 请求/响应（同步 syscall 模式） |
| FS SHM | fs_driver（memfd_create + ftruncate） | 4 页 | shell: SCM_RIGHTS 传 fd | fs_shm_header + fs_req_shm + fs_resp_shm |

Sleeping flag 协议和驱动工作流详见 [driver_workflow.md](driver_workflow.md)。

### 中断分发

`trap_dispatch`（kernel/trap.c）对硬件 IRQ 的处理：

1. 若 `irq_owner[irq] >= 0`：向绑定进程的 recv 队列入 RECV_IRQ 消息，唤醒（如果 WAIT_RECV），发送 EOI，返回
2. 否则查 `irq_handlers[]` 注册表（如 timer_handler）

`irq_owner[]` 数组在 `isr_init()` 中初始化为 -1，`sys_irq_bind` 时写入当前进程 PID（atomic RELEASE）。

### 各驱动工作流摘要

**kbd_driver**（driver/kbd_driver.cc）— RECV_NOTIFY 驱动：

1. `device_register(DEV_KBD)` → `open("/dev/kbd")` 可发现
2. `open("/dev/usb_hid")` + `mmap(...)` → 映射内核 HID SHM → `get_keycode_init()`
3. 主循环：`sys_recv` → RECV_NOTIFY → `get_keycode()` 循环 → `kbd_push()` → 写 kbd_ring → 检查 consumer_sleeping → notify terminal
4. RECV_REQ：处理 KBD_IOCTL_BIND/UNBIND（首次 bind 时 memfd_create + ftruncate 创建 kbd_ring SHM，dev_create("/dev/kbd", DEV_KBD, &kbd_ops, shm_fd) 关联 SHM）

**terminal**（driver/terminal.cc）— pipe + SHM 驱动：

1. `open("/dev/kbd")` → `ioctl(fd, KBD_IOCTL_BIND)` → `mmap(fd)` 从 inode->shm 映射 kbd SHM
2. `display_client_init()` → `open("/dev/kms")` + `ioctl(KMS_IOCTL_CREATE_BUF)` + `mmap` back buffer
3. 键盘输入：读 kbd_ring → `sys_write(1, ...)` 写 stdin pipe → shell 的 fd 0 收到
4. Shell 输出：`sys_read(0, ...)` 从 stdout pipe 读 → VT100 状态机 → 渲染到 back buffer → `display_client_flush()` → `ioctl(KMS_IOCTL_FLIP)`

**fs_driver**（driver/fs_driver.cc）— SHM + notify 驱动：

1. `memfd_create("fs_shm")` + `ftruncate(fd, 4*4096)` 创建 FS SHM，SCM_RIGHTS 传 fd 给 shell
2. 主循环 `sys_recv` 等待 RECV_NOTIFY（shell 通知）
3. 读 fs_req_shm → 执行文件操作 → 写 fs_resp_shm → 检查 client_sleeping → notify shell
4. 与磁盘通信：写 disk_req_shm → notify → recv 等响应 → 读 disk_resp_shm

**shell**（shell/shell.cc）— pipe + SHM 驱动：

1. SCM_RIGHTS 接收 FS SHM fd
2. 键盘输入：`sys_read(0, ...)` 从 stdin pipe 读
3. 输出：printf → sys_write(1, ...) → stdout pipe → terminal
4. 文件操作：写 fs_req_shm → notify fs_driver → recv 等响应 → 读 fs_resp_shm

### 启动时序

init/init.c : main：

1. `spawn_service("/driver/kbd.dev")` → kbd_driver
2. `wait_dev_ready("/dev/kbd")` — 轮询直到 /dev/kbd 可用
3. `spawn_service("/usr/bin/terminal")` → terminal
4. `waitpid(-1, &status, 0)` — 孤儿进程回收循环

`/dev/kms` 由内核在 vfs_init 时注册，不需要 init spawn KMS 进程。

### 与其他模块的关系

- **IPC**：所有驱动间通信通过 SHM + notify + recv。详见 [ipc.md](ipc.md)
- **驱动工作流**：sleeping flag 协议、轮询窗口、各驱动详细流程。详见 [driver_workflow.md](driver_workflow.md)
- **xHCI**：内核 USB HID SHM 通知 kbd_driver。详见 [xhci.md](xhci.md)
- **KMS**：内核态显示，terminal 通过 /dev/kms 交互。详见 [kms.md](kms.md)
- **键盘**：kbd_driver 3 层协议。详见 [kbd.md](kbd.md)
- **VFS**：设备节点 /dev/kbd /dev/kms 通过 devtmpfs 注册。详见 [vfs.md](vfs.md)

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| disk_driver 移除 | fs_driver 直接通过 sys_block_async 访问内核 AHCI，移除用户态 disk_driver 进程 | 中 |
| 驱动崩溃自动重连 | init 检测驱动崩溃 → respawn → 客户端 close 旧 fd → 循环 open 重连 | 低 |
