# 用户态驱动

> **已实现**。6 个用户态 ELF 驱动/服务进程，通过动态 SHM + sys_recv/sys_rpc/sys_notify 协作。内核仅提供最小机制（调度、内存、中断分发、syscall）。

## 驱动架构

| ELF | PID | IOPL | 功能 | SHM 角色 |
|-----|-----|------|------|---------|
| disk_driver | 2 | 3 | ATA PIO 磁盘读写 | 创建 5 页 Disk SHM |
| kbd_driver | 3 | 3 | 键盘输入（3 层协议） | 创建 1 页 KBD SHM |
| kms_driver | 4 | 0 | Framebuffer 渲染 | attach KBD SHM |
| terminal | 5 | 0 | VT100 终端 + 输入分发 | attach KBD SHM |
| shell | 6 | 0 | 命令行交互 | attach FS SHM |
| fs_driver | 7 | 0 | FAT32 文件系统 | 创建 4 页 FS SHM，attach Disk SHM |

IOPL=3 的驱动（disk/kbd）可直接 `in`/`out`，零 syscall 开销。IOPL=0 的进程执行 I/O 指令触发 #GP。

设备注册/发现：`sys_load_dev(pid, dev_type)` 注册 + `sys_lookup_dev(dev_type)` 查询，设备类型定义在 `common/dev.h`（DEV_DISK/DEV_KBD/DEV_KMS/DEV_FS/DEV_TERMINAL）。

## SHM IPC 拓扑

所有驱动间 IPC 统一使用动态共享内存（`sys_shm_create`/`sys_shm_attach`），数据结构定义在 `common/shm.h`。

| SHM Region | 创建者 | 大小 | 接入者 | 内容 |
|------------|--------|------|--------|------|
| KBD SHM | kbd_driver | 1 页 | kms_driver, terminal | driver_shm_header + kbd_ring(8 slots) + kms_ring(240 slots) |
| Disk SHM | disk_driver | 5 页 | fs_driver | disk_shm_header + disk_req_shm(2 页) + disk_resp_shm(2 页) |
| FS SHM | fs_driver | 4 页 | shell | fs_shm_header + fs_req_shm(1 页) + fs_resp_shm(2 页) |

Sleeping flag 协议和轮询窗口模式详见 [driver_workflow.md](driver_workflow.md)。

## 中断分发

`trap_dispatch`（`kernel/trap.cc`）对硬件 IRQ 的处理：

1. 若 `irq_owner[irq] >= 0`：向绑定进程的 recv 队列入 RECV_IRQ 消息，唤醒（如果 WAIT_RECV），发送 EOI，返回
2. 否则查 `irq_handlers[]` 注册表（如 timer_handler）

`irq_owner[]` 数组在 `isr_init()` 中初始化为 -1，`sys_irq_bind` 时写入当前进程 PID（atomic RELEASE）。

## 驱动工作流

### disk_driver（driver/disk_driver.cc）

创建 5 页 Disk SHM，主循环 `sys_recv` 等待请求：

1. 收到 RECV_NOTIFY（fs_driver 通知）：读 `disk_req_shm`（cmd/lba/count/data）
2. 执行 ATA PIO LBA28 操作（IOPL=3，从盘 0xF0）
3. 写 `disk_resp_shm`（status/count/data）
4. 检查 `fs_driver_sleeping`，若为 1 则 `sys_notify(fs_driver_pid)`

Sleeping flag 协议防止 lost-wakeup：fs_driver 写完请求后设 sleeping flag，disk_driver 处理完检查 flag 决定是否 notify。

### kbd_driver（driver/kbd_driver.cc）

3 层架构 + bind/unbind RPC 协议，详见下方 [键盘驱动 3 层协议](#键盘驱动-3-层协议)。

主循环通过 `sys_recv` 多路复用 RECV_IRQ 和 RECV_RPC 事件：

- RECV_IRQ：调用 3 层协议处理按键，写 kbd_ring，检查 consumer_sleeping 后 notify terminal
- RECV_RPC：处理 bind/unbind 请求，通过 sys_reply 回复

### kms_driver（driver/kms_driver.cc）

attach KBD SHM，60fps 帧调度模式：

1. drain 整个 kms_ring（240 slots）
2. 逐条渲染（8x16 字体，fg/bg 颜色）
3. 设置 `kms_sleeping=1` → double-check kms_ring → `sys_recv(&msg, 16)` 等待 16ms 定时
4. 被唤醒（notify 或超时）→ 设 `kms_sleeping=0` → 回到步骤 1

Framebuffer 直写虚拟地址 0x700000（内核 `process_create_elf` 时 map_fb=true 映射）。

### terminal（driver/terminal.cc）

attach KBD SHM，连接 shell 的 pipe 对（stdin + stdout）：

- **键盘输入**：读 kbd_ring → `sys_write(1, ...)` 写 stdin pipe → shell 的 fd 0 收到
- **shell 输出**：`sys_read(0, ...)` 从 stdout pipe 读（fd 0 = O_RDONLY|O_NONBLOCK）→ VT100 状态机 → cell 缓冲区 → `flush_dirty_cells` 写 kms_ring → 检查 kms_sleeping 后 `sys_notify(kms_pid)`
- 启动时通过 RPC bind 键盘：`sys_rpc(kbd_pid, &bind_req, &bind_reply)`

### fs_driver（driver/fs_driver.cc）

创建 4 页 FS SHM，attach Disk SHM，FAT32 文件系统服务：

- 主循环 `sys_recv` 等待 RECV_NOTIFY（shell 通知）
- 收到通知后读 `fs_req_shm`，执行 readdir/open/read/close/raw_read/touch/mkdir
- 与 disk_driver 通信：写 `disk_req_shm` → `sys_notify(disk_pid)` → `sys_recv` 等待响应 → 读 `disk_resp_shm`
- 处理完写 `fs_resp_shm`，检查 `client_sleeping`，notify shell

### shell（shell/shell.cc）

attach FS SHM，通过 fd 0/1 pipe 与 terminal 通信：

- 键盘输入：`sys_read(0, ...)` 从 stdin pipe 读
- 输出：printf → sys_write(1, ...) → stdout pipe → terminal
- 文件操作：写 `fs_req_shm` → notify fs_driver → `sys_recv` 等响应 → 读 `fs_resp_shm`
- 命令：ls/cat/cd/pwd/touch/mkdir/run/malloc/free/mtest/r/h

## 键盘驱动 3 层协议

### key_event 中间表示

```c
// common/input.h
#define MOD_SHIFT  0x01
#define MOD_CTRL   0x02
#define MOD_ALT    0x04
#define MOD_CAPS   0x08

struct key_event {
    uint16_t key;       // input_key 枚举值
    uint8_t  pressed;   // 1=按下, 0=释放
    uint8_t  modifiers; // 修饰键状态
};
```

### input_key 统一枚举

```c
// common/input.h
enum input_key {
    KEY_RESERVED = 0,
    KEY_ESC = 1,
    KEY_1 = 2,  KEY_2 = 3,  ...  KEY_0 = 11,
    KEY_MINUS = 12, KEY_EQUAL = 13,
    KEY_BACKSPACE = 14, KEY_TAB = 15,
    KEY_Q = 16, ... KEY_P = 25,
    KEY_LEFTBRACE = 26, KEY_RIGHTBRACE = 27, KEY_ENTER = 28,
    KEY_LEFTCTRL = 29,
    KEY_A = 30, ... KEY_L = 38,
    KEY_SEMICOLON = 39, KEY_APOSTROPHE = 40, KEY_GRAVE = 41,
    KEY_LEFTSHIFT = 42, KEY_BACKSLASH = 43,
    KEY_Z = 44, ... KEY_SLASH = 53,
    KEY_RIGHTSHIFT = 54, KEY_LEFTALT = 56, KEY_SPACE = 57,
    KEY_CAPSLOCK = 58,
    KEY_F1 = 59, ... KEY_F10 = 68, KEY_F11 = 87, KEY_F12 = 88,
    KEY_NUMLOCK = 69, KEY_SCROLLLOCK = 70,
    // 扩展键（0xE0 前缀）
    KEY_HOME = 102, KEY_UP = 103, KEY_PAGEUP = 104,
    KEY_LEFT = 105, KEY_RIGHT = 106,
    KEY_END = 107, KEY_DOWN = 108, KEY_PAGEDOWN = 109,
    KEY_INSERT = 110, KEY_DELETE = 111,
};
```

编号与 Linux `input-event-codes.h` 对齐。未来 BTN_* 从 0x100 起。

### 3 层函数

```c
// 获取层：从硬件读扫描码
int kbd_irq_acquire(uint8_t *scancodes, int max);

// 翻译层：扫描码 → key_event
int kbd_translate(uint8_t scancode, struct kbd_state *state, struct key_event *ev);

// 推送层：key_event → kbd_ring
void kbd_push(struct key_event *ev, kbd_ring *ring, pid_t consumer_pid);
```

### 扩展键 ESC 序列映射

| 按键 | ESC 序列 |
|------|----------|
| KEY_UP | `\033[A` |
| KEY_DOWN | `\033[B` |
| KEY_RIGHT | `\033[C` |
| KEY_LEFT | `\033[D` |
| KEY_HOME | `\033[H` |
| KEY_END | `\033[F` |
| KEY_INSERT | `\033[2~` |
| KEY_DELETE | `\033[3~` |
| KEY_PAGEUP | `\033[5~` |
| KEY_PAGEDOWN | `\033[6~` |
| KEY_F1 | `\033OP` |
| KEY_F2 | `\033OQ` |
| KEY_F3 | `\033OR` |
| KEY_F4 | `\033OS` |
| KEY_F5-F12 | `\033[15~` - `\033[24~` |

## bind/unbind RPC 协议

键盘消费者通过 `sys_rpc`/`sys_reply` 主动订阅/取消订阅，SHM 按需创建。

### RPC 请求/回复格式

```c
#define KBD_RPC_BIND    1
#define KBD_RPC_UNBIND  2

struct kbd_rpc_request {
    uint32_t opcode;   // KBD_RPC_BIND / KBD_RPC_UNBIND
    uint32_t pid;      // 请求者 PID（冗余校验）
    uint8_t  reserved[48];
};

struct kbd_rpc_reply {
    int32_t  result;   // 0=成功, -EBUSY=已有其他消费者
    uint8_t  reserved[52];
};
```

### bind 语义

- 首次 bind：kbd_driver 调 `sys_shm_create(4096)` 创建 KBD SHM，初始化 ring + sleeping flags
- 重复 bind 同一 PID：幂等，直接返回成功
- bind 不同 PID：返回 -EBUSY（单消费者模型，替换需先 unbind）
- unbind：清 consumer_pid，SHM 由 proc_reap 自动回收

### terminal 端绑定流程

```c
pid_t kbd_pid = sys_lookup_dev(DEV_KBD);
kbd_rpc_request req = { .opcode = KBD_RPC_BIND, .pid = sys_getpid() };
kbd_rpc_reply reply;
sys_rpc(kbd_pid, &req, &reply);
void *shm = sys_shm_attach(kbd_pid);
```
