#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "common/syscall.h"
#include "common/shm.h"
#include "common/pid.h"
#include "arch/x64/utils.h"

// ===================== KMS output =====================

static volatile kms_req_shm *kms_req = (volatile kms_req_shm *)KMS_REQ_ADDR;

static void kms_flush() {
    if (kms_req->count > 0) {
        sys_notify(KMS_DRIVER_PID);
    }
}

// ===================== Keyboard input =====================

static volatile kbd_shm       *kbd   = (volatile kbd_shm       *)KBD_SHM_ADDR;
static volatile fs_req_shm    *freq  = (volatile fs_req_shm    *)FS_REQ_ADDR;
static volatile fs_resp_shm   *fresp = (volatile fs_resp_shm   *)FS_RESP_ADDR;

// Current working directory
static char cwd[256] = "/";

#define KBD_BUF_SIZE 4088

static char getc() {
    while (kbd->tail == kbd->head) {
        kms_flush();
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
            putchar(c);
            break;
        }
        if (c == 8) { // backspace
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
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

static uint64_t parse_hex64(const char *s) {
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++;
        if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
        else break;
    }
    return v;
}

// ===================== FS IPC =====================

static int fs_request() {
    freq->client_pid = sys_getpid();
    kms_flush();
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
        printf("ls: error\n");
        return;
    }

    fs_dirent *entries = (fs_dirent *)fresp->data;
    for (uint32_t i = 0; i < fresp->total; i++) {
        if (long_format) {
            if (entries[i].attr & 0x10) printf("drwxr-xr-x");
            else printf("-rw-r--r--");
            printf("  0 root root %u Jan 01 00:00 %s\n", entries[i].size, entries[i].name);
        } else {
            printf("%s\n", entries[i].name);
        }
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

static void cmd_sbrk(const char *args) {
    uint32_t increment = parse_u32(args);
    int64_t result = sys_sbrk((int64_t)increment);
    if (result < 0)
        printf("sbrk(%u) = -%lX\n", increment, (unsigned long)(-result));
    else
        printf("sbrk(%u) = %lX\n", increment, (unsigned long)result);
}

static void cmd_malloc(const char *args) {
    uint32_t size = parse_u32(args);
    void *p = malloc(size);
    if (p) {
        printf("malloc(%u) = %p\n", size, p);
        char *buf = (char *)p;
        for (uint32_t i = 0; i < size && i < 64; i++)
            buf[i] = 'A' + (i % 26);
    } else {
        printf("malloc(%u) = NULL\n", size);
    }
}

static void cmd_free(const char *args) {
    uint64_t addr = parse_hex64(args);
    if (addr == 0) {
        printf("free: invalid addr\n");
        return;
    }
    printf("free(%p)\n", (void *)addr);
    free((void *)addr);
}

static void cmd_malloc_test(const char *) {
    printf("=== malloc test ===\n");

    void *p1 = malloc(64);
    printf("1. malloc(64) = %p\n", p1);

    void *p2 = malloc(128);
    printf("2. malloc(128) = %p\n", p2);

    void *p3 = malloc(32);
    printf("3. malloc(32) = %p\n", p3);

    free(p2);
    printf("4. free(p2)\n");

    void *p4 = realloc(p1, 256);
    printf("5. realloc(p1,256) = %p\n", p4);

    int *arr = (int *)calloc(10, sizeof(int));
    printf("6. calloc(10,4) = %p", arr);
    if (arr) {
        int ok = 1;
        for (int i = 0; i < 10; i++) { if (arr[i] != 0) ok = 0; }
        printf(ok ? " zero-ok" : " NOT-ZERO");
    }
    putchar('\n');

    void *p5 = malloc(0);
    printf("7. malloc(0) = %p\n", p5);

    free(p3);
    free(p4);
    free(arr);
    free(p5);
    printf("8. all freed\n");

    printf("=== test done ===\n");
}

// run <path>: read ELF file from FAT32, spawn as child process, wait for it
static void cmd_run(const char *rel_path) {
    char abs_path[256];
    build_abs_path(rel_path, abs_path);

    freq->cmd = FS_CMD_OPEN;
    set_path(abs_path);
    if (fs_request() != 0) {
        printf("run: cannot open\n");
        return;
    }

    uint32_t fd = fresp->fd;
    uint32_t file_size = fresp->total;

    if (file_size == 0) {
        printf("run: empty file\n");
        freq->cmd = FS_CMD_CLOSE;
        freq->fd = fd;
        fs_request();
        return;
    }

    uint8_t *elf_buf = (uint8_t *)malloc(file_size);
    if (!elf_buf) {
        printf("run: malloc failed\n");
        freq->cmd = FS_CMD_CLOSE;
        freq->fd = fd;
        fs_request();
        return;
    }

    uint32_t offset = 0;
    while (offset < file_size) {
        uint32_t to_read = file_size - offset;
        if (to_read > 8176) to_read = 8176;

        freq->cmd = FS_CMD_READ;
        freq->fd = fd;
        freq->offset = offset;
        freq->count = to_read;

        if (fs_request() != 0) {
            printf("run: read error\n");
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

    int64_t child_pid = sys_spawn((const void *)elf_buf, (uint64_t)file_size, 0);
    free(elf_buf);

    if (child_pid < 0) {
        printf("run: spawn failed (errno=%lX)\n", (unsigned long)(-child_pid));
        return;
    }

    printf("run: spawned pid=%u\n", (uint32_t)child_pid);

    int32_t exit_code = 0;
    int64_t result = sys_waitpid((int32_t)child_pid, &exit_code);

    if (result < 0) {
        printf("run: waitpid failed\n");
        return;
    }

    printf("run: pid=%u exited with code %u\n", (uint32_t)child_pid, (uint32_t)exit_code);
}

// ===================== Command parsing =====================

typedef void (*cmd_func)(const char *args);

struct cmd_entry {
    const char *name;
    cmd_func handler;
    int min_args;
};

static const cmd_entry cmds[] = {
    {"cat",   cmd_cat,             1},
    {"touch", cmd_touch,           1},
    {"mkdir", cmd_mkdir,           1},
};

// ===================== Main =====================

extern "C" void _start() {
    char line[80];

    while (1) {
        printf("> ");

        int len = readline(line, sizeof(line));
        if (len == 0) continue;

        const char *p = line;
        while (*p == ' ') p++;

        char cmd_name[16];
        int ci = 0;
        while (*p && *p != ' ' && ci < 15) cmd_name[ci++] = *p++;
        cmd_name[ci] = '\0';

        while (*p == ' ') p++;

        if (strcmp(cmd_name, "ls") == 0) {
            int long_fmt = 0;
            if (*p == '-' && *(p+1) == 'l') long_fmt = 1;
            cmd_ls(long_fmt);
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

        if (strcmp(cmd_name, "sbrk") == 0) {
            cmd_sbrk(p);
            continue;
        }

        if (strcmp(cmd_name, "malloc") == 0) {
            cmd_malloc(p);
            continue;
        }

        if (strcmp(cmd_name, "free") == 0) {
            cmd_free(p);
            continue;
        }

        if (strcmp(cmd_name, "mtest") == 0) {
            cmd_malloc_test(p);
            continue;
        }

        if (strcmp(cmd_name, "run") == 0) {
            if (*p == '\0') { printf("run: missing argument\n"); continue; }
            cmd_run(p);
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
            printf("ls [-l]         - list directory\n");
            printf("cat <path>      - read file\n");
            printf("cd <path>       - change directory\n");
            printf("pwd             - print working directory\n");
            printf("touch <path>    - create empty file\n");
            printf("mkdir <path>    - create directory\n");
            printf("run <path>      - execute ELF file\n");
            printf("r LBA [COUNT]   - raw disk read\n");
            printf("sbrk N          - test sbrk syscall\n");
            printf("malloc N        - test malloc\n");
            printf("free ADDR       - test free\n");
            printf("mtest           - malloc test suite\n");
            printf("h               - show help\n");
            continue;
        }

        for (int i = 0; i < (int)(sizeof(cmds) / sizeof(cmds[0])); i++) {
            if (strcmp(cmd_name, cmds[i].name) == 0) {
                if (cmds[i].handler) {
                    if (cmds[i].min_args > 0 && *p == '\0') {
                        printf("%s: missing argument\n", cmds[i].name);
                    } else {
                        cmds[i].handler(p);
                    }
                }
                goto next_line;
            }
        }

        printf("unknown cmd\n");
next_line:;
    }
}
