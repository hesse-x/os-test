// libc file I/O: fd_table + open/read/write/close/pipe dispatch
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/device.h>
#include "common/syscall.h"
#include "common/dev.h"

#define MAX_FD  32

enum fd_type_t { FD_NONE = 0, FD_PIPE, FD_FILE };

struct file_fd_entry {
    enum fd_type_t type;    // FD_NONE / FD_PIPE / FD_FILE
    int     flags;          // O_RDONLY / O_WRONLY / O_RDWR
    // FD_FILE only:
    int32_t fs_fd;          // fs_driver local fd
    uint64_t offset;        // current read/write offset (libc maintained)
    uint64_t file_size;     // file size (set by open)
};

static struct file_fd_entry fd_table[MAX_FD];

// Auto-init fd 0/1 as FD_PIPE (stdin/stdout)
static bool fd_table_inited;

static void fd_table_init() {
    if (fd_table_inited) return;
    for (int i = 0; i < MAX_FD; i++) {
        fd_table[i].type = FD_NONE;
        fd_table[i].flags = 0;
        fd_table[i].fs_fd = -1;
        fd_table[i].offset = 0;
    }
    fd_table[0].type = FD_PIPE;
    fd_table[0].flags = O_RDONLY;
    fd_table[1].type = FD_PIPE;
    fd_table[1].flags = O_WRONLY;
    fd_table_inited = true;
}

uint64_t fd_file_size(int fd) {
    fd_table_init();
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type != FD_FILE)
        return 0;
    return fd_table[fd].file_size;
}

// Lazy fs_driver PID cache
static pid_t fs_pid_cache = -1;

static pid_t get_fs_pid() {
    if (fs_pid_cache < 0) {
        fs_pid_cache = device_lookup(DEV_FS);
    }
    return fs_pid_cache;
}

// ===================== Message protocol =====================

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
    // OPEN/CREATE/MKDIR
    char     path[256];
    uint32_t flags;
    // READ/WRITE/CLOSE
    uint32_t fs_fd;
    uint64_t offset;
    uint32_t count;
    // RAW_READ
    uint32_t lba;
    // READDIR
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

// ===================== open =====================

int open(const char *path, int flags, ...) {
    fd_table_init();

    pid_t pid = get_fs_pid();
    if (pid < 0) { errno = ENOENT; return -1; }

    int fd = -1;
    for (int i = 3; i < MAX_FD; i++) {
        if (fd_table[i].type == FD_NONE) { fd = i; break; }
    }
    if (fd < 0) { errno = EMFILE; return -1; }

    struct file_req req;
    memset(&req, 0, sizeof(req));
    req.cmd = FILE_CMD_OPEN;
    req.flags = (uint32_t)flags;
    strncpy(req.path, path, 255);
    req.path[255] = '\0';

    uint8_t reply_buf[8192];

    int r = sys_msg(pid, &req, sizeof(req), reply_buf, sizeof(reply_buf));
    if (r < 0) {
        if (r == -ESRCH) fs_pid_cache = -1;
        errno = -r;
        return -1;
    }

    struct file_resp *resp = (struct file_resp *)reply_buf;
    if (resp->status != 0) { errno = -resp->status; return -1; }

    fd_table[fd].type = FD_FILE;
    fd_table[fd].fs_fd = (int32_t)resp->fd;
    fd_table[fd].offset = 0;
    fd_table[fd].flags = flags;
    fd_table[fd].file_size = resp->file_size;
    return fd;
}

// ===================== read =====================

ssize_t file_read(int fd, void *buf, size_t count) {
    if ((fd_table[fd].flags & O_WRONLY) && !(fd_table[fd].flags & O_RDWR))
        { errno = EBADF; return -1; }

    pid_t pid = get_fs_pid();
    if (pid < 0) { errno = ENOENT; return -1; }

    size_t chunk = count > 65536 ? 65536 : count;

    struct file_req req;
    memset(&req, 0, sizeof(req));
    req.cmd = FILE_CMD_READ;
    req.fs_fd = (uint32_t)fd_table[fd].fs_fd;
    req.offset = fd_table[fd].offset;
    req.count = (uint32_t)chunk;

    size_t reply_len = sizeof(struct file_resp) + chunk;
    uint8_t *reply_buf = (uint8_t *)malloc(reply_len);
    if (!reply_buf) { errno = ENOMEM; return -1; }

    int r = sys_msg(pid, &req, sizeof(req), reply_buf, reply_len);
    if (r < 0) {
        if (r == -ESRCH) fs_pid_cache = -1;
        free(reply_buf);
        errno = -r;
        return -1;
    }

    struct file_resp *resp = (struct file_resp *)reply_buf;
    if (resp->status != 0) {
        free(reply_buf);
        errno = -resp->status;
        return -1;
    }

    memcpy(buf, resp->data, resp->count);
    size_t nread = resp->count;
    fd_table[fd].offset += nread;
    free(reply_buf);
    return (ssize_t)nread;
}

// ===================== write (file — ENOSYS stub) =====================

ssize_t file_write(int fd, const void *buf, size_t count) {
    (void)fd; (void)buf; (void)count;
    errno = ENOSYS;
    return -1;
}

// ===================== close (file) =====================

int file_close(int fd) {
    pid_t pid = get_fs_pid();
    if (pid >= 0) {
        struct file_req req;
        memset(&req, 0, sizeof(req));
        req.cmd = FILE_CMD_CLOSE;
        req.fs_fd = (uint32_t)fd_table[fd].fs_fd;

        struct file_resp resp;
        sys_msg(pid, &req, sizeof(req), &resp, sizeof(resp));
    }
    fd_table[fd].type = FD_NONE;
    return 0;
}

// ===================== pipe =====================

int pipe(int fd[2]) {
    fd_table_init();

    int r = sys_pipe(fd);
    if (r < 0) { errno = -r; return -1; }
    fd_table[fd[0]].type = FD_PIPE;
    fd_table[fd[0]].flags = O_RDONLY;
    fd_table[fd[1]].type = FD_PIPE;
    fd_table[fd[1]].flags = O_WRONLY;
    return 0;
}

// ===================== read (dispatch) =====================

ssize_t read(int fd, void *buf, size_t count) {
    fd_table_init();

    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }

    if (fd_table[fd].type == FD_PIPE) {
        int64_t n = sys_read(fd, buf, count);
        if (n < 0) { errno = (int)(-n); return -1; }
        return (ssize_t)n;
    }

    return file_read(fd, buf, count);
}

// ===================== write (dispatch) =====================

ssize_t write(int fd, const void *buf, size_t count) {
    fd_table_init();

    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }

    if (fd_table[fd].type == FD_PIPE) {
        int64_t n = sys_write(fd, buf, count);
        if (n < 0) { errno = (int)(-n); return -1; }
        return (ssize_t)n;
    }

    return file_write(fd, buf, count);
}

// ===================== close (dispatch) =====================

int close(int fd) {
    fd_table_init();

    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }

    if (fd_table[fd].type == FD_PIPE) {
        int r = sys_close(fd);
        if (r < 0) { errno = -r; return -1; }
    } else {
        file_close(fd);
    }
    fd_table[fd].type = FD_NONE;
    return 0;
}

int fcntl(int fd, int cmd, ...) {
    fd_table_init();

    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }

    if (cmd == F_GETFL) {
        return fd_table[fd].flags;
    } else if (cmd == F_SETFL) {
        va_list ap;
        va_start(ap, cmd);
        int arg = va_arg(ap, int);
        va_end(ap);
        // If FD_PIPE, update kernel-side flags too
        if (fd_table[fd].type == FD_PIPE) {
            int r = sys_fcntl(fd, cmd, arg);
            if (r < 0) { errno = -r; return -1; }
        }
        fd_table[fd].flags = arg;
        return 0;
    }

    errno = EINVAL;
    return -1;
}

// stat — get file status via fs_driver
int stat(const char *path, struct stat *st) {
    fd_table_init();

    if (!path || !st) { errno = EFAULT; return -1; }

    int32_t fs_pid = get_fs_pid();
    if (fs_pid <= 0) { errno = ENOENT; return -1; }

    // FILE_CMD_STAT = 9
    struct {
        uint32_t cmd;
        char path[256];
    } freq;
    freq.cmd = 9;
    int i;
    for (i = 0; path[i] && i < 255; i++)
        freq.path[i] = path[i];
    freq.path[i] = '\0';

    struct {
        uint32_t status;
        uint64_t size;
    } fresp;

    int r = sys_msg(fs_pid, &freq, sizeof(freq), &fresp, sizeof(fresp));
    if (r < 0) { errno = -r; return -1; }
    if (fresp.status != 0) { errno = (int)fresp.status; return -1; }

    st->st_size = (off_t)fresp.size;
    return 0;
}

// dup2 — duplicate fd with libc fd_table sync
int dup2(int old_fd, int new_fd) {
    fd_table_init();

    int r = sys_dup2(old_fd, new_fd);
    if (r < 0) { errno = -r; return -1; }

    // Sync libc fd_table: copy old_fd entry to new_fd
    if (new_fd >= 0 && new_fd < MAX_FD && old_fd >= 0 && old_fd < MAX_FD) {
        fd_table[new_fd] = fd_table[old_fd];
    }
    return r;
}
