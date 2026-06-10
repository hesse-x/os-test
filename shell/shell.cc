#include "arch/x64/utils.h"
#include "common/shm.h"

static inline void putc(char c)       { sys_putc(c); }

static volatile kbd_shm       *kbd   = (volatile kbd_shm       *)KBD_SHM_ADDR;
static volatile fs_req_shm    *freq  = (volatile fs_req_shm    *)FS_REQ_ADDR;
static volatile fs_resp_shm   *fresp = (volatile fs_resp_shm   *)FS_RESP_ADDR;

#define FS_DRIVER_PID 5

// Current working directory
static char cwd[256] = "/";

// ===================== Keyboard input =====================

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

// ===================== Print helpers =====================

static void puts(const char *s) {
    while (*s) putc(*s++);
}

static void print_hex(uint32_t v) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        putc(hex[(v >> i) & 0xF]);
}

static void print_u32(uint32_t v) {
    if (v == 0) { putc('0'); return; }
    char buf[12];
    int i = 0;
    while (v > 0) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }
    for (int j = i - 1; j >= 0; j--) putc(buf[j]);
}

static uint32_t parse_u32(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

// ===================== FS IPC =====================

static int fs_request() {
    freq->client_pid = sys_getpid();
    sys_notify(FS_DRIVER_PID);
    sys_wait();
    return (int)fresp->status;
}

// Copy string to freq->path, zero-padded
static void set_path(const char *s) {
    for (int i = 0; i < 256; i++) {
        freq->path[i] = s[i];
        if (s[i] == '\0') {
            for (int j = i + 1; j < 256; j++) freq->path[j] = 0;
            break;
        }
    }
}

// Build absolute path from relative path + cwd
static void build_abs_path(const char *rel, char *abs) {
    int i;
    if (rel[0] == '/') {
        for (i = 0; rel[i] && i < 255; i++) abs[i] = rel[i];
        abs[i] = '\0';
    } else {
        for (i = 0; cwd[i] && i < 255; i++) abs[i] = cwd[i];
        if (i > 1 && cwd[i-1] != '/' && i < 255) abs[i++] = '/';
        for (int j = 0; rel[j] && i < 255; j++, i++) abs[i] = rel[j];
        abs[i] = '\0';
    }
}

// ===================== Command handlers =====================

static void cmd_ls(int long_format) {
    freq->cmd = FS_CMD_READDIR;
    set_path(cwd);

    if (fs_request() != 0) {
        puts("ls: error\n");
        return;
    }

    fs_dirent *entries = (fs_dirent *)fresp->data;
    for (uint32_t i = 0; i < fresp->total; i++) {
        if (long_format) {
            if (entries[i].attr & 0x10) puts("drwxr-xr-x");
            else puts("-rw-r--r--");
            puts("  0 root root ");
            print_u32(entries[i].size);
            puts(" Jan 01 00:00 ");
            puts(entries[i].name);
            putc('\n');
        } else {
            puts(entries[i].name);
            putc('\n');
        }
    }
}

static void cmd_cat(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    freq->cmd = FS_CMD_OPEN;
    set_path(abs_path);
    if (fs_request() != 0) {
        puts("cat: cannot open\n");
        return;
    }

    uint32_t fd = fresp->fd;
    uint32_t file_size = fresp->total;

    uint32_t offset = 0;
    while (offset < file_size) {
        uint32_t to_read = file_size - offset;
        if (to_read > 8176) to_read = 8176;

        freq->cmd = FS_CMD_READ;
        freq->fd = fd;
        freq->offset = offset;
        freq->count = to_read;

        if (fs_request() != 0) break;
        if (fresp->count == 0) break;

        for (uint32_t i = 0; i < fresp->count; i++) {
            putc(((char *)fresp->data)[i]);
        }
        offset += fresp->count;
    }

    freq->cmd = FS_CMD_CLOSE;
    freq->fd = fd;
    fs_request();
}

static void cmd_cd(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    freq->cmd = FS_CMD_READDIR;
    set_path(abs_path);
    if (fs_request() != 0) {
        puts("cd: not a directory\n");
        return;
    }

    for (int i = 0; i < 255 && abs_path[i]; i++) cwd[i] = abs_path[i];
    int len = 0;
    while (cwd[len]) len++;
    if (len > 1 && cwd[len-1] == '/') cwd[len-1] = '\0';
}

static void cmd_pwd() {
    puts(cwd);
    putc('\n');
}

static void cmd_touch(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    freq->cmd = FS_CMD_CREATE;
    set_path(abs_path);
    int rc = fs_request();
    if (rc != 0) {
        if (rc == 1) puts("touch: parent directory not found\n");
        else if (rc == 2) puts("touch: parent is not a directory\n");
        else if (rc == 4) puts("touch: no free cluster\n");
        else puts("touch: error\n");
    }
}

static void cmd_mkdir(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    freq->cmd = FS_CMD_MKDIR;
    set_path(abs_path);
    int rc = fs_request();
    if (rc != 0) {
        if (rc == 1) puts("mkdir: parent directory not found\n");
        else if (rc == 3) puts("mkdir: already exists\n");
        else if (rc == 4) puts("mkdir: no free cluster\n");
        else puts("mkdir: error\n");
    }
}

static void cmd_raw_read(uint32_t lba, uint32_t count) {
    freq->cmd = FS_CMD_RAW_READ;
    freq->lba = lba;
    freq->count = count;

    if (fs_request() != 0) {
        puts("ERR\n");
        return;
    }

    puts("OK\n");
    uint32_t bytes = fresp->count;
    const uint8_t *data = (const uint8_t *)fresp->data;
    for (uint32_t i = 0; i < bytes; i++) {
        const char *hex = "0123456789ABCDEF";
        putc(hex[(data[i] >> 4) & 0xF]);
        putc(hex[data[i] & 0xF]);
        if ((i + 1) % 16 == 0) putc('\n');
        else if ((i + 1) % 8 == 0) putc(' ');
    }
    if (bytes % 16 != 0) putc('\n');
}

// ===================== Table-driven command parsing =====================

typedef void (*cmd_func)(const char *args);

struct cmd_entry {
    const char *name;
    cmd_func handler;
    int min_args;  // 0 = no args needed, 1 = requires one arg
};

static const cmd_entry cmds[] = {
    {"cat",   cmd_cat,             1},
    {"touch", cmd_touch,           1},
    {"mkdir", cmd_mkdir,           1},
};

static int my_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

// ===================== Main =====================

extern "C" void _start() {
    char line[80];

    while (1) {
        putc('>');
        putc(' ');

        int len = readline(line, sizeof(line));
        if (len == 0) continue;

        // Skip leading spaces
        const char *p = line;
        while (*p == ' ') p++;

        // Extract command name
        char cmd_name[16];
        int ci = 0;
        while (*p && *p != ' ' && ci < 15) cmd_name[ci++] = *p++;
        cmd_name[ci] = '\0';

        // Skip spaces after command
        while (*p == ' ') p++;

        // Handle special commands
        // ls with -l flag
        if (my_strcmp(cmd_name, "ls") == 0) {
            int long_fmt = 0;
            if (*p == '-' && *(p+1) == 'l') long_fmt = 1;
            cmd_ls(long_fmt);
            continue;
        }

        // r LBA [COUNT] — special numeric parsing
        if (my_strcmp(cmd_name, "r") == 0) {
            uint32_t lba = 0;
            uint32_t count = 1;
            if (*p) {
                lba = parse_u32(p);
                const char *q = p;
                while (*q >= '0' && *q <= '9') q++;
                if (*q == ' ') count = parse_u32(q + 1);
            }
            cmd_raw_read(lba, count);
            continue;
        }

        // cd with no arg = cd /
        if (my_strcmp(cmd_name, "cd") == 0) {
            if (*p == '\0') cmd_cd("/");
            else cmd_cd(p);
            continue;
        }

        // Help
        if (my_strcmp(cmd_name, "h") == 0) {
            puts("ls [-l]         - list directory\n");
            puts("cat <path>      - read file\n");
            puts("cd <path>       - change directory\n");
            puts("pwd             - print working directory\n");
            puts("touch <path>    - create empty file / update timestamp\n");
            puts("mkdir <path>    - create directory\n");
            puts("r LBA [COUNT]   - raw disk read (hex dump)\n");
            puts("h               - show this help\n");
            continue;
        }

        // Standard command lookup
        for (int i = 0; i < (int)(sizeof(cmds) / sizeof(cmds[0])); i++) {
            if (my_strcmp(cmd_name, cmds[i].name) == 0) {
                if (cmds[i].handler) {
                    if (cmds[i].min_args > 0 && *p == '\0') {
                        puts(cmds[i].name);
                        puts(": missing argument\n");
                    } else {
                        cmds[i].handler(p);
                    }
                }
                goto next_line;
            }
        }

        puts("unknown cmd\n");
next_line:;
    }
}