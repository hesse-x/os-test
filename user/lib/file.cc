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
#include "common/shm.h"

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

    // If O_CREAT and open fails with ENOENT, create the file first
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
    if (resp->status != 0 && (flags & O_CREAT) && resp->status == -ENOENT) {
        // File doesn't exist and O_CREAT is set — create it first
        struct file_req creq;
        memset(&creq, 0, sizeof(creq));
        creq.cmd = FILE_CMD_CREATE;
        strncpy(creq.path, path, 255);
        creq.path[255] = '\0';

        int cr = msg_fd(fs_fd, &creq, sizeof(creq), reply_buf, sizeof(reply_buf));
        if (cr < 0) {
            if (cr == -ESRCH) { close(fs_dev_fd); fs_dev_fd = -1; }
            errno = -cr;
            return -1;
        }
        resp = (struct file_resp *)reply_buf;
        if (resp->status != 0 && resp->status != -EEXIST) {
            errno = -resp->status;
            return -1;
        }

        // File created — retry open
        memset(&req, 0, sizeof(req));
        req.cmd = FILE_CMD_OPEN;
        req.flags = (uint32_t)flags;
        strncpy(req.path, path, 255);
        req.path[255] = '\0';

        r = msg_fd(fs_fd, &req, sizeof(req), reply_buf, sizeof(reply_buf));
        if (r < 0) {
            if (r == -ESRCH) { close(fs_dev_fd); fs_dev_fd = -1; }
            errno = -r;
            return -1;
        }
        resp = (struct file_resp *)reply_buf;
    }

    if (resp->status != 0) { errno = -resp->status; return -1; }

    // Register FD_FILE in kernel fd_table via sys_install_fd
    // Strip create/trunc flags — they only matter at open time, not for the kernel fd
    int install_flags = flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_NONBLOCK);
    pid_t fs_pid = fd_table[fs_fd].target_pid;
    int fd = sys_install_fd(fs_pid, (int32_t)resp->fd, 0, install_flags, resp->file_size);
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

// ===================== FD_DEV helpers =====================

// Get target_pid for an FD_DEV fd (used by mmap/sys_mman.cc)
pid_t __fd_dev_target_pid(int fd) {
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type != FD_DEV)
        return -1;
    return fd_table[fd].target_pid;
}

// req_fd — send REQ to device driver via fd
int req_fd(int fd, void *req_ptr, void *resp) {
    return sys_dev_req(fd, req_ptr, resp);
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
    pid_t target_pid = fd_table[fd].target_pid;
    return sys_msg(target_pid, (void*)msg_buf, msg_len, reply_buf, reply_len);
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

// ===================== getcwd =====================
char *getcwd(char *buf, size_t size) {
    if (!buf) { errno = EFAULT; return NULL; }
    const char *cwd = __get_cwd();
    int len = 0;
    while (cwd[len]) len++;
    if ((size_t)len >= size) { errno = ERANGE; return NULL; }
    int i;
    for (i = 0; cwd[i]; i++) buf[i] = cwd[i];
    buf[i] = '\0';
    return buf;
}

// ===================== lseek =====================
off_t lseek(int fd, off_t offset, int whence) {
    fd_table_init();
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }
    if (fd_table[fd].type == FD_FILE) {
        // FD_FILE: use kernel sys_lseek
        int64_t r = sys_lseek(fd, offset, whence);
        if (r < 0) { errno = (int)(-r); return -1; }
        return (off_t)r;
    }
    if (fd_table[fd].type == FD_PIPE) {
        errno = ESPIPE;
        return -1;
    }
    errno = EINVAL;
    return -1;
}

// ===================== stat (updated for full struct stat) =====================
int stat(const char *path, struct stat *st) {
    fd_table_init();
    if (!path || !st) { errno = EFAULT; return -1; }

    int fs_fd = get_fs_dev_fd();
    if (fs_fd < 0) { errno = ENOENT; return -1; }

    // Resolve path to absolute
    char abs_path[256];
    if (path[0] != '/') {
        int cwdi = 0;
        while (cwd_path[cwdi] && cwdi < 254) {
            abs_path[cwdi] = cwd_path[cwdi];
            cwdi++;
        }
        if (cwdi > 0 && abs_path[cwdi-1] != '/') abs_path[cwdi++] = '/';
        int pi = 0;
        while (path[pi] && cwdi < 254) abs_path[cwdi++] = path[pi++];
        abs_path[cwdi] = '\0';
        path = abs_path;
    }

    struct {
        uint32_t cmd;
        char path[256];
    } freq;
    memset(&freq, 0, sizeof(freq));
    freq.cmd = 9; // FILE_CMD_STAT

    int i;
    for (i = 0; path[i] && i < 255; i++)
        freq.path[i] = path[i];
    freq.path[i] = '\0';

    struct {
        int32_t  status;
        uint32_t fd;
        uint64_t file_size;
        uint32_t count;
        uint32_t total;
    } fresp;

    int r = msg_fd(fs_fd, &freq, sizeof(freq), &fresp, sizeof(fresp));
    if (r < 0) { errno = -r; return -1; }
    if (fresp.status != 0) { errno = (int)fresp.status; return -1; }

    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)fresp.file_size;
    // Default mode: regular file with 0644
    st->st_mode = S_IFREG | 0644;
    st->st_blksize = 512;
    st->st_blocks = (blkcnt_t)((fresp.file_size + 511) / 512);
    st->st_nlink = 1;
    return 0;
}

// ===================== access =====================
int access(const char *path, int mode) {
    fd_table_init();
    if (!path) { errno = EFAULT; return -1; }

    // F_OK: just check if file exists via stat
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    (void)mode; // ignore R_OK/W_OK/X_OK — no permissions in FAT32
    return 0;
}

// ===================== unlink =====================
int unlink(const char *path) {
    fd_table_init();
    if (!path) { errno = EFAULT; return -1; }

    char abs_path[256];
    if (path[0] != '/') {
        int cwdi = 0;
        while (cwd_path[cwdi] && cwdi < 254) { abs_path[cwdi] = cwd_path[cwdi]; cwdi++; }
        if (cwdi > 0 && abs_path[cwdi-1] != '/') abs_path[cwdi++] = '/';
        int pi = 0;
        while (path[pi] && cwdi < 254) abs_path[cwdi++] = path[pi++];
        abs_path[cwdi] = '\0';
        path = abs_path;
    }

    int fs_fd = get_fs_dev_fd();
    if (fs_fd < 0) { errno = ENOENT; return -1; }

    struct {
        uint32_t cmd;
        char path[256];
        uint32_t flags;
        uint32_t fs_fd;
        uint64_t offset;
        uint32_t count;
        uint32_t lba;
        uint32_t readdir_offset;
        uint32_t readdir_count;
    } req;
    memset(&req, 0, sizeof(req));
    req.cmd = 10; // FILE_CMD_UNLINK
    int i;
    for (i = 0; path[i] && i < 255; i++) req.path[i] = path[i];
    req.path[i] = '\0';

    struct file_resp resp_buf;
    int r = msg_fd(fs_fd, &req, sizeof(req), &resp_buf, sizeof(resp_buf));
    if (r < 0) { errno = -r; return -1; }
    if (resp_buf.status != 0) { errno = (int)resp_buf.status; return -1; }
    return 0;
}

// ===================== rmdir =====================
int rmdir(const char *path) {
    fd_table_init();
    if (!path) { errno = EFAULT; return -1; }

    char abs_path[256];
    if (path[0] != '/') {
        int cwdi = 0;
        while (cwd_path[cwdi] && cwdi < 254) { abs_path[cwdi] = cwd_path[cwdi]; cwdi++; }
        if (cwdi > 0 && abs_path[cwdi-1] != '/') abs_path[cwdi++] = '/';
        int pi = 0;
        while (path[pi] && cwdi < 254) abs_path[cwdi++] = path[pi++];
        abs_path[cwdi] = '\0';
        path = abs_path;
    }

    int fs_fd = get_fs_dev_fd();
    if (fs_fd < 0) { errno = ENOENT; return -1; }

    struct {
        uint32_t cmd;
        char path[256];
        uint32_t flags;
        uint32_t fs_fd;
        uint64_t offset;
        uint32_t count;
        uint32_t lba;
        uint32_t readdir_offset;
        uint32_t readdir_count;
    } req;
    memset(&req, 0, sizeof(req));
    req.cmd = 11; // FILE_CMD_RMDIR
    int i;
    for (i = 0; path[i] && i < 255; i++) req.path[i] = path[i];
    req.path[i] = '\0';

    struct file_resp resp_buf;
    int r = msg_fd(fs_fd, &req, sizeof(req), &resp_buf, sizeof(resp_buf));
    if (r < 0) { errno = -r; return -1; }
    if (resp_buf.status != 0) { errno = (int)resp_buf.status; return -1; }
    return 0;
}

// ===================== isatty =====================
int isatty(int fd) {
    fd_table_init();
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE) return 0;
    if (fd_table[fd].type == FD_DEV && fd_table[fd].target_pid == DEV_TERMINAL)
        return 1;
    return 0;
}

// ===================== fstat =====================
int fstat(int fd, struct stat *st) {
    fd_table_init();
    if (!st) { errno = EFAULT; return -1; }
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type == FD_NONE)
        { errno = EBADF; return -1; }

    memset(st, 0, sizeof(*st));

    if (fd_table[fd].type == FD_FILE) {
        // For FD_FILE, get file_size from libc fd_table
        st->st_size = (off_t)fd_file_size(fd);
        st->st_mode = S_IFREG | 0644;
    } else if (fd_table[fd].type == FD_DEV) {
        st->st_mode = S_IFCHR | 0666;
        st->st_size = 0;
    } else if (fd_table[fd].type == FD_PIPE) {
        st->st_mode = S_IFIFO | 0644;
        st->st_size = 0;
    }

    st->st_blksize = 512;
    st->st_blocks = (blkcnt_t)((st->st_size + 511) / 512);
    st->st_nlink = 1;
    return 0;
}

// ===================== mkdir (with mode_t, ignores mode) =====================
int mkdir(const char *path, mode_t mode) {
    (void)mode; // FAT32 doesn't support permissions
    fd_table_init();
    if (!path) { errno = EFAULT; return -1; }

    // Resolve path
    char abs_path[256];
    if (path[0] != '/') {
        int cwdi = 0;
        while (cwd_path[cwdi] && cwdi < 254) { abs_path[cwdi] = cwd_path[cwdi]; cwdi++; }
        if (cwdi > 0 && abs_path[cwdi-1] != '/') abs_path[cwdi++] = '/';
        int pi = 0;
        while (path[pi] && cwdi < 254) abs_path[cwdi++] = path[pi++];
        abs_path[cwdi] = '\0';
        path = abs_path;
    }

    int fs_fd = get_fs_dev_fd();
    if (fs_fd < 0) { errno = ENOENT; return -1; }

    struct {
        uint32_t cmd;
        char path[256];
        uint32_t flags;
        uint32_t fs_fd;
        uint64_t offset;
        uint32_t count;
        uint32_t lba;
        uint32_t readdir_offset;
        uint32_t readdir_count;
    } req;
    memset(&req, 0, sizeof(req));
    req.cmd = 7; // FILE_CMD_MKDIR
    int i;
    for (i = 0; path[i] && i < 255; i++) req.path[i] = path[i];
    req.path[i] = '\0';

    struct file_resp resp_buf;
    int r = msg_fd(fs_fd, &req, sizeof(req), &resp_buf, sizeof(resp_buf));
    if (r < 0) { errno = -r; return -1; }
    if (resp_buf.status != 0) { errno = (int)resp_buf.status; return -1; }
    return 0;
}

// ===================== opendir / readdir / closedir =====================
#include <dirent.h>
#include <stdlib.h>

DIR *opendir(const char *name) {
    fd_table_init();
    if (!name) { errno = EFAULT; return NULL; }

    // Resolve path
    char abs_path[256];
    if (name[0] != '/') {
        int cwdi = 0;
        while (cwd_path[cwdi] && cwdi < 254) { abs_path[cwdi] = cwd_path[cwdi]; cwdi++; }
        if (cwdi > 0 && abs_path[cwdi-1] != '/') abs_path[cwdi++] = '/';
        int pi = 0;
        while (name[pi] && cwdi < 254) abs_path[cwdi++] = name[pi++];
        abs_path[cwdi] = '\0';
        name = abs_path;
    }

    int fs_fd = get_fs_dev_fd();
    if (fs_fd < 0) { errno = ENOENT; return NULL; }

    struct {
        uint32_t cmd;
        char path[256];
        uint32_t flags;
        uint32_t fs_fd;
        uint64_t offset;
        uint32_t count;
        uint32_t lba;
        uint32_t readdir_offset;
        uint32_t readdir_count;
    } req;
    memset(&req, 0, sizeof(req));
    req.cmd = 13; // FILE_CMD_OPENDIR
    int i;
    for (i = 0; name[i] && i < 255; i++) req.path[i] = name[i];
    req.path[i] = '\0';

    struct {
        int32_t  status;
        uint32_t fd;
        uint64_t file_size;
        uint32_t count;
        uint32_t total;
    } resp;

    int r = msg_fd(fs_fd, &req, sizeof(req), &resp, sizeof(resp));
    if (r < 0) { errno = -r; return NULL; }
    if (resp.status != 0) { errno = (int)resp.status; return NULL; }

    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) { errno = ENOMEM; return NULL; }
    dir->dd_fd = (int)resp.fd;
    return dir;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) { errno = EBADF; return NULL; }

    int fs_fd = get_fs_dev_fd();
    if (fs_fd < 0) { errno = ENOENT; return NULL; }

    struct {
        uint32_t cmd;
        char path[256];
        uint32_t flags;
        uint32_t fs_fd;
        uint64_t offset;
        uint32_t count;
        uint32_t lba;
        uint32_t readdir_offset;
        uint32_t readdir_count;
    } req;
    memset(&req, 0, sizeof(req));
    req.cmd = 14; // FILE_CMD_DIRENT
    req.fs_fd = (uint32_t)dirp->dd_fd;

    // Allocate space for the fs_dirent response
    struct {
        int32_t  status;
        uint32_t fd;
        uint64_t file_size;
        uint32_t count;
        uint32_t total;
        uint8_t  data[sizeof(struct dirent) + 32];
    } resp;

    int r = msg_fd(fs_fd, &req, sizeof(req), &resp, sizeof(resp));
    if (r < 0) { errno = -r; return NULL; }
    if (resp.status != 0) { errno = (int)resp.status; return NULL; }
    if (resp.count < sizeof(fs_dirent)) return NULL; // EOF

    // Convert fs_dirent to struct dirent
    fs_dirent *fde = (fs_dirent *)resp.data;
    static struct dirent result;
    result.d_ino = 0;
    int j = 0;
    while (fde->name[j] && j < 255) { result.d_name[j] = fde->name[j]; j++; }
    result.d_name[j] = '\0';
    return &result;
}

int closedir(DIR *dirp) {
    if (!dirp) { errno = EBADF; return -1; }

    int fs_fd = get_fs_dev_fd();
    if (fs_fd < 0) { errno = ENOENT; return -1; }

    struct {
        uint32_t cmd;
        char path[256];
        uint32_t flags;
        uint32_t fs_fd;
        uint64_t offset;
        uint32_t count;
        uint32_t lba;
        uint32_t readdir_offset;
        uint32_t readdir_count;
    } req;
    memset(&req, 0, sizeof(req));
    req.cmd = 15; // FILE_CMD_CLOSEDIR
    req.fs_fd = (uint32_t)dirp->dd_fd;

    msg_fd(fs_fd, &req, sizeof(req), NULL, 0);
    free(dirp);
    return 0;
}
