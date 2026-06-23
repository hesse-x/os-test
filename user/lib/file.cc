// libc file I/O: fd_table + open/read/write/close/pipe dispatch
// VFS unified: all file operations go through syscalls directly.
// Kernel handles FAT32, devtmpfs, pipes, sockets, etc.
// FD_DEV: device driver interaction via req/notify/msg IPC.
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
    fd_table[2].type = FD_PIPE;
    fd_table[2].flags = O_WRONLY;
    fd_table_inited = true;
}

uint64_t fd_file_size(int fd) {
    fd_table_init();
    if (fd < 0 || fd >= MAX_FD || fd_table[fd].type != FD_FILE)
        return 0;
    // Use sys_lseek(SEEK_END) to get file size from kernel
    int64_t size = sys_lseek(fd, 0, SEEK_END);
    if (size < 0) return 0;
    // Restore position to beginning
    sys_lseek(fd, 0, SEEK_SET);
    return (uint64_t)size;
}

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

    // /dev/ paths use sys_open_dev which returns target_pid
    if (path && path[0] == '/' && path[1] == 'd' && path[2] == 'e' &&
        path[3] == 'v' && path[4] == '/') {
        const char *devname = path + 5;  // skip "/dev/"
        int dev_type = DEV_NONE;
        if (strcmp(devname, "kbd") == 0) dev_type = DEV_KBD;
        else if (strcmp(devname, "kms") == 0) dev_type = DEV_KMS;
        else if (strcmp(devname, "terminal") == 0) dev_type = DEV_TERMINAL;
        else if (strcmp(devname, "serial") == 0) dev_type = DEV_SERIAL;
        else { errno = ENOENT; return -1; }

        // sys_open_dev returns packed (fd | target_pid << 32)
        uint64_t result = sys_open_dev(dev_type);
        int32_t fd = (int32_t)(result & 0xFFFFFFFFULL);
        if (fd < 0) { errno = -fd; return -1; }
        pid_t target_pid = (pid_t)(result >> 32);

        fd_table[fd].type = FD_DEV;
        fd_table[fd].flags = O_RDWR;
        fd_table[fd].target_pid = target_pid;
        return fd;
    }

    // Regular file path: sys_open handles FAT32
    int fd = sys_open(path, flags);
    if (fd < 0) { errno = -fd; return -1; }

    fd_table[fd].type = FD_FILE;
    fd_table[fd].flags = flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_NONBLOCK);
    fd_table[fd].file_size = 0;

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

    // FD_PIPE, FD_FILE, FD_DIR: unified sys_read, kernel dispatches internally
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

// ===================== stat (via sys_stat syscall) =====================
int stat(const char *path, struct stat *st) {
    fd_table_init();
    if (!path || !st) { errno = EFAULT; return -1; }

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

    int r = sys_stat(path, st);
    if (r < 0) { errno = -r; return -1; }
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

// ===================== unlink (via sys_unlink syscall) =====================
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

    int r = sys_unlink(path);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

// ===================== rmdir (via sys_rmdir syscall) =====================
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

    int r = sys_rmdir(path);
    if (r < 0) { errno = -r; return -1; }
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
        // Use sys_lseek(SEEK_END) to get file size from kernel
        int64_t size = sys_lseek(fd, 0, SEEK_END);
        sys_lseek(fd, 0, SEEK_SET);
        st->st_size = (size > 0) ? (off_t)size : 0;
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

// ===================== mkdir (via sys_mkdir syscall) =====================
int mkdir(const char *path, mode_t mode) {
    (void)mode; // FAT32 doesn't support permissions
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

    int r = sys_mkdir(path, 0);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

// ===================== opendir / readdir / closedir =====================
#include <dirent.h>
#include <stdlib.h>
#include "common/dirent.h"

#define GETDENTS_BUF_SIZE 4096

struct __dir {
    int    dd_fd;
    uint8_t *buf;
    size_t  buf_pos;
    size_t  buf_len;
};

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

    int fd = open(name, O_RDONLY);
    if (fd < 0) return NULL;

    struct __dir *dir = (struct __dir *)calloc(1, sizeof(struct __dir));
    if (!dir) { close(fd); errno = ENOMEM; return NULL; }

    dir->buf = (uint8_t *)malloc(GETDENTS_BUF_SIZE);
    if (!dir->buf) { close(fd); free(dir); errno = ENOMEM; return NULL; }

    dir->dd_fd = fd;
    dir->buf_pos = 0;
    dir->buf_len = 0;
    return (DIR *)dir;
}

struct dirent *readdir(DIR *dirp) {
    struct __dir *dir = (struct __dir *)dirp;
    if (!dir) { errno = EBADF; return NULL; }

    // If buffer is exhausted, refill via sys_getdents
    if (dir->buf_pos >= dir->buf_len) {
        int n = sys_getdents(dir->dd_fd, dir->buf, GETDENTS_BUF_SIZE);
        if (n <= 0) return NULL;  // EOF or error
        dir->buf_pos = 0;
        dir->buf_len = (size_t)n;
    }

    // Parse current dirent64 entry
    struct dirent64 *d64 = (struct dirent64 *)(dir->buf + dir->buf_pos);
    static struct dirent result;
    result.d_ino = (ino_t)d64->d_ino;
    int j = 0;
    while (d64->d_name[j] && j < 255) { result.d_name[j] = d64->d_name[j]; j++; }
    result.d_name[j] = '\0';

    dir->buf_pos += d64->d_reclen;
    return &result;
}

int closedir(DIR *dirp) {
    struct __dir *dir = (struct __dir *)dirp;
    if (!dir) { errno = EBADF; return -1; }

    close(dir->dd_fd);
    free(dir->buf);
    free(dir);
    return 0;
}
