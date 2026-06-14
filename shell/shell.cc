#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/device.h>
#include <sys/shm.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <sys/serial.h>
#include "common/shm.h"
#include "common/dev.h"

// ===================== FS IPC =====================

static volatile fs_req_shm    *freq;
static volatile fs_resp_shm   *fresp;
static volatile fs_shm_header *fs_hdr;

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

static int fs_request() {
    freq->client_pid = getpid();
    fflush(stdout);

    fs_rpc_request req;
    req.cmd = freq->cmd;
    memset(req.reserved, 0, sizeof(req.reserved));

    fs_rpc_reply rep;
    rpc(fs_pid, &req, &rep);

    fresp->status = rep.status;
    fresp->fd     = rep.fd;
    fresp->count  = rep.count;
    fresp->total  = rep.total;

    return (int)fresp->status;
}

static void set_path(const char *s) {
    for (int i = 0; i < 256; i++) {
        freq->path[i] = s[i];
        if (s[i] == '\0') { for (int j = i + 1; j < 256; j++) freq->path[j] = 0; break; }
    }
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
    int year  = ((date >> 9) & 0x7F) + 1980;
    int month = (date >> 5) & 0x0F;
    int day   = date & 0x1F;
    int hour  = (time >> 11) & 0x1F;
    int min   = (time >> 5) & 0x3F;

    if (month < 1 || month > 12) month = 1;

    // Simple formatting: "Jun 14 10:30"
    const char *mon = month_names[month - 1];
    int pos = 0;
    for (int i = 0; mon[i] && pos < out_len - 1; i++) out[pos++] = mon[i];
    if (pos < out_len - 1) out[pos++] = ' ';
    // Day
    if (day < 10 && pos < out_len - 1) out[pos++] = ' ';
    if (day >= 10 && pos < out_len - 2) {
        out[pos++] = '0' + (day / 10);
        out[pos++] = '0' + (day % 10);
    } else if (pos < out_len - 1) {
        out[pos++] = '0' + day;
    }
    if (pos < out_len - 1) out[pos++] = ' ';
    // Hour
    if (hour < 10 && pos < out_len - 1) out[pos++] = '0';
    if (hour >= 10 && pos < out_len - 2) {
        out[pos++] = '0' + (hour / 10);
        out[pos++] = '0' + (hour % 10);
    } else if (pos < out_len - 1) {
        out[pos++] = '0' + hour;
    }
    if (pos < out_len - 1) out[pos++] = ':';
    // Minute
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
        // No argument — list cwd directly
        for (int i = 0; cwd[i] && i < 255; i++) abs_path[i] = cwd[i];
        abs_path[255] = '\0';
    } else {
        build_abs_path(rel_path, abs_path);
    }

    // Try readdir directly — if path is a file, fs_driver returns status 2
    freq->cmd = FS_CMD_READDIR;
    set_path(abs_path);
    freq->offset = 0;
    freq->count = 30;

    int rc = fs_request();
    if (rc == 2) {
        printf("ls: not a directory\n");
        return;
    }
    if (rc != 0) {
        printf("ls: cannot access\n");
        return;
    }

    fs_dirent *entries = (fs_dirent *)fresp->data;
    uint32_t total = fresp->total;

    // Paginated readdir loop
    uint32_t offset = 0;
    bool first = true;
    while (true) {
        if (!first) {
            freq->cmd = FS_CMD_READDIR;
            set_path(abs_path);
            freq->offset = offset;
            freq->count = 30;
            rc = fs_request();
            if (rc != 0) break;
            entries = (fs_dirent *)fresp->data;
            total = fresp->total;
        }
        first = false;

        for (uint32_t i = 0; i < total; i++) {
            if (long_format) {
                // Permissions
                if (entries[i].attr & 0x10)
                    printf("drwxr-xr-x");
                else if (entries[i].attr & 0x01)
                    printf("-r--r--r--");
                else
                    printf("-rw-r--r--");
                // Hard links
                printf(" %u", (entries[i].attr & 0x10) ? 2 : 1);
                // Owner/group
                printf(" root root");
                // Size
                printf(" %u", entries[i].size);
                // Date/time
                char dt[20];
                format_datetime(entries[i].date, entries[i].time, dt, sizeof(dt));
                printf(" %s", dt);
                // Name
                printf(" %s", entries[i].name);
                putchar('\n');
            } else {
                printf("%s\n", entries[i].name);
            }
        }

        offset += total;
        if (total < 30) break;
    }
}

static void cmd_cat(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    freq->cmd = FS_CMD_OPEN;
    set_path(abs_path);
    if (fs_request() != 0) {
        printf("cat: cannot open\n");
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
            putchar(((char *)fresp->data)[i]);
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
    freq->offset = 0;
    freq->count = 1;
    if (fs_request() != 0) {
        printf("cd: not a directory\n");
        return;
    }

    for (int i = 0; i < 255 && abs_path[i]; i++) cwd[i] = abs_path[i];
    int len = 0;
    while (cwd[len]) len++;
    if (len > 1 && cwd[len-1] == '/') cwd[len-1] = '\0';
}

static void cmd_pwd() {
    printf("%s\n", cwd);
}

static void cmd_touch(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    freq->cmd = FS_CMD_CREATE;
    set_path(abs_path);
    int rc = fs_request();
    if (rc != 0) {
        if (rc == 1) printf("touch: parent directory not found\n");
        else if (rc == 2) printf("touch: parent is not a directory\n");
        else if (rc == 4) printf("touch: no free cluster\n");
        else printf("touch: error\n");
    }
}

static void cmd_mkdir(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    freq->cmd = FS_CMD_MKDIR;
    set_path(abs_path);
    int rc = fs_request();
    if (rc != 0) {
        if (rc == 1) printf("mkdir: parent directory not found\n");
        else if (rc == 3) printf("mkdir: already exists\n");
        else if (rc == 4) printf("mkdir: no free cluster\n");
        else printf("mkdir: error\n");
    }
}

static void cmd_raw_read(uint32_t lba, uint32_t count) {
    freq->cmd = FS_CMD_RAW_READ;
    freq->lba = lba;
    freq->count = count;

    if (fs_request() != 0) {
        printf("ERR\n");
        return;
    }

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
    freq->cmd = FS_CMD_OPEN;
    set_path(abs_path);
    if (fs_request() != 0) {
        printf("%s: file not found\n", rel_path);
        return;
    }

    uint32_t fd = fresp->fd;
    uint32_t file_size = fresp->total;

    if (file_size == 0) {
        printf("%s: empty file\n", rel_path);
        freq->cmd = FS_CMD_CLOSE;
        freq->fd = fd;
        fs_request();
        return;
    }

    uint8_t *elf_buf = (uint8_t *)malloc(file_size);
    if (!elf_buf) {
        printf("%s: malloc failed\n", rel_path);
        freq->cmd = FS_CMD_CLOSE;
        freq->fd = fd;
        fs_request();
        return;
    }

    // Read entire file
    uint32_t offset = 0;
    while (offset < file_size) {
        uint32_t to_read = file_size - offset;
        if (to_read > 8176) to_read = 8176;

        freq->cmd = FS_CMD_READ;
        freq->fd = fd;
        freq->offset = offset;
        freq->count = to_read;

        if (fs_request() != 0) {
            printf("%s: read error\n", rel_path);
            free(elf_buf);
            freq->cmd = FS_CMD_CLOSE;
            freq->fd = fd;
            fs_request();
            return;
        }
        if (fresp->count == 0) break;

        __memcpy(elf_buf + offset, (const void *)fresp->data, fresp->count);
        offset += fresp->count;
    }

    freq->cmd = FS_CMD_CLOSE;
    freq->fd = fd;
    fs_request();

    // ELF magic check
    if (elf_buf[0] != 0x7F || elf_buf[1] != 'E' || elf_buf[2] != 'L' || elf_buf[3] != 'F') {
        printf("%s: not an executable file\n", rel_path);
        free(elf_buf);
        return;
    }

    pid_t child_pid = spawn((const void *)elf_buf, (size_t)file_size, 0);
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
};

// ===================== Main =====================

extern "C" void _start() {
    serial_write("shell: waiting for fs_driver\n", 29);
    while ((fs_pid = device_lookup(DEV_FS)) < 0) {
        struct recv_msg m;
        recv(&m, 1);
    }
    serial_write("shell: fs_driver found\n", 23);

    void *shm_addr = NULL;
    while (shm_attach(fs_pid, &shm_addr) < 0) {
        struct recv_msg m;
        recv(&m, 1);
    }
    uint64_t fs_shm = (uint64_t)shm_addr;
    serial_write("shell: fs_driver attached\n", 26);
    fs_hdr = (volatile fs_shm_header *)(fs_shm + FS_SHM_HEADER_OFFSET);
    freq   = (volatile fs_req_shm *)(fs_shm + FS_REQ_OFFSET);
    fresp  = (volatile fs_resp_shm *)(fs_shm + FS_RESP_OFFSET);

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
