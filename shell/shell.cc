#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/device.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <sys/serial.h>
#include "common/shm.h"
#include "common/dev.h"
#include "common/errno.h"
#include "fcntl.h"

// ===================== FS IPC via sys_msg =====================

// Message protocol (must match fs_driver and libc file.cc)
#define FILE_CMD_OPEN      1
#define FILE_CMD_READ      2
#define FILE_CMD_WRITE     3
#define FILE_CMD_CLOSE     4
#define FILE_CMD_READDIR   5
#define FILE_CMD_CREATE    6
#define FILE_CMD_MKDIR     7
#define FILE_CMD_RAW_READ  8

struct file_req {
    uint32_t cmd;
    char     path[256];
    uint32_t flags;
    uint32_t fs_fd;
    uint64_t offset;
    uint32_t count;
    uint32_t lba;
    uint32_t readdir_offset;
    uint32_t readdir_count;
};

struct file_resp {
    int32_t  status;
    uint32_t fd;
    uint64_t file_size;
    uint32_t count;
    uint32_t total;
    uint8_t  data[];
};

// Current working directory
static char cwd[256] = "/";

static char getc() {
    char ch;
    while (read(0, &ch, 1) != 1) {}
    return ch;
}

static int readline(char *buf, int len) {
    int i = 0;
    while (i < len - 1) {
        char c = getc();
        if (c == '\n') { putchar(c); break; }
        if (c == 8) {
            if (i > 0) { i--; putchar(c); putchar(' '); putchar(c); }
            continue;
        }
        buf[i++] = c;
        putchar(c);
    }
    buf[i] = '\0';
    return i;
}

// ===================== Helpers =====================

static uint32_t parse_u32(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

// ===================== FS IPC =====================

static pid_t fs_pid;

// Reply buffer: header + up to 64KB data (same as libc and fs_driver)
#define FS_REPLY_BUF_SIZE (sizeof(struct file_resp) + 65536)
static uint8_t fs_reply_buf[FS_REPLY_BUF_SIZE];

static int fs_request(struct file_req *freq, size_t resp_len) {
    fflush(stdout);
    int r = sys_msg(fs_pid, freq, sizeof(*freq), fs_reply_buf, resp_len);
    if (r < 0) return r;
    struct file_resp *fresp = (struct file_resp *)fs_reply_buf;
    return (int)fresp->status;
}

// Write request: file_req + inline data, single sys_msg
static int fs_write_request(uint32_t fs_fd, const void *data, size_t len, size_t *written) {
    fflush(stdout);
    size_t msg_len = sizeof(struct file_req) + len;
    uint8_t *msg_buf = (uint8_t *)malloc(msg_len);
    if (!msg_buf) return -ENOMEM;
    struct file_req *freq = (struct file_req *)msg_buf;
    memset(freq, 0, sizeof(*freq));
    freq->cmd = FILE_CMD_WRITE;
    freq->fs_fd = fs_fd;
    freq->count = (uint32_t)len;
    memcpy(msg_buf + sizeof(struct file_req), data, len);
    int r = sys_msg(fs_pid, msg_buf, msg_len, fs_reply_buf, sizeof(struct file_resp));
    free(msg_buf);
    if (r < 0) return r;
    struct file_resp *fresp = (struct file_resp *)fs_reply_buf;
    if (fresp->status != 0) return (int)fresp->status;
    if (written) *written = fresp->count;
    return 0;
}

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

// ===================== Date/time formatting =====================

static const char *month_names[] = {
    "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};

// Format FAT32 date+time as "Mon DD HH:MM"
static void format_datetime(uint32_t date, uint32_t time, char *out, int out_len) {
    int month = (date >> 5) & 0x0F;
    int day   = date & 0x1F;
    int hour  = (time >> 11) & 0x1F;
    int min   = (time >> 5) & 0x3F;

    if (month < 1 || month > 12) month = 1;

    const char *mon = month_names[month - 1];
    int pos = 0;
    for (int i = 0; mon[i] && pos < out_len - 1; i++) out[pos++] = mon[i];
    if (pos < out_len - 1) out[pos++] = ' ';
    if (day < 10 && pos < out_len - 1) out[pos++] = ' ';
    if (day >= 10 && pos < out_len - 2) {
        out[pos++] = '0' + (day / 10);
        out[pos++] = '0' + (day % 10);
    } else if (pos < out_len - 1) {
        out[pos++] = '0' + day;
    }
    if (pos < out_len - 1) out[pos++] = ' ';
    if (hour < 10 && pos < out_len - 1) out[pos++] = '0';
    if (hour >= 10 && pos < out_len - 2) {
        out[pos++] = '0' + (hour / 10);
        out[pos++] = '0' + (hour % 10);
    } else if (pos < out_len - 1) {
        out[pos++] = '0' + hour;
    }
    if (pos < out_len - 1) out[pos++] = ':';
    if (min < 10 && pos < out_len - 1) out[pos++] = '0';
    if (min >= 10 && pos < out_len - 2) {
        out[pos++] = '0' + (min / 10);
        out[pos++] = '0' + (min % 10);
    } else if (pos < out_len - 1) {
        out[pos++] = '0' + min;
    }
    out[pos] = '\0';
}

// ===================== Command handlers =====================

static void cmd_ls(const char *rel_path, int long_format) {
    char abs_path[256];
    if (rel_path[0] == '\0') {
        int i;
        for (i = 0; cwd[i] && i < 255; i++) abs_path[i] = cwd[i];
        abs_path[i] = '\0';
    } else {
        build_abs_path(rel_path, abs_path);
    }

    struct file_req freq;
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_READDIR;
    strncpy(freq.path, abs_path, 255);
    freq.readdir_offset = 0;
    freq.readdir_count = 30;

    int rc = fs_request(&freq, FS_REPLY_BUF_SIZE);
    if (rc == -ENOTDIR) {
        printf("ls: not a directory\n");
        return;
    }
    if (rc != 0) {
        printf("ls: cannot access\n");
        return;
    }

    struct file_resp *fresp = (struct file_resp *)fs_reply_buf;
    fs_dirent *entries = (fs_dirent *)fresp->data;
    uint32_t total = fresp->total;

    // Paginated readdir loop
    uint32_t offset = total;
    bool first = true;
    while (true) {
        for (uint32_t i = 0; i < total; i++) {
            if (long_format) {
                if (entries[i].attr & 0x10)
                    printf("drwxr-xr-x");
                else if (entries[i].attr & 0x01)
                    printf("-r--r--r--");
                else
                    printf("-rw-r--r--");
                printf(" %u", (entries[i].attr & 0x10) ? 2 : 1);
                printf(" root root");
                printf(" %u", entries[i].size);
                char dt[20];
                format_datetime(entries[i].date, entries[i].time, dt, sizeof(dt));
                printf(" %s", dt);
                printf(" %s", entries[i].name);
                putchar('\n');
            } else {
                printf("%s\n", entries[i].name);
            }
        }

        if (first) {
            first = false;
            if (total < 30) break;

            // Fetch next page
            memset(&freq, 0, sizeof(freq));
            freq.cmd = FILE_CMD_READDIR;
            strncpy(freq.path, abs_path, 255);
            freq.readdir_offset = offset;
            freq.readdir_count = 30;
            rc = fs_request(&freq, FS_REPLY_BUF_SIZE);
            if (rc != 0) break;
            fresp = (struct file_resp *)fs_reply_buf;
            entries = (fs_dirent *)fresp->data;
            total = fresp->total;
            offset += total;
            if (total < 30) break;
        } else {
            if (total < 30) break;

            memset(&freq, 0, sizeof(freq));
            freq.cmd = FILE_CMD_READDIR;
            strncpy(freq.path, abs_path, 255);
            freq.readdir_offset = offset;
            freq.readdir_count = 30;
            rc = fs_request(&freq, FS_REPLY_BUF_SIZE);
            if (rc != 0) break;
            fresp = (struct file_resp *)fs_reply_buf;
            entries = (fs_dirent *)fresp->data;
            total = fresp->total;
            offset += total;
        }
    }
}

static void cmd_cat(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    struct file_req freq;
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_OPEN;
    freq.flags = 0;  // O_RDONLY
    strncpy(freq.path, abs_path, 255);

    if (fs_request(&freq, FS_REPLY_BUF_SIZE) != 0) {
        printf("cat: cannot open\n");
        return;
    }

    struct file_resp *fresp = (struct file_resp *)fs_reply_buf;
    uint32_t fd = fresp->fd;
    uint32_t file_size = (uint32_t)fresp->file_size;

    uint32_t offset = 0;
    while (offset < file_size) {
        uint32_t to_read = file_size - offset;
        if (to_read > 65536) to_read = 65536;

        memset(&freq, 0, sizeof(freq));
        freq.cmd = FILE_CMD_READ;
        freq.fs_fd = fd;
        freq.count = to_read;

        if (fs_request(&freq, FS_REPLY_BUF_SIZE) != 0) break;

        fresp = (struct file_resp *)fs_reply_buf;
        if (fresp->count == 0) break;

        for (uint32_t i = 0; i < fresp->count; i++) {
            putchar(fresp->data[i]);
        }
        offset += fresp->count;
    }

    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_CLOSE;
    freq.fs_fd = fd;
    fs_request(&freq, sizeof(struct file_resp));
}

static void cmd_cd(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    struct file_req freq;
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_READDIR;
    strncpy(freq.path, abs_path, 255);
    freq.readdir_offset = 0;
    freq.readdir_count = 1;

    if (fs_request(&freq, FS_REPLY_BUF_SIZE) != 0) {
        printf("cd: not a directory\n");
        return;
    }

    int i;
    for (i = 0; i < 255 && abs_path[i]; i++) cwd[i] = abs_path[i];
    cwd[i] = '\0';
    int len = i;
    if (len > 1 && cwd[len-1] == '/') { cwd[len-1] = '\0'; len--; }
}

static void cmd_pwd() {
    printf("%s\n", cwd);
}

static void cmd_touch(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    struct file_req freq;
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_CREATE;
    strncpy(freq.path, abs_path, 255);

    int rc = fs_request(&freq, sizeof(struct file_resp));
    if (rc != 0) {
        if (rc == -ENOENT) printf("touch: parent directory not found\n");
        else if (rc == -ENOTDIR) printf("touch: parent is not a directory\n");
        else if (rc == -ENOMEM) printf("touch: no free cluster\n");
        else printf("touch: error\n");
    }
}

static void cmd_echo(const char *args) {
    // Parse: echo TEXT > FILE  or  echo TEXT >> FILE
    const char *text = args;
    const char *redirect = NULL;
    int append = 0;
    // Find > or >>
    for (const char *p = args; *p; p++) {
        if (*p == '>') {
            if (*(p+1) == '>') { append = 1; redirect = p + 2; }
            else { append = 0; redirect = p + 1; }
            break;
        }
    }
    if (!redirect) { printf("Usage: echo TEXT > FILE  or  echo TEXT >> FILE\n"); return; }

    // Extract text (everything before >)
    int text_len = (int)(redirect - args - 1 - append);
    if (text_len < 0) text_len = 0;
    while (text_len > 0 && text[text_len-1] == ' ') text_len--;

    // Skip spaces after >>
    while (*redirect == ' ') redirect++;

    char abs_path[256];
    build_abs_path(redirect, abs_path);

    // Open file for write
    struct file_req freq;
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_OPEN;
    freq.flags = O_WRONLY | (append ? O_APPEND : 0);
    strncpy(freq.path, abs_path, 255);

    if (fs_request(&freq, FS_REPLY_BUF_SIZE) != 0) {
        printf("echo: cannot open %s\n", abs_path);
        return;
    }

    struct file_resp *fresp = (struct file_resp *)fs_reply_buf;
    uint32_t fd = fresp->fd;

    size_t written = 0;
    int rc = fs_write_request(fd, text, (size_t)text_len, &written);

    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_CLOSE;
    freq.fs_fd = fd;
    fs_request(&freq, sizeof(struct file_resp));

    if (rc != 0) printf("echo: write error (%d)\n", rc);
}

static void cmd_mkdir(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    struct file_req freq;
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_MKDIR;
    strncpy(freq.path, abs_path, 255);

    int rc = fs_request(&freq, sizeof(struct file_resp));
    if (rc != 0) {
        if (rc == -ENOENT) printf("mkdir: parent directory not found\n");
        else if (rc == -EEXIST) printf("mkdir: already exists\n");
        else if (rc == -ENOMEM) printf("mkdir: no free cluster\n");
        else printf("mkdir: error\n");
    }
}

static void cmd_raw_read(uint32_t lba, uint32_t count) {
    struct file_req freq;
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_RAW_READ;
    freq.lba = lba;
    freq.count = count;

    if (fs_request(&freq, FS_REPLY_BUF_SIZE) != 0) {
        printf("ERR\n");
        return;
    }

    struct file_resp *fresp = (struct file_resp *)fs_reply_buf;
    printf("OK\n");
    uint32_t bytes = fresp->count;
    const uint8_t *data = (const uint8_t *)fresp->data;
    for (uint32_t i = 0; i < bytes; i++) {
        printf("%02X", data[i]);
        if ((i + 1) % 16 == 0) putchar('\n');
        else if ((i + 1) % 8 == 0) putchar(' ');
    }
    if (bytes % 16 != 0) putchar('\n');
}

// Execute a file as an ELF: open, read, spawn, wait
static void exec_path(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    // Open file
    struct file_req freq;
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_OPEN;
    freq.flags = 0;  // O_RDONLY
    strncpy(freq.path, abs_path, 255);

    if (fs_request(&freq, FS_REPLY_BUF_SIZE) != 0) {
        printf("%s: file not found\n", rel_path);
        return;
    }

    struct file_resp *fresp = (struct file_resp *)fs_reply_buf;
    uint32_t fd = fresp->fd;
    uint32_t file_size = (uint32_t)fresp->file_size;

    if (file_size == 0) {
        printf("%s: empty file\n", rel_path);
        memset(&freq, 0, sizeof(freq));
        freq.cmd = FILE_CMD_CLOSE;
        freq.fs_fd = fd;
        fs_request(&freq, sizeof(struct file_resp));
        return;
    }

    uint8_t *elf_buf = (uint8_t *)malloc(file_size);
    if (!elf_buf) {
        printf("%s: malloc failed\n", rel_path);
        memset(&freq, 0, sizeof(freq));
        freq.cmd = FILE_CMD_CLOSE;
        freq.fs_fd = fd;
        fs_request(&freq, sizeof(struct file_resp));
        return;
    }

    // Read entire file
    uint32_t offset = 0;
    while (offset < file_size) {
        uint32_t to_read = file_size - offset;
        if (to_read > 65536) to_read = 65536;

        memset(&freq, 0, sizeof(freq));
        freq.cmd = FILE_CMD_READ;
        freq.fs_fd = fd;
        freq.count = to_read;

        if (fs_request(&freq, FS_REPLY_BUF_SIZE) != 0) {
            printf("%s: read error\n", rel_path);
            free(elf_buf);
            memset(&freq, 0, sizeof(freq));
            freq.cmd = FILE_CMD_CLOSE;
            freq.fs_fd = fd;
            fs_request(&freq, sizeof(struct file_resp));
            return;
        }

        fresp = (struct file_resp *)fs_reply_buf;
        if (fresp->count == 0) break;

        __memcpy(elf_buf + offset, fresp->data, fresp->count);
        offset += fresp->count;
    }

    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_CLOSE;
    freq.fs_fd = fd;
    fs_request(&freq, sizeof(struct file_resp));

    // ELF magic check
    if (elf_buf[0] != 0x7F || elf_buf[1] != 'E' || elf_buf[2] != 'L' || elf_buf[3] != 'F') {
        printf("%s: not an executable file\n", rel_path);
        free(elf_buf);
        return;
    }

    pid_t child_pid = spawn((const void *)elf_buf, (size_t)file_size);
    free(elf_buf);

    if (child_pid < 0) {
        printf("%s: spawn failed\n", rel_path);
        return;
    }

    int32_t exit_code = 0;
    pid_t result = waitpid(child_pid, &exit_code, 0);
    if (result < 0) {
        printf("%s: waitpid failed\n", rel_path);
        return;
    }
}

// ===================== Command parsing =====================

typedef void (*cmd_func)(const char *args);

struct cmd_entry {
    const char *name;
    cmd_func handler;
    int min_args;
};

static const cmd_entry cmds[] = {
    {"cat",   cmd_cat,    1},
    {"touch", cmd_touch,  1},
    {"mkdir", cmd_mkdir,  1},
    {"echo",  cmd_echo,   1},
};

// ===================== Main =====================

extern "C" void _start() {
    serial_write("shell: waiting for fs_driver\n", 29);
    while ((fs_pid = device_lookup(DEV_FS)) < 0) {
        struct recv_msg m;
        recv(&m, NULL, 0, 1);
    }
    serial_write("shell: fs_driver found\n", 23);

    char line[256];

    while (1) {
        printf("> ");

        int len = readline(line, sizeof(line));
        if (len == 0) continue;

        const char *p = line;
        while (*p == ' ') p++;

        char cmd_name[256];
        int ci = 0;
        while (*p && *p != ' ' && ci < 255) cmd_name[ci++] = *p++;
        cmd_name[ci] = '\0';

        while (*p == ' ') p++;

        // Built-in commands
        if (strcmp(cmd_name, "ls") == 0) {
            int long_fmt = 0;
            const char *arg = p;
            if (*p == '-' && *(p+1) == 'l') {
                long_fmt = 1;
                p += 2;
                while (*p == ' ') p++;
                arg = p;
            }
            cmd_ls(arg, long_fmt);
            continue;
        }

        if (strcmp(cmd_name, "r") == 0) {
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

        if (strcmp(cmd_name, "cd") == 0) {
            if (*p == '\0') cmd_cd("/");
            else cmd_cd(p);
            continue;
        }

        if (strcmp(cmd_name, "pwd") == 0) {
            cmd_pwd();
            continue;
        }

        if (strcmp(cmd_name, "h") == 0) {
            printf("ls [-l] [path]  - list directory\n");
            printf("cat <path>      - read file\n");
            printf("cd <path>       - change directory\n");
            printf("pwd             - print working directory\n");
            printf("touch <path>    - create empty file\n");
            printf("echo TEXT > FILE  - write text to file\n");
            printf("mkdir <path>    - create directory\n");
            printf("r LBA [COUNT]   - raw disk read\n");
            printf("<path>          - execute ELF file\n");
            printf("h               - show help\n");
            continue;
        }

        // Check built-in command table
        bool found_builtin = false;
        for (int i = 0; i < (int)(sizeof(cmds) / sizeof(cmds[0])); i++) {
            if (strcmp(cmd_name, cmds[i].name) == 0) {
                if (cmds[i].handler) {
                    if (cmds[i].min_args > 0 && *p == '\0') {
                        printf("%s: missing argument\n", cmds[i].name);
                    } else {
                        cmds[i].handler(p);
                    }
                }
                found_builtin = true;
                break;
            }
        }
        if (found_builtin) continue;

        // Not a built-in command — treat as file path to execute
        exec_path(cmd_name);
    }
}
