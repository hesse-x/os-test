#include "arch/x64/utils.h"
#include "common/shm.h"

static inline void putc(char c)       { sys_putc(c); }
static inline long getpid()           { return sys_getpid(); }

static volatile kbd_shm *kbd = (volatile kbd_shm *)KBD_SHM_ADDR;
static volatile disk_req_shm  *dreq  = (volatile disk_req_shm  *)DISK_REQ_ADDR;
static volatile disk_resp_shm *dresp = (volatile disk_resp_shm *)DISK_RESP_ADDR;

// disk_driver PID: we are the last created process (PID4),
// disk_driver is PID2 (first created after idle).
// Hardcoded for now; could be replaced by a naming service later.
#define DISK_DRIVER_PID 2

#define KBD_BUF_SIZE 4088

static char getc() {
    while (kbd->tail == kbd->head) {
        sys_wait();
    }
    char ch = (char)kbd->data[kbd->tail];
    kbd->tail = (kbd->tail + 1) % KBD_BUF_SIZE;
    return ch;
}

// Read a line into buf (max len-1 chars), returns length
static int readline(char *buf, int len) {
    int i = 0;
    while (i < len - 1) {
        char c = getc();
        if (c == '\n') {
            putc(c);
            break;
        }
        if (c == 8) { // backspace
            if (i > 0) { i--; putc(c); putc(' '); putc(c); }
            continue;
        }
        buf[i++] = c;
        putc(c);
    }
    buf[i] = '\0';
    return i;
}

// Parse decimal string to uint32_t
static uint32_t parse_u32(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

// Print hex value
static void print_hex(uint32_t v) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        putc(hex[(v >> i) & 0xF]);
}

// Print string
static void puts(const char *s) {
    while (*s) putc(*s++);
}

// Read sectors from disk via disk_driver
// Returns 0 on success, non-zero on failure
static int disk_read(uint32_t lba, uint32_t count) {
    dreq->cmd   = 0; // DISK_CMD_READ
    dreq->lba   = lba;
    dreq->count = count;

    sys_notify(DISK_DRIVER_PID);
    sys_wait();

    return dresp->status;
}

// Print hex dump of response data
static void dump_response(uint32_t count) {
    uint32_t bytes = count * 512;
    if (bytes > 4088) bytes = 4088;
    for (uint32_t i = 0; i < bytes; i++) {
        const char *hex = "0123456789ABCDEF";
        putc(hex[(dresp->data[i] >> 4) & 0xF]);
        putc(hex[dresp->data[i] & 0xF]);
        if ((i + 1) % 16 == 0) putc('\n');
        else if ((i + 1) % 8 == 0) putc(' ');
    }
    if (bytes % 16 != 0) putc('\n');
}

extern "C" void _start() {
    char line[80];

    while (1) {
        putc('>');
        putc(' ');

        int len = readline(line, sizeof(line));

        if (len == 0) continue;

        // Parse command: "r LBA [COUNT]"
        if (line[0] == 'r' && (line[1] == ' ' || line[1] == '\0')) {
            uint32_t lba = 0;
            uint32_t count = 1;
            if (line[1] == ' ') {
                lba = parse_u32(line + 2);
                // Check for optional count
                const char *p = line + 2;
                while (*p >= '0' && *p <= '9') p++;
                if (*p == ' ') count = parse_u32(p + 1);
            }
            int rc = disk_read(lba, count);
            if (rc == 0) {
                puts("OK\n");
                dump_response(count);
            } else {
                puts("ERR\n");
            }
        } else if (line[0] == 'h' && (line[1] == ' ' || line[1] == '\0')) {
            puts("r LBA [COUNT]  - read disk sectors (hex dump)\n");
            puts("h             - show this help\n");
        } else {
            puts("unknown cmd\n");
        }
    }
}
