# Serial Console 设计

## 目标

实现标准 16550 UART 串口控制台，支持 RX/TX 双向 I/O，让 tmux 可以通过 QEMU 串口与 OS shell 交互。对齐 Linux serial8250 + agetty 模型。

## 架构概览

```
QEMU stdin/stdout ←→ -serial mon:stdio ←→ COM1 (0x3F8) ←→ 8250 UART
    ↑                                                                   ↓
  tmux send-keys                                               IRQ4 (GSI 4)
    ↓                                                                   ↓
  serial shell                                               kernel ISR
    stdin=FD_SERIAL                                          inb(COM1) → ring buf
    stdout=FD_SERIAL                                         wake waiter
```

与 Linux 对齐的分层：

```
Linux:                          本 OS:
8250_core.c (ISR + inb/outb)   kernel/serial.c (serial_irq_handler + serial_putc)
serial_core.c (uart 框架)       不需要，只有一个串口
tty_buffer (ring buffer)        serial_rx_ring (内核环形缓冲)
n_tty.c (线路规则)              不需要，canonical/raw 由 shell/readline 处理
/dev/ttyS0 (字符设备)           sys_open_dev(DEV_SERIAL) → FD_SERIAL
agetty (打开设备 + exec shell)  init: open_dev → dup2 → spawn shell
```

## 改动清单

### 1. kernel/serial.c — 标准 16550 初始化 + RX + TX 锁

**1.1 标准 UART 初始化（对齐 Linux serial8250_startup）**

```c
#define COM1      0x3F8
#define COM1_IER  0x3F9   // Interrupt Enable Register
#define COM1_IIR  0x3FA   // Interrupt Identification Register
#define COM1_FCR  0x3FA   // FIFO Control Register (same port as IIR, write-only)
#define COM1_LCR  0x3FB   // Line Control Register
#define COM1_MCR  0x3FC   // Modem Control Register
#define COM1_LSR  0x3FD   // Line Status Register

// IER bits
#define IER_RX_ENABLE   0x01   // Enable RX data interrupt
#define IER_TX_ENABLE   0x02   // Enable TX holding register empty interrupt

// LSR bits
#define LSR_DR   0x01   // Data Ready (RX has data)
#define LSR_THRE 0x20   // TX Holding Register Empty

// FCR bits
#define FCR_ENABLE_FIFO  0x01
#define FCR_CLEAR_RX     0x02
#define FCR_CLEAR_TX     0x04
#define FCR_TRIGGER_1    0x00   // FIFO trigger level: 1 byte

void serial_init(void) {
    outb(COM1_IER, 0x00);    // Disable all interrupts
    outb(COM1_LCR, 0x80);    // Enable DLAB
    outb(COM1,     0x01);    // Divisor low byte: 115200 baud
    outb(COM1_IER, 0x00);    // Divisor high byte
    outb(COM1_LCR, 0x03);    // 8N1, disable DLAB
    outb(COM1_FCR, 0xC7);    // Enable FIFO, clear, 14-byte threshold
    outb(COM1_MCR, 0x03);    // DTR + RTS
    // IER RX enable deferred to sys_open_dev(DEV_SERIAL)
}
```

**1.2 TX：加 LSR 等待 + TX 自旋锁（对齐 Linux uart_port->lock）**

```c
static spinlock_t serial_tx_lock;

void serial_putc(char c) {
    spin_lock_irqsave(&serial_tx_lock);
    while (!(inb(COM1_LSR) & LSR_THRE))
        ;
    outb(COM1, c);
    spin_unlock_irqrestore(&serial_tx_lock);
}
```

**1.3 RX：内核环形缓冲 + 自旋锁（对齐 Linux tty_buffer + spinlock）**

```c
#define SERIAL_RX_BUF_SIZE 256

static uint8_t serial_rx_buf[SERIAL_RX_BUF_SIZE];
static uint32_t serial_rx_head = 0;  // ISR write position
static uint32_t serial_rx_tail = 0;  // sys_read read position
static spinlock_t serial_rx_lock;
static pid_t serial_read_waiter = -1;
static int serial_fd_count = 0;       // ref count for open/close
static bool serial_irq_registered = false;
```

**1.4 ISR（对齐 Linux serial8250_interrupt）**

```c
static void serial_irq_handler(trapframe_t *tf) {
    spin_lock_irqsave(&serial_rx_lock);
    // Drain all available bytes from FIFO (Linux: read LSR in loop)
    while (inb(COM1_LSR) & LSR_DR) {
        uint8_t c = inb(COM1);
        uint32_t next = (serial_rx_head + 1) % SERIAL_RX_BUF_SIZE;
        if (next != serial_rx_tail) {  // drop if full
            serial_rx_buf[serial_rx_head] = c;
            serial_rx_head = next;
        }
    }
    // Wake blocked reader (对齐 Linux tty_flip_buffer_push → wait queue wake)
    if (serial_read_waiter >= 0) {
        pid_t waiter = serial_read_waiter;
        serial_read_waiter = -1;
        spin_unlock_irqrestore(&serial_rx_lock);
        // 唤醒逻辑复用 irq_owner 路径：持 scheduler_lock + 设 READY + 入队
        wake_process(waiter);
        return;
    }
    spin_unlock_irqrestore(&serial_rx_lock);
}
```

### 2. kernel/proc.h — 新增 FD_SERIAL

```c
#define FD_SERIAL 6   // 在 FD_SOCKET(5) 之后

// file_t union 无需新增成员：
// FD_SERIAL 不需要 pipe/shm/file_data/sock，
// fd 本身是全局唯一资源（只有一个 COM1），状态由 kernel/serial.c 全局变量管理。
```

### 3. common/dev.h — 新增 DEV_SERIAL

```c
#define DEV_SERIAL   6   // 在 DEV_TERMINAL(5) 之后
```

### 4. kernel/trap.c — syscall 分发扩展

**4.1 sys_open_dev 新增 DEV_SERIAL 分支（对齐 Linux uart_open）**

```c
// 在 sys_open_dev 中，DEV_NONE 检查之后、dev_table 查找之前，加入：
if (dev_type == DEV_SERIAL) {
    // 互斥检查：不能同时被用户态 irq_bind 占用
    if (irq_owner[36] >= 0) return (uint64_t)-EBUSY;

    proc_t *proc = current_proc;
    int fd = -1;
    for (int i = 0; i < MAX_FD; i++) {  // 允许占 fd 0/1（serial shell 需要）
        if (proc->fd_table[i].type == FD_NONE) { fd = i; break; }
    }
    if (fd < 0) return (uint64_t)-EMFILE;

    proc->fd_table[fd].type = FD_SERIAL;
    proc->fd_table[fd].flags = O_RDWR;

    serial_fd_count++;
    if (!serial_irq_registered) {
        register_irq(36, serial_irq_handler);
        // Unmask GSI 4 (COM1 IRQ4) in I/O APIC
        uint32_t bsp_apic_id = lapic_read(LAPIC_ID) >> 24;
        ioapic_set_irq(4, 36, bsp_apic_id, false);  // edge-triggered
        // Enable RX interrupt in UART IER
        outb(COM1_IER, IER_RX_ENABLE);
        serial_irq_registered = true;
    }

    return (uint64_t)fd;
}
```

**4.2 sys_write 新增 FD_SERIAL 分支（对齐 Linux uart_write）**

在 FD_FILE 和 FD_SOCKET 分支之间加入：

```c
    // ===== FD_SERIAL: write to UART TX =====
    if (proc->fd_table[fd].type == FD_SERIAL) {
        if (!(proc->fd_table[fd].flags & (O_WRONLY | O_RDWR)))
            return (uint64_t)-EINVAL;
        if (!buf) return (uint64_t)-EFAULT;
        // serial_putc 内部有 tx_lock + LSR 等待
        for (size_t i = 0; i < len; i++)
            serial_putc(((const char __force *)buf)[i]);
        return (uint64_t)len;
    }
```

**4.3 sys_read 新增 FD_SERIAL 分支（对齐 Linux n_tty_read + tty_read）**

在 FD_FILE 和 FD_SOCKET 分支之间加入：

```c
    // ===== FD_SERIAL: read from kernel RX ring buffer =====
    if (proc->fd_table[fd].type == FD_SERIAL) {
        if ((proc->fd_table[fd].flags & O_WRONLY) && !(proc->fd_table[fd].flags & O_RDWR))
            return (uint64_t)-EINVAL;
        if (!buf) return (uint64_t)-EFAULT;
        uint64_t ptr_start = (__force uint64_t)buf;
        uint64_t ptr_end = ptr_start + len;
        if (ptr_end < ptr_start || ptr_start >= 0xFFFFFFFF80000000ULL
            || ptr_end > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;

        spin_lock_irqsave(&serial_rx_lock);
        while (serial_rx_head == serial_rx_tail) {
            // Buffer empty: block or EAGAIN
            if (proc->fd_table[fd].flags & O_NONBLOCK) {
                spin_unlock_irqrestore(&serial_rx_lock);
                return (uint64_t)-EAGAIN;
            }
            serial_read_waiter = proc->pid;
            proc->state = BLOCKED;
            proc->wait_event = WAIT_PIPE;  // 复用 WAIT_PIPE，语义一致
            spin_unlock_irqrestore(&serial_rx_lock);
            schedule();
            spin_lock_irqsave(&serial_rx_lock);
        }

        // Read available bytes
        size_t nread = 0;
        while (nread < len && serial_rx_head != serial_rx_tail) {
            ((char __force *)buf)[nread] = serial_rx_buf[serial_rx_tail];
            serial_rx_tail = (serial_rx_tail + 1) % SERIAL_RX_BUF_SIZE;
            nread++;
        }
        spin_unlock_irqrestore(&serial_rx_lock);
        return (uint64_t)nread;
    }
```

**4.4 sys_close 新增 FD_SERIAL 分支（对齐 Linux uart_close）**

在 FD_SOCKET 分支之后加入：

```c
    } else if (current_proc->fd_table[fd].type == FD_SERIAL) {
        serial_fd_count--;
        // Clear waiter if this process was the waiter
        spin_lock_irqsave(&serial_rx_lock);
        if (serial_read_waiter == current_proc->pid)
            serial_read_waiter = -1;
        spin_unlock_irqrestore(&serial_rx_lock);
        // Last close: mask IRQ + unregister ISR
        if (serial_fd_count == 0) {
            outb(COM1_IER, 0x00);          // Disable UART interrupts
            ioapic_set_irq(4, 36, 0, true); // Mask GSI 4 in I/O APIC
            irq_handlers[36] = NULL;         // Unregister kernel ISR
            serial_irq_registered = false;
        }
    }
```

**4.5 sys_dup2 新增 FD_SERIAL 支持**

dup2 复制 FD_SERIAL 时，`serial_fd_count++`（和 FD_PIPE/FD_FILE 的 ref_count 逻辑对齐）：

```c
// 在 dup2 复制源 fd 后、根据 type 做 ref_count++ 的位置加入：
    } else if (proc->fd_table[new_fd].type == FD_SERIAL) {
        serial_fd_count++;
    }
```

**4.6 删除 FD_PIPE → serial 镜像**

删除 `sys_write` FD_PIPE 路径末尾的镜像代码（trap.c:1933-1935）：

```c
    // 删除这段：
    // for (size_t i = 0; i < written; i++)
    //     serial_putc(((const char __force *)buf)[i]);
```

串口输出统一由 FD_SERIAL 的 `sys_write` 负责，VGA shell 输出不再自动出现在串口。

### 5. kernel/proc.c — proc_reap 扩展

在 proc_reap 的 fd 清理循环中新增 FD_SERIAL 分支：

```c
        } else if (proc->fd_table[fd].type == FD_SERIAL) {
            serial_fd_count--;
            spin_lock_irqsave(&serial_rx_lock);
            if (serial_read_waiter == proc->pid)
                serial_read_waiter = -1;
            spin_unlock_irqrestore(&serial_rx_lock);
            if (serial_fd_count == 0) {
                outb(COM1_IER, 0x00);
                ioapic_set_irq(4, 36, 0, true);
                irq_handlers[36] = NULL;
                serial_irq_registered = false;
            }
        }
```

### 6. kernel/socket.c — sys_poll 新增 FD_SERIAL

在 poll 的 fd type 分发中新增 FD_SERIAL 分支（对齐 Linux tty_poll）：

```c
            } else if (f->type == FD_SERIAL) {
                spin_lock_irqsave(&serial_rx_lock);
                // POLLIN: RX buffer has data
                if (serial_rx_head != serial_rx_tail) {
                    if (kfds[i].events & POLLIN) kfds[i].revents |= POLLIN;
                    ready++;
                }
                // POLLOUT: always ready (UART TX is non-blocking with LSR wait)
                if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
                ready++;
                spin_unlock_irqrestore(&serial_rx_lock);
            }
```

### 7. kernel/serial.h — 声明扩展

```c
#pragma once
#include <stdint.h>
#include <stdarg.h>
#include "kernel/spinlock.h"

void serial_init(void);

// RX state (for trap.c and proc.c)
extern uint8_t serial_rx_buf[];
extern uint32_t serial_rx_head;
extern uint32_t serial_rx_tail;
extern spinlock_t serial_rx_lock;
extern pid_t serial_read_waiter;
extern int serial_fd_count;
extern bool serial_irq_registered;

#ifdef NSERIAL
#define serial_putc(c)    ((void)0)
#define serial_puts(s)    ((void)0)
#define serial_put_hex(v) ((void)0)
#define serial_printf(...) ((void)0)
#else
void serial_putc(char c);
void serial_puts(const char *s);
void serial_put_hex(uint64_t val);
void serial_printf(const char *fmt, ...);
#endif
```

### 8. init/init.c — spawn serial shell（对齐 Linux agetty）

```c
#include "common/dev.h"  // DEV_SERIAL

// 在现有 spawn terminal 之后，spawn serial shell：
{
    // Open serial device (like agetty opens /dev/ttyS0)
    int sfd = open_dev(DEV_SERIAL);  // 返回 fd，O_RDWR
    if (sfd >= 0) {
        dup2(sfd, 0);  // stdin = serial
        dup2(sfd, 1);  // stdout = serial
        // 不关 sfd（dup2 后 fd 0/1 已指向 FD_SERIAL，sfd 如果 >1 可关）
        if (sfd > 1) close(sfd);

        // Spawn shell with serial fd 0/1
        spawn_service("/usr/bin/shell.elf");

        // 恢复 init 自己的 fd 0/1（如有需要）
    }
}
```

注意：init spawn shell 时子进程继承 fd 0/1。spawn 后 init 需要恢复自己的 pipe fd，或者 init 本身不用 fd 0/1 也可以（init 当前只用 printf 调试输出，不走 serial）。

更精确的做法：fork-like 语义中，先 dup2 到 0/1，spawn（子进程继承），然后 init 恢复自己的 0/1。但当前 spawn 是 syscall 不是 fork，init 可以简单处理——spawn serial shell 前设好 fd 0/1，spawn 后不管（init 不依赖 fd 0/1）。

### 9. user/include/sys/device.h — 暴露 DEV_SERIAL

```c
#define DEV_SERIAL   6
```

### 10. run.sh — 串口接 stdio

```bash
# 将 -serial file:log.txt 改为：
-serial mon:stdio
```

这样 tmux send-keys 发的数据走 QEMU stdin → COM1 → OS RX ISR → serial shell stdin。串口输出（内核 serial_printf + serial shell printf）出现在 tmux pane（`tmux capture-pane` 可抓取）。

如需持久化日志：
```bash
tmux new-session -d -s qemu './run.sh 2>&1 | tee log.txt'
```

### 11. sys_irq_bind 互斥检查

在 `sys_irq_bind` 中增加检查：如果 `irq_handlers[irq]` 已注册（被 FD_SERIAL 占用），返回 `-EBUSY`：

```c
uint64_t sys_irq_bind(uint64_t arg1, ...) {
    int irq = (int)arg1;
    if (irq < 0 || irq >= MAX_IRQ_HANDLERS) return (uint64_t)-EINVAL;
    // 互斥：不能和内核 ISR 共享同一 IRQ
    if (irq_handlers[irq] != NULL) return (uint64_t)-EBUSY;
    // ... 原有逻辑
}
```

## 数据流总结

### Serial shell 输入路径（对齐 Linux: QEMU→ttyS0→agetty→shell stdin）

```
tmux send-keys → QEMU stdin → -serial mon:stdio → COM1 RX
  → IRQ4 → serial_irq_handler → inb(COM1) → serial_rx_buf
  → wake serial_read_waiter → sys_read(FD_SERIAL) 返回字节
  → shell stdin
```

### Serial shell 输出路径（对齐 Linux: shell stdout→ttyS0→QEMU stdout）

```
shell printf → sys_write(1, buf, len)
  → FD_SERIAL → serial_putc 循环 → LSR 等待 → outb(COM1)
  → QEMU → tmux pane 显示
```

### VGA shell 路径（不变）

```
shell printf → sys_write(1, buf, len) → FD_PIPE → terminal → display SHM → KMS
```

VGA shell 输出不再镜像到串口。

## 锁协议

| 锁 | 保护对象 | 持锁者 |
|---|---|---|
| `serial_tx_lock` | UART THR 写入（outb COM1） | serial_putc (内核 printf + sys_write FD_SERIAL) |
| `serial_rx_lock` | RX ring buffer + serial_read_waiter + serial_fd_count | serial_irq_handler (ISR) + sys_read/close/reap/dup2/poll |

锁获取顺序：`serial_rx_lock` 与 `scheduler_lock` 不存在嵌套（serial_rx_lock 在 schedule 前释放）。`serial_tx_lock` 不与任何其他锁嵌套。

## 与 Linux 对齐情况

| 方面 | Linux | 本 OS | 对齐 |
|---|---|---|---|
| UART 初始化 | serial8250_startup: IER/DLAB/LCR/FCR/MCR | serial_init: 同样 7 步寄存器 | 完全对齐 |
| TX 发送 | uart_console_write → wait THRE + outb | serial_putc → wait THRE + outb + tx_lock | 对齐 |
| TX 锁 | uart_port->lock (spinlock) | serial_tx_lock (spinlock_t) | 对齐 |
| RX ISR | serial8250_interrupt → read LSR in loop | serial_irq_handler → 同样 while(LSR_DR) 循环 | 对齐 |
| RX buffer | tty_buffer (spinlock protected) | serial_rx_buf ring (serial_rx_lock) | 对齐 |
| RX 唤醒 | tty_flip_buffer_push → wait queue wake | serial_read_waiter + scheduler_lock wake | 语义对齐 |
| 设备打开 | open("/dev/ttyS0") → uart_open | sys_open_dev(DEV_SERIAL) | 对齐（无 VFS 时简化） |
| 设备关闭 | close → uart_close → shutdown + free_irq | sys_close → mask IRQ + unregister ISR | 对齐 |
| 打开时启中断 | uart_startup → request_irq | open_dev → register_irq + unmask GSI + IER | 对齐 |
| 进程模型 | agetty ttyS0 → exec /bin/sh | init: open_dev → dup2 → spawn shell.elf | 对齐 |
| 双 shell | agetty tty1 + agetty ttyS0 | VGA shell (pipe) + serial shell (FD_SERIAL) | 对齐 |
| poll | tty_poll → POLLIN/POLLOUT | sys_poll FD_SERIAL → 同样逻辑 | 对齐 |
| 进程退出清理 | tty_release → wait queue cleanup | proc_reap → waiter clear + fd_count-- | 对齐 |

## 不做的事

- **线路规则（n_tty）**：不实现内核侧行编辑/回显。Shell 的 readline 自行处理。Linux 的 n_tty 是因为要支持多种前端（cat/ed/vim），本 OS 的 serial shell 只有 shell 一个消费者，不需要。
- **多串口**：只支持 COM1，不抽象 uart_driver 框架。以后需要时再扩展。
- **TCGETS/TCSETS ioctl**：不需要设置波特率等参数（QEMU 模拟的 UART 固定 115200）。
