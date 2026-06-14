#include "stdio.h"
#include "string.h"
#include <sys.h>
#include "common/shm.h"
#include "common/dev.h"

/* ===================== KMS ring globals ===================== */

static volatile kms_ring *g_kms_ring;
static volatile driver_shm_header *g_shm_hdr;
static int32_t g_kms_driver_pid;

void kms_shm_init(uint64_t shm_addr) {
    g_shm_hdr = (volatile driver_shm_header *)shm_addr;
    g_kms_ring = (volatile kms_ring *)(shm_addr + KMS_RING_OFFSET);
}

/* Lazy init: auto-attach to KBD driver's shm on first output */
static void kms_ensure_init() {
    if (g_kms_ring) return;
    uint64_t addr = (uint64_t)sys_shm_attach(sys_lookup_dev(DEV_KBD));
    if (addr) {
        g_kms_driver_pid = sys_lookup_dev(DEV_KMS);
        kms_shm_init(addr);
    }
}

/* ===================== sys_write based flush ===================== */

static void sys_write_flush(FILE *f, const char *data, int len) {
    (void)f;
    sys_write(f->fd, data, len);
    // Mirror output to serial port
    sys_serial_write(data, len);
}

/* ===================== sys_read based fill ===================== */

static int sys_read_fill(FILE *f, char *buf, int len) {
    int64_t n = sys_read(f->fd, buf, len);
    if (n <= 0) return 0;
    return (int)n;
}

/* ===================== KMS ring output (for terminal process only) ===================== */

static void kms_write_flush(FILE *f, const char *data, int len) {
    (void)f;
    if (!g_kms_ring) kms_ensure_init();
    if (!g_kms_ring) return;
    for (int i = 0; i < len; i++) {
        // Wait for a free slot in the KMS ring
        while (g_kms_ring->head == ((g_kms_ring->tail + KMS_RING_SIZE - 1) % KMS_RING_SIZE)) {
            // Ring full — notify KMS driver to drain it
            if (g_shm_hdr->kms_sleeping) {
                sys_notify(g_kms_driver_pid);
            }
            sys_yield();
        }
        uint32_t idx = g_kms_ring->head;
        g_kms_ring->msgs[idx].cmd  = KMS_CMD_PUTC;
        g_kms_ring->msgs[idx].arg1 = (uint32_t)(unsigned char)data[i];
        g_kms_ring->msgs[idx].arg2 = 0xFFFFFF;
        g_kms_ring->head = (idx + 1) % KMS_RING_SIZE;
    }
    // Notify KMS driver if it's sleeping (fast path: skip syscall)
    if (g_shm_hdr->kms_sleeping) {
        sys_notify(g_kms_driver_pid);
    }
    // Mirror output to serial port
    sys_serial_write(data, len);
}

static FILE stdin_file = {
    0, nullptr, 0, 0, _IONBF, _F_READ, nullptr, sys_read_fill
};

static FILE stdout_file = {
    1, nullptr, 0, 0, _IONBF, _F_WRITE, sys_write_flush, nullptr
};

static FILE stderr_file = {
    2, nullptr, 0, 0, _IONBF, _F_WRITE, sys_write_flush, nullptr
};

FILE *stdin  = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

/* ===================== Buffer management ===================== */

static void file_flush(FILE *f) {
    if (f->buf_pos > 0 && f->write_fn) {
        f->write_fn(f, f->buf, f->buf_pos);
        f->buf_pos = 0;
    }
}

int fflush(FILE *f) {
    if (f) file_flush(f);
    return 0;
}

/* ===================== Character output ===================== */

static void file_putc_internal(FILE *f, char c) {
    if (f->buf_mode == _IONBF) {
        /* unbuffered: direct write */
        char tmp = c;
        if (f->write_fn) f->write_fn(f, &tmp, 1);
        return;
    }

    /* buffered: write to buffer */
    if (f->buf && f->buf_pos < f->buf_size) {
        f->buf[f->buf_pos++] = c;
    }

    /* line-buffered: flush on newline */
    if (f->buf_mode == _IOLBF && c == '\n') {
        file_flush(f);
    }

    /* full-buffered: flush when buffer full */
    if (f->buf_mode == _IOFBF && f->buf_pos >= f->buf_size) {
        file_flush(f);
    }
}

int putchar(int c) {
    file_putc_internal(stdout, (char)c);
    fflush(stdout);
    return c;
}

int fputc(int c, FILE *f) {
    file_putc_internal(f, (char)c);
    return c;
}

int fputs(const char *s, FILE *f) {
    while (*s) file_putc_internal(f, *s++);
    return 0;
}

int puts(const char *s) {
    fputs(s, stdout);
    file_putc_internal(stdout, '\n');
    return 0;
}

/* ===================== Number formatting helpers ===================== */

static void fmt_uint(FILE *f, unsigned long val, int base, int uppercase,
                     int width, char pad) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[20];
    int pos = 0;

    if (val == 0) {
        buf[pos++] = '0';
    } else {
        while (val) {
            buf[pos++] = digits[val % base];
            val /= base;
        }
    }

    /* pad to width */
    while (pos < width) buf[pos++] = pad;

    /* reverse and output */
    while (--pos >= 0) file_putc_internal(f, buf[pos]);
}

static void fmt_int(FILE *f, long val, int width, char pad) {
    if (val < 0) {
        file_putc_internal(f, '-');
        fmt_uint(f, (unsigned long)(-val), 10, 0, width > 0 ? width - 1 : 0, pad);
    } else {
        fmt_uint(f, (unsigned long)val, 10, 0, width, pad);
    }
}

/* ===================== vfprintf ===================== */

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    int count = 0;

    while (*fmt) {
        if (*fmt != '%') {
            file_putc_internal(f, *fmt++);
            count++;
            continue;
        }

        fmt++; /* skip '%' */

        /* width and pad */
        int width = 0;
        char pad = ' ';

        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '1' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* length modifier */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        /* specifier */
        switch (*fmt) {
        case '%':
            file_putc_internal(f, '%');
            count++;
            break;

        case 'c': {
            char c = (char)va_arg(ap, int);
            file_putc_internal(f, c);
            count++;
            break;
        }

        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) {
                file_putc_internal(f, *s++);
                count++;
            }
            break;
        }

        case 'd': {
            if (is_long) {
                long val = va_arg(ap, long);
                fmt_int(f, val, width, pad);
            } else {
                int val = va_arg(ap, int);
                fmt_int(f, (long)val, width, pad);
            }
            count++;
            break;
        }

        case 'u': {
            if (is_long) {
                unsigned long val = va_arg(ap, unsigned long);
                fmt_uint(f, val, 10, 0, width, pad);
            } else {
                unsigned int val = va_arg(ap, unsigned int);
                fmt_uint(f, (unsigned long)val, 10, 0, width, pad);
            }
            count++;
            break;
        }

        case 'x': {
            if (is_long) {
                unsigned long val = va_arg(ap, unsigned long);
                fmt_uint(f, val, 16, 0, width, pad);
            } else {
                unsigned int val = va_arg(ap, unsigned int);
                fmt_uint(f, (unsigned long)val, 16, 0, width, pad);
            }
            count++;
            break;
        }

        case 'X': {
            if (is_long) {
                unsigned long val = va_arg(ap, unsigned long);
                fmt_uint(f, val, 16, 1, width, pad);
            } else {
                unsigned int val = va_arg(ap, unsigned int);
                fmt_uint(f, (unsigned long)val, 16, 1, width, pad);
            }
            count++;
            break;
        }

        case 'p': {
            unsigned long val = (unsigned long)va_arg(ap, void *);
            file_putc_internal(f, '0');
            file_putc_internal(f, 'x');
            fmt_uint(f, val, 16, 0, 0, '0');
            count++;
            break;
        }

        default:
            /* unknown specifier: output as-is */
            file_putc_internal(f, '%');
            if (is_long) file_putc_internal(f, 'l');
            file_putc_internal(f, *fmt);
            count++;
            break;
        }

        fmt++;
    }

    /* line-buffered: may need final flush if no trailing newline */
    if (f->buf_mode == _IOLBF && f->buf_pos > 0) {
        /* don't auto-flush here; only fflush or newline triggers flush */
    }

    return count;
}

/* ===================== printf / fprintf ===================== */

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
    return n;
}

/* ===================== Character input ===================== */

int fgetc(FILE *f) {
    char c;
    if (f->read_fn) {
        int n = f->read_fn(f, &c, 1);
        if (n <= 0) return EOF;
        return (unsigned char)c;
    }
    /* Fallback: direct sys_read */
    int64_t n = sys_read(f->fd, &c, 1);
    if (n <= 0) return EOF;
    return (unsigned char)c;
}

int getchar(void) {
    return fgetc(stdin);
}