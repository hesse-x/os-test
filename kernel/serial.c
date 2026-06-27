#include "kernel/serial.h"
#include "kernel/devtmpfs.h"
#include "kernel/proc.h"
#include "kernel/trap.h"
#include "kernel/inode.h"
#include "arch/x64/utils.h"
#include "arch/x64/apic.h"
#include "common/errno.h"
#include "common/dev.h"     // DEV_SERIAL
#include "common/socket.h"  // POLLIN/POLLOUT
#include "common/kvformat.h"

#include "common/ioctl.h"   // TCGETS

// ===================== TX lock =====================
spinlock_t serial_tx_lock;

// ===================== RX ring buffer =====================

uint8_t serial_rx_buf[SERIAL_RX_BUF_SIZE];
uint32_t serial_rx_head = 0;  // ISR write position
uint32_t serial_rx_tail = 0;  // sys_read read position
spinlock_t serial_rx_lock;
int32_t serial_read_waiter = -1;
int serial_fd_count = 0;
bool serial_irq_registered = false;

void serial_init(void) {
    outb(COM1_IER, 0x00);    // Disable all interrupts
    outb(COM1_LCR, 0x80);    // Enable DLAB
    outb(COM1,     0x01);    // Divisor low byte: 115200 baud
    outb(COM1_IER, 0x00);    // Divisor high byte
    outb(COM1_LCR, 0x03);    // 8N1, disable DLAB
    outb(COM1_FCR, 0xC7);    // Enable FIFO, clear, 14-byte threshold
    outb(COM1_MCR, 0x03);    // DTR + RTS
    // IER RX enable deferred to first serial open
}

#ifndef NSERIAL

static void serial_putc(char c) {
    uint64_t flags;
    spin_lock_irqsave(&serial_tx_lock, &flags);
    while (!(inb(COM1_LSR) & LSR_THRE))
        ;
    outb(COM1, c);
    spin_unlock_irqrestore(&serial_tx_lock, flags);
}

// ISR: drain all available bytes from UART FIFO into kernel ring buffer
static void serial_irq_handler(trapframe_t *tf) {
    uint64_t flags;
    spin_lock_irqsave(&serial_rx_lock, &flags);
    // Drain all available bytes from FIFO (Linux: read LSR in loop)
    while (inb(COM1_LSR) & LSR_DR) {
        uint8_t c = inb(COM1);
        uint32_t next = (serial_rx_head + 1) % SERIAL_RX_BUF_SIZE;
        if (next != serial_rx_tail) {  // drop if full
            serial_rx_buf[serial_rx_head] = c;
            serial_rx_head = next;
        }
    }
    // Wake blocked reader (like Linux tty_flip_buffer_push → wait queue wake)
    if (serial_read_waiter >= 0) {
        int32_t waiter = serial_read_waiter;
        serial_read_waiter = -1;
        spin_unlock_irqrestore(&serial_rx_lock, flags);
        wake_process(waiter);
        return;
    }
    spin_unlock_irqrestore(&serial_rx_lock, flags);
}

static void serial_puts(const char *s) {
  while (*s) {
    serial_putc(*s++);
  }
}

struct serial_buf_arg { char *buf; int pos; int cap; };

static void serial_buf_putc(char c, void *arg) {
    struct serial_buf_arg *a = (struct serial_buf_arg *)arg;
    if (a->pos < a->cap) a->buf[a->pos++] = c;
}

void serial_printf(const char *fmt, ...) {
  char buf[256];
  struct serial_buf_arg a = { buf, 0, 255 };
  va_list ap;
  va_start(ap, fmt);
  kvformat(serial_buf_putc, &a, fmt, ap);
  va_end(ap);
  buf[a.pos] = '\0';
  serial_puts(buf);
}

// ===================== Serial dev_ops (VFS callback dispatch) =====================

static int serial_dev_open(struct task_t *proc, int fd) {
    // Mutual exclusion: cannot coexist with user-space irq_bind on vector 36
    // But if serial IRQ is already registered (by a previous open), it's fine —
    // just bump the fd count.
    if (!serial_irq_registered && irq_owner_check(36) >= 0) return -EBUSY;

    serial_fd_count++;
    if (!serial_irq_registered) {
        register_irq(36, serial_irq_handler);
        uint32_t bsp_apic_id = (uint32_t)(lapic_read(LAPIC_ID) >> 24);
        ioapic_set_irq(4, 36, bsp_apic_id, false, false, false);  // edge-triggered
        outb(COM1_IER, IER_RX_ENABLE);
        serial_irq_registered = true;
    }
    return 0;
}

static int serial_dev_close(struct task_t *proc, int fd) {
    serial_fd_count--;
    uint64_t rx_flags;
    spin_lock_irqsave(&serial_rx_lock, &rx_flags);
    if (serial_read_waiter == proc->pid)
        serial_read_waiter = -1;
    spin_unlock_irqrestore(&serial_rx_lock, rx_flags);
    if (serial_fd_count == 0) {
        outb(COM1_IER, 0x00);           // Disable UART interrupts
        ioapic_set_irq(4, 36, 0, true, false, false); // Mask GSI 4 in I/O APIC
        unregister_irq(36);
        serial_irq_registered = false;
    }
    return 0;
}

static ssize_t serial_dev_read(struct task_t *proc, int fd, void *buf, size_t count) {
    if (!buf) return -EFAULT;

    uint64_t rx_flags;
    spin_lock_irqsave(&serial_rx_lock, &rx_flags);
    while (serial_rx_head == serial_rx_tail) {
        if (proc->mm->files->fd_table[fd].flags & O_NONBLOCK) {
            spin_unlock_irqrestore(&serial_rx_lock, rx_flags);
            return -EAGAIN;
        }
        serial_read_waiter = proc->pid;
        proc->state = BLOCKED;
        proc->wait_event = WAIT_PIPE;  // reuse WAIT_PIPE, same semantics
        spin_unlock_irqrestore(&serial_rx_lock, rx_flags);
        schedule();
        spin_lock_irqsave(&serial_rx_lock, &rx_flags);
    }

    size_t nread = 0;
    char *dst = (char __force *)buf;
    while (nread < count && serial_rx_head != serial_rx_tail) {
        dst[nread] = serial_rx_buf[serial_rx_tail];
        serial_rx_tail = (serial_rx_tail + 1) % SERIAL_RX_BUF_SIZE;
        nread++;
    }
    spin_unlock_irqrestore(&serial_rx_lock, rx_flags);
    return (ssize_t)nread;
}

static ssize_t serial_dev_write(struct task_t *proc, int fd, const void *buf, size_t count) {
    if (!buf) return -EFAULT;
    const char *src = (const char __force *)buf;
    for (size_t i = 0; i < count; i++)
        serial_putc(src[i]);
    return (ssize_t)count;
}

static long serial_dev_ioctl(uint32_t cmd, void *arg) {
    if (cmd == TCGETS) return 0;  // serial is a tty
    return -ENOTTY;
}

static __poll_t serial_dev_poll(struct task_t *proc, int events) {
    __poll_t revents = 0;
    uint64_t rx_flags;
    spin_lock_irqsave(&serial_rx_lock, &rx_flags);
    if (serial_rx_head != serial_rx_tail) {
        if (events & POLLIN) revents |= POLLIN;
    }
    spin_unlock_irqrestore(&serial_rx_lock, rx_flags);
    // POLLOUT: always ready
    if (events & POLLOUT) revents |= POLLOUT;
    return revents;
}

static struct dev_ops serial_ops = {
    .driver_pid  = 0,
    .device_type = DEV_SERIAL,
    .open        = serial_dev_open,
    .close       = serial_dev_close,
    .read        = serial_dev_read,
    .write       = serial_dev_write,
    .ioctl       = serial_dev_ioctl,
    .poll        = serial_dev_poll,
};

void serial_dev_register(void) {
    int rc = devtmpfs_create("serial", DEV_SERIAL, &serial_ops);
    if (rc != 0) {
        serial_printf("serial_dev_register: failed (rc=%d)\n", rc);
    }
}

#endif
