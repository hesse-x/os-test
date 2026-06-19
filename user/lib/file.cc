// libc file I/O: fd_table + open/read/write/close/pipe dispatch
// VFS unified: all fd types dispatch through sys_read/sys_write/sys_close in kernel.
// FD_FILE: kernel proxies to fs_driver via kernel_msg_send.
// FD_PIPE: kernel direct pipe ring buffer.
// FD_DEV: kernel sys_open_dev path.
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
#include <sys/poll.h>
#include "common/syscall.h"
#include "common/dev.h"

#define MAX_FD  32

enum fd_type_t { FD_NONE = 0, FD_PIPE, FD_FILE, FD_DEV };

struct file_fd_entry {
    enum fd_type_t type;    // FD_NONE / FD_PIPE / FD_FILE / FD_DEV
    int     flags;          // O_RDONLY / O_WRONLY / O_RDWR
    union {
        // FD_FILE only:
        uint64_t file_size;  // file size (set by open)
        // FD_DEV only:
        pid_t target_pid;    // driver PID
    };
};

static struct file_fd_entry fd_table[MAX_FD];

// ===================== Working directory (per-process) =====================
static char cwd_path[256] = "/";

const char *__get_cwd(void) { return cwd_path; }

// Auto-init fd 0/1 as FD_PIPE (stdin/stdout)
static bool fd_table_inited;

static void fd_table_init() {
    if (fd_table_inited) return;
    memset(fd_table, 0, sizeof(fd_table));
    for (int i = 0; i < MAX_FD; i++) {
        fd_table[i].type = FD_NONE;
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

// Lazy fs_driver fd cache (opened via /dev/fs)
static int fs_dev_fd = -1;

static int get_fs_dev_fd() {
    if (fs_dev_fd < 0) {
        fs_dev_fd = open("/dev/fs", O_RDWR);
        if (fs_dev_fd < 0) return -1;
    }
    return fs_dev_fd;
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

    // Handle relative paths by prepending cwd
    char abs_path[256];
    if (path && path[0] != '/') {
        abs_path[0] = '\0';
        int cwdi = 0;
        while (cwd_path[cwdi] && cwdi < 254) {
            abs_path[cwdi] = cwd_path[cwdi];
            cwdi++;
        }
        if (cwdi > 0 && abs_path[cwdi-1] != '/') {
            abs_path[cwdi++] = '/';
        }
        int pi = 0;
        while (path[pi] && cwdi < 254) {
            abs_path[cwdi++] = path[pi++];
        }
        abs_path[cwdi] = '\0';
        path = abs_path;
    }

    // Check for /dev/ prefix — open device node instead of file (no fs_driver needed)
    if (path && path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/') {
        const char *devname = path + 5;  // skip "/dev/"
        int dev_type = DEV_NONE;
        // Map device name to dev_type
        if (strcmp(devname, "kbd") == 0) dev_type = DEV_KBD;
        else if (strcmp(devname, "kms") == 0) dev_type = DEV_KMS;
        else if (strcmp(devname, "fs") == 0) dev_type = DEV_FS;
        else if (strcmp(devname, "terminal") == 0) dev_type = DEV_TERMINAL;
        else { errno = ENOENT; return -1; }

        // sys_open_dev now returns packed (fd | target_pid << 32)
        uint64_t result = sys_open_dev(dev_type);
        int32_t fd = (int32_t)(result & 0xFFFFFFFFULL);
        if (fd < 0) { errno = -fd; return -1; }
        pid_t target_pid = (pid_t)(result >> 32);

        // Record FD_DEV in libc fd_table
        fd_table[fd].type = FD_DEV;
        fd_table[fd].flags = O_RDWR;
        fd_table[fd].target_pid = target_pid;

        return fd;
    }

    // Normal file path: talk to fs_driver, then register FD_FILE via sys_install_fd
    int local_fd = -1;
    for (int i = 3; i < MAX_FD; i++) {
        if (fd_table[i].type == FD_NONE) { local_fd = i; break; }
    }
    if (local_fd < 0) { errno = EMFILE; return -1; }

    int fs_fd = get_fs_dev_fd();
    if (fs_fd < 0) { errno = ENOENT; return -1; }

    struct file_req req;
    memset(&req, 0, sizeof(req));
    req.cmd = FILE_CMD_OPEN;
    req.flags = (uint32_t)flags;
    strncpy(req.path, path, 255);
    req.path[255] = '\0';

    uint8_t reply_buf[8192];

    int r = msg_fd(fs_fd, &req, sizeof(req), reply_buf, sizeof(reply_buf));
    if (r < 0) {
        if (r == -ESRCH) { close(fs_dev_fd); fs_dev_fd = -1; }
        errno = -r;
        return -1;
    }

    struct file_resp *resp = (struct file_resp *)reply_buf;
    if (resp->status != 0) { errno = -resp->status; return -1; }

    // Register FD_FILE in kernel fd_table via sys_install_fd
    pid_t fs_pid = fd_table[fs_fd].target_pid;
    int fd = sys_install_fd(fs_pid, (int32_t)resp->fd, 0, flags, resp->file_size);
    if (fd < 0) {
        // Registration failed — tell fs_driver to close the session fd
        struct file_req close_req;
        memset(&close_req, 0, sizeof(close_req));
        close_req.cmd = FILE_CMD_CLOSE;
        close_req.fs_fd = resp->fd;
        msg_fd(fs_fd, &close_req, sizeof(close_req), NULL, 0);
        errno = -fd;
        return -1;
    }

    // Record FD_FILE in libc fd_table
    fd_table[fd].type = FD_FILE;
    fd_table[fd].flags = flags;
    fd_table[fd].file_size = resp->file_size;
    return fd;
}

// ===================== read (unified — kernel dispatches) =====================

ssize_t read(int fd, void *buf, size_t count) {
    fd_table_init();

    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }

    if (fd_table[fd].type == FD_DEV) {
        // Device fds use req/notify, not read/write
        errno = ENOSYS;
        return -1;
    }

    // FD_PIPE and FD_FILE: unified sys_read, kernel dispatches internally
    int64_t n = sys_read(fd, buf, count);
    if (n < 0) { errno = (int)(-n); return -1; }
    return (ssize_t)n;
}

// ===================== write (unified — kernel dispatches) =====================

ssize_t write(int fd, const void *buf, size_t count) {
    fd_table_init();

    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }

    if (fd_table[fd].type == FD_DEV) {
        errno = ENOSYS;
        return -1;
    }

    // FD_PIPE and FD_FILE: unified sys_write, kernel dispatches internally
    int64_t n = sys_write(fd, buf, count);
    if (n < 0) { errno = (int)(-n); return -1; }
    return (ssize_t)n;
}

// ===================== close (unified — kernel dispatches) =====================

int close(int fd) {
    fd_table_init();

    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }

    // All fd types go through sys_close — kernel handles dispatch and cleanup
    int r = sys_close(fd);
    if (r < 0) { errno = -r; return -1; }
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

// ===================== chdir =====================

int chdir(const char *path) {
    fd_table_init();

    if (!path) { errno = EFAULT; return -1; }

    // Resolve path: if it doesn't start with '/', prepend cwd
    char abs_path[256];
    if (path[0] != '/') {
        int cwdi = 0;
        while (cwd_path[cwdi] && cwdi < 254) {
            abs_path[cwdi] = cwd_path[cwdi];
            cwdi++;
        }
        if (cwdi > 0 && abs_path[cwdi-1] != '/') {
            abs_path[cwdi++] = '/';
        }
        int pi = 0;
        while (path[pi] && cwdi < 254) {
            abs_path[cwdi++] = path[pi++];
        }
        abs_path[cwdi] = '\0';
        path = abs_path;
    }

    // Normalize: remove trailing slash (except for "/")
    char norm[256];
    int ni = 0;
    while (path[ni] && ni < 255) {
        norm[ni] = path[ni];
        ni++;
    }
    norm[ni] = '\0';
    while (ni > 1 && norm[ni-1] == '/') {
        norm[ni-1] = '\0';
        ni--;
    }

    // Validate path exists via stat
    struct stat st;
    if (stat(norm, &st) != 0) return -1;

    // Update cwd
    int i = 0;
    while (norm[i] && i < 255) {
        cwd_path[i] = norm[i];
        i++;
    }
    cwd_path[i] = '\0';
    return 0;
}

// ===================== fcntl =====================

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
        // If FD_PIPE or FD_FILE, update kernel-side flags too
        if (fd_table[fd].type == FD_PIPE || fd_table[fd].type == FD_FILE) {
            int r = sys_fcntl(fd, cmd, arg);
            if (r < 0) { errno = -r; return -1; }
        }
        fd_table[fd].flags = arg;
        return 0;
    }

    errno = EINVAL;
    return -1;
}

// ===================== stat =====================
// stat — get file status via fs_driver
int stat(const char *path, struct stat *st) {
    fd_table_init();

    if (!path || !st) { errno = EFAULT; return -1; }

    int fs_fd = get_fs_dev_fd();
    if (fs_fd < 0) { errno = ENOENT; return -1; }

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
        int32_t  status;
        uint64_t size;
    } fresp;

    int r = msg_fd(fs_fd, &freq, sizeof(freq), &fresp, sizeof(fresp));
    if (r < 0) { errno = -r; return -1; }
    if (fresp.status != 0) { errno = (int)fresp.status; return -1; }

    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)fresp.size;
    // Default mode: assume regular file (S_IFREG) or directory based on path
    // For proper mode, fs_driver would need to return attr in stat response.
    // For now, directories just have a smaller heuristic.
    if (fresp.size == 0) {
        // Could be a directory or empty file — check if path ends with known dir pattern
    }
    return 0;
}

// ===================== FD_DEV helpers =====================

// Get target_pid for an FD_DEV fd (used by mmap/sys_mman.cc)
pid_t __fd_dev_target_pid(int fd) {
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type != FD_DEV)
        return -1;
    return fd_table[fd].target_pid;
}

// req_fd — send REQ to device driver via fd
int req_fd(int fd, void *req_ptr, void *resp) {
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type != FD_DEV) {
        errno = EBADF;
        return -1;
    }
    return sys_req(fd_table[fd].target_pid, req_ptr, resp);
}

// notify_fd — notify device driver via fd
int notify_fd(int fd) {
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type != FD_DEV) {
        errno = EBADF;
        return -1;
    }
    return sys_notify(fd_table[fd].target_pid);
}

// msg_fd — send variable-length message to device driver via fd
int msg_fd(int fd, const void *msg_buf, size_t msg_len,
           void *reply_buf, size_t reply_len) {
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type != FD_DEV) {
        errno = EBADF;
        return -1;
    }
    return sys_dev_msg(fd, (void*)msg_buf, msg_len, reply_buf, reply_len);
}

// poll — wait for events (kernel-implemented via SYS_POLL)
int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
    int64_t ret = __syscall3(SYS_POLL, (int64_t)(uintptr_t)fds,
        (int64_t)nfds, (int64_t)timeout_ms);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
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
