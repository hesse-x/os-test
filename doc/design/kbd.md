# USB 键盘迁移设计

PS/2 键盘（IRQ1 + inb 0x60/0x64）→ USB HID Boot Protocol 键盘（xHCI Transfer Ring + SHM ring）。

**已实现。** PS/2 路径已完全移除。

## 设计决策

| # | 问题 | 决策 | 理由 |
|---|------|------|------|
| 1 | 内核 vs 用户空间 | 内核做 HC 初始化 + 设备枚举 + Transfer Ring + ISR；用户态做翻译推送 | USB 设备枚举需要 DMA/MMIO，用户态无法直接访问 |
| 2 | 数据传输接口 | SHM ring（ISR 写原始 HID 报告） | ISR 安全（无阻塞）、零拷贝、与现有 SHM+notify 模式一致 |
| 3 | 内核职责边界 | 内核做完整枚举 + Transfer Ring 配置 | USB 枚举需 DMA/MMIO，拆成 syscall 代理层回报不高 |
| 4 | SHM 数据格式 | 原始字节 + 类型标记 `{type, len, data[8]}` | 内核只搬运数据，语义解析在用户态，符合微内核原则 |
| 5 | Ring 槽位大小 | 固定 10 字节/槽位 | 简化 head/tail 计算，与 HID 8B 报告 + 2B 头部对齐 |
| 6 | 单 ring vs 多 ring | 1 页 4KB 内 4 个子 ring（键盘/鼠标/手柄/触控板） | 不同设备由不同驱动进程消费，独立 head/tail |
| 7 | SHM 创建方 | 内核 xhci_init 预分配 | 内核是 ring 生产者（ISR 写数据），内核创建更自然 |
| 8 | SHM attach 方式 | `sys_shm_attach(shm_id, mode=1)` 内核 SHM 模式 | arg2=0 为原有 PID 模式，arg2=1 为内核 SHM ID 查找，零新增 syscall |
| 9 | get_keycode 位置 | 封装在 `user/lib/usb_kbd.cc` | 用户态做状态→事件转换，HID keycode 映射 |
| 10 | ISR 写入模式 | 无条件写完整 HID 报告，不去重 | 重复报告用户态对比后自然得出 0 事件，内核不做语义判断 |
| 11 | MSI-X vector | 2 个（interrupter 0=keyboard, interrupter 1=spare） | 机制建立即可，不为未存在的设备提前预留 |
| 12 | Transfer Ring 管理 | `xhci_intr` per-interrupter 结构体 | 与多 interrupter 架构对齐，ISR 通过 interrupter index 自然定位 |
| 13 | 热插拔 | 暂不支持，xhci_init 一步枚举 | ISR 中做枚举需异步状态机或内核工作队列（不存在），工作量较大 |
| 14 | get_keycode 返回格式 | `key_event {key, pressed, modifiers}` | kbd_push 层零改动，HID keycode→input_key 映射在 get_keycode 内部完成 |
| 15 | PS/2 路径 | 完全移除 | 目标是迁移到 USB，保留 PS/2 增加维护负担 |
| 16 | ISR→驱动通知 | `dev_table[DEV_KBD].pid` + 内核内部 RECV_NOTIFY + wake_process | 无需新 syscall，kbd_driver 用 device_register 注册 PID，ISR 查 dev_table |
| 17 | EP 方向 | EP 1-IN (Interrupt IN)，DCI=3 | USB 键盘通过 Interrupt IN 端点向主机发送数据，不是 EP 1-Out |
| 18 | USB_HID_SHM_ID | ID=1（非 0） | PID 0 可能有效，用 arg2 mode 参数区分 PID vs 内核 SHM |

## 数据流

```
USB 键盘 → xHCI Transfer Ring → xHCI ISR (内核)
  → 写 HID 报告到 SHM keyboard sub-ring
  → dev_table[DEV_KBD].pid 找 kbd_driver PID
  → 入队 RECV_NOTIFY + wake_process

kbd_driver (用户态, IOPL=0):
  shm_attach_kernel(USB_HID_SHM_ID) → get_keycode_init()
  sys_recv → RECV_NOTIFY
  → get_keycode() 读 SHM ring + 对比 last_report + HID keycode→input_key → key_event
  → kbd_push() → kbd_ring SHM → notify terminal

terminal (不变): 读 kbd_ring → VT100 → display SHM → KMS
```

## USB HID SHM 页布局（1 页 4KB）

内核预分配 1 页物理内存，分为 header + 4 个子 ring。通过 `register_kernel_shm(USB_HID_SHM_ID=1, phys, 1)` 注册，用户态通过 `shm_attach_kernel(1, &addr)` attach。

### Header（offset 0, 32 字节）

```c
struct usb_hid_shm_header {
    uint32_t magic;          // 0x55484944 ("UHID")
    uint32_t version;        // 1
    struct {
        uint32_t head;
        uint32_t tail;
        uint32_t capacity;   // 槽位数量
        uint32_t reserved;
    } rings[4];              // 4 个子 ring 的 head/tail/capacity
};
```

### Sub-ring 槽位格式（10 字节/槽位）

```c
struct usb_hid_slot {
    uint8_t  type;           // 1=HID keyboard, 2=HID mouse, 3=HID gamepad, 4=HID touchpad
    uint8_t  len;            // data 中有效字节长度
    uint8_t  data[8];        // 原始数据（HID keyboard: 8B Boot Protocol 报告）
};
```

### Ring 分区（4KB 页减去 header）

- Header: 32 字节
- 可用空间: 4096 - 32 = 4064 字节
- 每个子 ring: 4064 / 4 = 1016 字节 ≈ 101 槽位 × 10 字节 = 1010 字节
- 实际对齐: 每个子 ring 容量 100 槽位（1000 字节），剩余 16 字节 padding

| 子 ring | 设备类型 | Offset | 容量 | interrupter |
|---------|----------|--------|------|-------------|
| 0 | 键盘 | 32 | 100 | 0 |
| 1 | 鼠标 | 1032 | 100 | 1 (spare) |
| 2 | 手柄 | 2032 | 100 | — |
| 3 | 触控板 | 3032 | 100 | — |

### Ring 操作

- 生产者（内核 ISR）：`head = (head + 1) % capacity`，写 slot[head]
- 消费者（用户态 get_keycode）：读 slot[tail]，`tail = (tail + 1) % capacity`
- 空: `head == tail`
- 满: `(head + 1) % capacity == tail`

内核 ISR 写 SHM 时不需要锁（ISR 是唯一生产者，用户态是唯一消费者）。head/tail 用 `__atomic_store_n` / `__atomic_load_n` 确保 SMP 可见性。

## 内核侧改动

### xhci.cc

完整 USB 键盘枚举 + Transfer Ring + ISR。详见 `kernel/xhci.cc`。

关键步骤：
- Port Discovery → Port Reset → Enable Slot → Address Device → Get Descriptor → Set Protocol(Boot) → Configure Endpoint (EP1-IN DCI=3) → Transfer Ring start → Ring Doorbell
- ISR 处理 Transfer Event: 读 HID DMA buffer → 写 SHM sub-ring → replenish TRB → notify kbd_driver
- Masks PS/2 GSI 1 via `ioapic_set_irq(1, 33, bsp_apic_id, true)`
- `register_kernel_shm(USB_HID_SHM_ID=1, usb_hid_shm_phys, 1)`

### shm.h 新增

- `usb_hid_shm_header` + `usb_hid_slot` 结构体定义
- `USB_HID_SHM_ID=1` 常量
- 子 ring offset/capacity/slot size 常量

### sys_shm_attach 扩展

- arg2 (mode): 0=原有 PID attach, 1=内核 SHM ID attach
- `kernel_shm_table[MAX_KERNEL_SHM]` 存储内核预分配 SHM
- `register_kernel_shm(id, phys, npages)` 供 xhci_init 调用

### I/O APIC 变更

- GSI 1（PS/2 键盘）被 mask

### trap.h 新增

- `register_kernel_shm()` 声明
- `lookup_dev()` 声明（ISR 查 dev_table）

## 用户态改动

### kbd_driver.cc 重构

移除：
- `sys_irq_bind(33)` → 改为 RECV_NOTIFY 驱动
- `sys_ioperm(0x60/0x64)` → 移除（IOPL=0）
- `kbd_irq_acquire` → 移除
- `kbd_translate` → 移除
- PS/2 scancode 映射表 → 移除
- `kbd_state` 结构体 → 移除

新增：
- `shm_attach_kernel(USB_HID_SHM_ID, &addr)` → attach 内核 HID SHM
- `get_keycode_init(addr)` → 初始化 get_keycode 内部状态
- 主循环：RECV_NOTIFY → `get_keycode()` 循环 → `kbd_push()`

不变：
- `device_register(DEV_KBD)`
- BIND/UNBIND handler（创建 kbd_ring SHM）
- `kbd_push` 层

### user/lib/usb_kbd.cc（新增）

`get_keycode` 实现：
- 读 SHM sub-ring slot[tail]，tail++ (atomic)
- 解析 HID Boot Protocol 报告: data[0]=modifier, data[2-7]=keycodes
- 对比 last_report[8] 检测 press/release
- HID keycode → input_key 映射表 (256 entries)
- Modifier bitmap → MOD_CTRL/MOD_SHIFT/MOD_ALT/MOD_CAPS

`get_keycode_init(shm_addr)` 初始化 SHM 地址和 last_report。

### user/include/usb_hid.h（新增）

引用 `common/shm.h` 的结构体定义，声明 `get_keycode()` 和 `get_keycode_init()`。

### IOPL 变更

kbd_driver 不再需要 IOPL=3（不再访问 I/O 端口）。改为 IOPL=0（普通用户进程），通过 SHM 接收数据。

## xHCI Transfer Ring + HID Boot Protocol 配置细节

### Address Device Command

Input Context（Slot Context + EP 0 Context）:
- Slot Context: route_string=0, ctx_entries=1, interrupter_target=0, speed=Full(1), slot_state=Default
- EP 0 Context: EP type=Control(4), max_packet_size=64, interval=0, EP state=Running, TR dequeue ptr=ep0_ring_phys|DCS

### Configure Endpoint Command

Input Context（Slot Context + EP 1-IN Context, DCI=3）:
- Slot Context: ctx_entries=2, slot_state=Configured
- EP 3 Context: EP type=Interrupt-IN(7), max_packet_size=8, interval=4, EP state=Running, TR dequeue ptr=xhci_intrs[0].ring_phys|DCS
- Doorbell target=3 (DCI for EP1-IN)

### Transfer Ring

- 1 页（256 TRB），index 255 = Link TRB（TC=1）
- Normal TRB: data pointer = HID DMA buffer（8 字节），TRB type=Normal, IOC=1
- ISR 收到 Transfer Event 后：读 DMA buffer → 写 SHM → replenish TRB → ring Doorbell

### HID Boot Protocol

- Set Protocol(Boot)：bmRequestType=0x21, bRequest=0x0B, wValue=0, wLength=0
- 报告格式: byte 0 = modifier bitmap, byte 1 = reserved(0), byte 2-7 = 当前按下的 keycode（最多 6 个，0 = 无键）

## 暂不支持：热插拔

热插拔需要以下机制，当前内核不支持：

1. Port Status Change 事件处理（ISR 中检测设备插入/移除）
2. 内核工作队列 / 内核线程（ISR 中不能阻塞 poll_cmd_complete，需要异步状态机）
3. 设备移除：Disable Slot Command + Transfer Ring 释放 + SHM ring 清空
4. 设备插入：Port Reset → Enable Slot → Address Device → Configure Endpoint（ISR 中多步 Command Ring 操作）

待内核工作队列机制建立后再实现。当前键盘枚举在 xhci_init 中一步完成，QEMU 模拟的 USB 键盘在启动时已连接。

---

## Phase 2：设备发现 `/dev/kbd` + fd 统一接口

### 动机

当前设备发现通过 `device_lookup(DEV_KBD)` 查内核 dev_table 返回 PID，通信通过 `sys_req(pid, ...)` 和 `shm_attach(pid)` 完成。这存在三个问题：

1. **API 不统一**：设备发现用 lookup，控制用 req，数据用 shm_attach，三个不同入口
2. **非 POSIX**：外部程序需要理解 PID 概念而非标准 fd
3. **驱动进程号耦合**：虽然不是静态 PID 常量了，但仍需理解「对端是一个进程」

目标：**统一到 fd 模型**——open 返回 fd，用 req/mmap/poll 操作 fd，底层走 sys_req/notify/shm 不变。

### 设计决策

| # | 问题 | 决策 | 理由 |
|---|------|------|------|
| 1 | 设备节点来源 | 内核路径识别，不依赖 fs_driver | open 时路径以 `/dev/` 开头就走内核分配 FD_DEV |
| 2 | dev_table 存留 | **保留**，作为内核内部设备注册表 | open("/dev/kbd") 时内核查 dev_table 找到驱动 PID 绑定到 fd |
| 3 | FD_DEV 内核数据结构 | fd_table 新增 type=FD_DEV，含 target_pid | 复用现有内核 fd_table 框架，改动最小 |
| 4 | 控制通道 | `req(fd, request, reply)` — libc 包装 | 内部做 fd→PID 翻译后走 sys_req，底层零改动 |
| 5 | 数据通道 | `mmap(fd, size, prot, flags, offset)` | 内核根据 fd 定位 target_pid，映射驱动 SHM 物理页到本进程 |
| 6 | 事件等待 | `poll(fd, events)` | 封装 sys_recv 或检查 ring buffer head/tail |
| 7 | 热插拔/驱动切换 | **上层应用（合成器）主动重连** | 与 Linux 一致——compositor 负责管理设备生命周期，驱动崩溃后 init 重新 spawn 新驱动，合成器 `close` 旧 fd → 循环 `open` → `req` bind → `mmap` |
| 8 | SHM 归属 | **进程级**（现有 shm_regions），驱动进程退出时 SHM 释放 | 合成器重连后新驱动创建新 SHM，重新 mmap。不做内核永久 SHM |
| 9 | 是否新增 sys_open | **否**——扩展现有 open 路径 | libc open 检查路径前缀 `/dev/`，是则调用内核新 syscall 或扩展现有路径 |
| 10 | 是否保留 sys_req/sys_resp 接口 | **保留**——libc req(fd, ...) 内部分发到 sys_req | 56B 内联零分配对控制信令（bind/unbind）足够高效 |

### fd 拓扑

```c
// 内核 fd_table 新类型
#define FD_DEV     3   // 新增

struct file {
    int type;           // FD_NONE / FD_PIPE / FD_DEV
    int flags;
    // FD_DEV 专用
    pid_t target_pid;    // 驱动进程 PID
    // FD_PIPE 专用（不变）
    struct pipe *pipe;
};
```

```
终端进程 fd_table:
  fd 0 = FD_PIPE (stdin)
  fd 1 = FD_PIPE (stdout)
  fd 2 = FD_DEV  → target_pid = kbd_pid    ← 新增
  fd 3 = FD_DEV  → target_pid = fs_pid     ← 新增
  fd 4 = FD_DEV  → target_pid = kms_pid    ← 新增
```

### 用户态 API

```c
// — 设备发现 —
int open(const char *path, int flags);          // "/dev/kbd" → 返回 FD_DEV fd

// — 控制通道（底层 sys_req） —
int req(int fd, void *request, void *reply);    // 取代 sys_req(pid, ...)
int notify(int fd);                             // 取代 sys_notify(pid)

// — 数据通道（底层 shm） —
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
                                                // mmap dev_fd 取驱动 SHM

// — 事件等待 —
int poll(struct pollfd *fds, nfds_t nfds, int timeout);
                                                // 取代 sys_recv
```

### 数据流

```
终端（合成器）:
  fd = open("/dev/kbd")          → fd_table[2] = FD_DEV, target_pid = kbd_pid

  req(fd, &BIND_REQ, &resp)      → libc: fd[2].target_pid → sys_req(kbd_pid, ...)
                                      内核: RECV_REQ → kbd_driver → kbd_driver 创建 SHM →
                                      sys_resp → 返回

  ring = mmap(fd, 4096, ...)     → 内核: fd[2].target_pid → 找到 kbd_driver 的 SHM 物理页
                                      映射到终端地址空间 → 返回虚拟地址

  poll(fd, POLLIN)               → 等按键事件
```

### 驱动切换流程

```c
// init 检测 kbd_driver 崩溃，重新 spawn
// 新 kbd_driver 启动 → shm_create → device_register(DEV_KBD) → 进入事件循环

// 终端检测到 fd 失效
poll(fd, POLLIN) 返回 POLLERR 或 req(fd, ...) 返回 ESRCH
  → 终端检测到驱动崩溃
  → close(fd)

  // 重连循环
  while ((fd = open("/dev/kbd")) < 0) {
      poll(NULL, 0, 100);   // 等待 init 重新 spawn kbd_driver
  }
  req(fd, &BIND_REQ, &resp);
  ring = mmap(fd, 4096, PROT_READ, MAP_SHARED, 0);
  // 继续消费
```

支持 Wayland 多进程拓扑后，合成器解耦出来承担设备管理职责：

```
kbd_driver → [合成器 (Compositor)] → 各 Wayland 客户端
                  ↑
            终端/合成器: 管理 kbd fd
            驱动切换时合成器重连，客户端不感知
```

### 和 Linux 的对齐

| 层面 | Linux | 本方案 | 对齐？ |
|------|-------|--------|--------|
| 设备发现 | open("/dev/input/eventX") | open("/dev/kbd") | ✅ 接口对齐 |
| 控制通道 | ioctl(fd, EVIOCG.../EVIOCS...) | req(fd, ...) | ⭕ 实现不同，语义等价 |
| 数据通道 | mmap/mmap+read | mmap(fd, ...) | ✅ |
| 事件等待 | poll(fd, POLLIN) | poll(fd, POLLIN) | ✅ |
| 驱动切换 | libinput 监听 udev → compositor 重连 | 合成器检测 POLLERR → 循环重连 | ✅ |
| SHM 生命周期 | evdev buffer 归内核管 | SHM 归驱动进程管，驱动死则 SHM 释放 | ⭕ 这里不同，但重连机制弥补 |

### 实施步骤（建议顺序）

1. **内核新增 FD_DEV 类型** — `kernel/proc.h` `struct file` 加 type 和 target_pid
2. **扩展 open 路径** — libc open 识别 `/dev/` 前缀 → 内核分配 FD_DEV fd
3. **libc 包装 req/notify 支持 fd** — fd→PID 翻译 → sys_req/sys_notify
4. **libc 包装 mmap 支持 FD_DEV** — 内核映射 target_pid 的 SHM 物理页
5. **libc 包装 poll 支持 FD_DEV** — 封装 sys_recv timeout 或检查 ring buffer
6. **迁移 kbd_driver/terminal 到 fd API** — 替换 device_lookup + sys_req + shm_attach
7. **驱动切换测试** — 手动 kill kbd_driver，验证终端自动重连
