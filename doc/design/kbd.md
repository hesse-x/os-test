# USB 键盘驱动

## 当前架构设计

PS/2 路径已完全移除。键盘输入链路：USB 键盘 → xHCI Transfer Ring → xHCI ISR → USB HID SHM → kbd_driver → kbd_ring SHM → terminal。

### 设计决策

| # | 决策 | 选择 | 理由 |
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
| 13 | 热插拔 | 暂不支持，xhci_init 一步枚举 | ISR 中做枚举需异步状态机或内核工作队列（不存在） |
| 14 | get_keycode 返回格式 | `key_event {key, pressed, modifiers}` | kbd_push 层零改动，HID keycode→input_key 映射在 get_keycode 内部完成 |
| 15 | PS/2 路径 | 完全移除 | 目标是迁移到 USB，保留 PS/2 增加维护负担 |
| 16 | EP 方向 | EP 1-IN (Interrupt IN)，DCI=3 | USB 键盘通过 Interrupt IN 端点向主机发送数据 |

### 数据流

```
USB 键盘 → xHCI Transfer Ring → xHCI ISR (内核)
  → 写 HID 报告到 SHM keyboard sub-ring
  → isr_lookup_driver(DEV_KBD) 找 kbd_driver PID
  → wake_process()

kbd_driver (用户态, IOPL=0):
  sys_shm_attach(USB_HID_SHM_ID, 1) → get_keycode_init()
  sys_recv → RECV_NOTIFY
  → get_keycode() 读 SHM ring + 对比 last_report + HID keycode→input_key → key_event
  → kbd_push() → kbd_ring SHM → notify terminal

terminal: 读 kbd_ring → VT100 → display back buffer → ioctl(FLIP)
```

### USB HID SHM 页布局（1 页 4KB）

内核预分配 1 页物理内存，通过 `register_kernel_shm(USB_HID_SHM_ID=1, phys, 1)` 注册，用户态通过 `sys_shm_attach(1, &addr, 1)` attach。

**Header**（offset 0, 32 字节）— common/shm.h : usb_hid_shm_header_t：
  magic : uint32_t — 0x55484944 ("UHID")
  version : uint32_t — 1
  rings[4] : struct { head/tail/capacity/reserved : uint32_t } — 4 个子 ring 的 head/tail/capacity

**Sub-ring 槽位格式**（10 字节/槽位）— common/shm.h : usb_hid_slot_t：
  type : uint8_t — 1=HID keyboard, 2=HID mouse, 3=HID gamepad, 4=HID touchpad
  len : uint8_t — data 中有效字节长度
  data[8] : uint8_t — 原始数据（HID keyboard: 8B Boot Protocol 报告）

**Ring 分区**：

| 子 ring | 设备类型 | Offset | 容量 | interrupter |
|---------|----------|--------|------|-------------|
| 0 | 键盘 | 32 | 100 | 0 |
| 1 | 鼠标 | 1032 | 100 | 1 (spare) |
| 2 | 手柄 | 2032 | 100 | — |
| 3 | 触控板 | 3032 | 100 | — |

常量：`HID_SUBRING_KBD_OFFSET=32`, `HID_SUBRING_CAPACITY=100`, `HID_SLOT_SIZE=10`

**Ring 操作**：

- 生产者（内核 ISR）：`head = (head + 1) % capacity`，写 slot[head]
- 消费者（用户态 get_keycode）：读 slot[tail]，`tail = (tail + 1) % capacity`
- 空: `head == tail`，满: `(head + 1) % capacity == tail`
- ISR 是唯一生产者，用户态是唯一消费者，无需锁；head/tail 用 `__atomic_store_n` / `__atomic_load_n` 确保 SMP 可见性

### 键盘驱动 3 层协议

**key_event 中间表示**（common/input.h : key_event）：
  key : uint16_t — input_key 枚举值（与 Linux input-event-codes.h 对齐）
  pressed : uint8_t — 1=按下, 0=释放
  modifiers : uint8_t — MOD_SHIFT/MOD_CTRL/MOD_ALT/MOD_CAPS 位图

**3 层函数**：

1. **获取层**（内核侧）：xHCI ISR 读 HID DMA buffer → 写 USB HID SHM sub-ring → wake kbd_driver
2. **翻译层**（用户态）：user/lib/usb_kbd.cc : `get_keycode()` — 读 SHM slot + 对比 last_report[8] 检测 press/release + HID keycode→input_key 映射（`hid_to_input_key[256]` 查找表） + modifier bitmap→MOD_* flags
3. **推送层**（用户态）：kbd_push() — key_event → kbd_ring SHM → 检查 consumer_sleeping → notify terminal

初始化：user/lib/usb_kbd.cc : `get_keycode_init(shm_addr)` 设置 SHM 地址和 last_report 状态

### bind/unbind ioctl 协议

键盘消费者通过 `ioctl(fd, KBD_IOCTL_BIND/KBD_IOCTL_UNBIND)` 主动订阅/取消订阅。常量定义在 common/ioctl.h。

- `KBD_IOCTL_BIND` — `_IOWR('K', 0x11, char[8])`，8B 结构体
- `KBD_IOCTL_UNBIND` — `_IO('K', 0x12)`

bind 语义：
- 首次 bind：kbd_driver 调 `sys_shm_create(4096)` 创建 KBD SHM，初始化 ring + sleeping flags
- 重复 bind 同一 PID：幂等，直接返回成功
- bind 不同 PID：返回 -EBUSY（单消费者模型，替换需先 unbind）
- unbind：清 consumer_pid，SHM 由 proc_reap 自动回收

terminal 端绑定流程：
1. `open("/dev/kbd")` → fd
2. `ioctl(fd, KBD_IOCTL_BIND, &arg)` → 绑定
3. `mmap(NULL, 0, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)` → 映射 kbd SHM

### 扩展键 ESC 序列映射

| 按键 | ESC 序列 |
|------|----------|
| KEY_UP / DOWN / RIGHT / LEFT | `\033[A` / `B` / `C` / `D` |
| KEY_HOME / KEY_END | `\033[H` / `F` |
| KEY_INSERT / KEY_DELETE | `\033[2~` / `3~` |
| KEY_PAGEUP / PAGEDOWN | `\033[5~` / `6~` |
| KEY_F1-F4 | `\033OP` - `\033OS` |
| KEY_F5-F12 | `\033[15~` - `\033[24~` |

### xHCI 键盘枚举

kernel/xhci.c : xhci_init_keyboard 完整流程：

1. 分配页：USB HID SHM + HID DMA buffer + EP0 ring + EP1-IN transfer ring + device context + control DMA buffer
2. 初始化 USB HID SHM header（magic, version, 4 sub-rings），`register_kernel_shm(USB_HID_SHM_ID, kshm)`
3. Port Discovery → Port Reset → Enable Slot → Address Device（Slot Context + EP0 Control）
4. Get Descriptor（Device Descriptor + Configuration Descriptor）→ 解析 EP1-IN max_packet_size/interval
5. Set Protocol(Boot)：bmRequestType=0x21, bRequest=0x0B, wValue=0
6. Set Configuration(config value 1)
7. Configure Endpoint（EP1-IN Interrupt, DCI=3）
8. 启动 Transfer Ring：Normal TRB（8B HID DMA buffer, IOC=1）→ Ring Doorbell
9. Mask PS/2 键盘 GSI 1：`ioapic_set_irq(1, 33, bsp_apic_id, true)`

### 组件清单

| 文件 | 角色 |
|------|------|
| kernel/xhci.c / kernel/xhci.h | xHCI 控制器驱动 + USB 键盘枚举 + ISR |
| kernel/pci.c | MSI-X 向量分配 + Table/PBA 操作 |
| common/shm.h | usb_hid_shm_header_t + usb_hid_slot_t + USB_HID_SHM_ID 常量 |
| common/input.h | key_event + input_key 枚举 + modifier 常量 |
| common/ioctl.h | KBD_IOCTL_BIND / UNBIND 命令编码 |
| user/lib/usb_kbd.cc | get_keycode / get_keycode_init（HID→key_event 转换） |
| user/include/usb_hid.h | get_keycode / get_keycode_init 声明 |
| driver/kbd_driver.cc | 用户态键盘驱动（recv → get_keycode → kbd_push → notify terminal） |

### 与其他模块的关系

- **xHCI**：ISR 写 USB HID SHM，wake kbd_driver。详见 [xhci.md](xhci.md)
- **Terminal**：kbd_driver 通过 kbd_ring SHM + notify 与 terminal 交互。详见 [terminal.md](terminal.md)
- **IPC/SHM**：USB HID SHM 为内核预分配共享内存，kbd_ring 为用户态动态 SHM。详见 [ipc.md](ipc.md)
- **用户态驱动**：kbd_driver 是 IOPL=0 的用户态驱动进程。详见 [user_driver.md](user_driver.md)

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| `/dev/kbd` fd 统一接口 | 设备发现统一到 fd 模型：open→fd，req(fd,...)/mmap(fd)/poll(fd) 替代 device_lookup+sys_req+shm_attach；底层 sys_req/notify/shm 不变 | 高 |
| 热插拔 | Port Status Change 事件处理 + 内核工作队列；当前枚举在 xhci_init 一步完成 | 中 |
| USB mouse | 第二设备 slot / 第二 endpoint / mouse sub-ring | 低 |
| 驱动崩溃自动重连 | 终端检测 POLLERR → close(fd) → 循环 open 重连；需 Wayland compositor 架构 | 低 |
