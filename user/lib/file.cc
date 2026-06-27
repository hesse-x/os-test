// libc file I/O: all file operations go through syscalls directly.
// Kernel handles FAT32, devtmpfs, pipes, sockets, etc.
// No libc-side fd_table — kernel's proc->fd_table is the single source of truth.
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
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

// ===================== Working directory (per-process) =====================
static char cwd_path[256] = "/";

const char *__get_cwd(void) { return cwd_path; }

uint64_t fd_file_size(int fd) {
    // Use sys_lseek(SEEK_END) to get file size from kernel
    int64_t size = sys_lseek(fd, 0, SEEK_END);
    if (size < 0) return 0;
    // Restore position to beginning
    sys_lseek(fd, 0, SEEK_SET);
    return (uint64_t)size;
}

// ===================== open =====================

int open(const char *path, int flags, ...) {
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

    // All paths go through sys_open — kernel dispatches FAT32 and devtmpfs internally
    uint64_t result = sys_open(path, flags);
    int32_t fd = (int32_t)(result & 0xFFFFFFFFULL);
    if (fd < 0) { errno = -fd; return -1; }

    return fd;
}

// ===================== read =====================

ssize_t read(int fd, void *buf, size_t count) {
    int64_t n = sys_read(fd, buf, count);
    if (n < 0) { errno = (int)(-n); return -1; }
    return (ssize_t)n;
}

// ===================== write =====================

ssize_t write(int fd, const void *buf, size_t count) {
    int64_t n = sys_write(fd, buf, count);
    if (n < 0) { errno = (int)(-n); return -1; }
    return (ssize_t)n;
}

// ===================== close =====================

int close(int fd) {
    int r = sys_close(fd);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

// ===================== pipe =====================

int pipe(int fd[2]) {
    int r = sys_pipe(fd);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

// ===================== chdir =====================

int chdir(const char *path) {
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
    if (cmd == F_GETFL) {
        return sys_fcntl(fd, F_GETFL, 0);
    } else if (cmd == F_SETFL) {
        va_list ap;
        va_start(ap, cmd);
        int arg = va_arg(ap, int);
        va_end(ap);
        int r = sys_fcntl(fd, cmd, arg);
        if (r < 0) { errno = -r; return -1; }
        return 0;
    }

    errno = EINVAL;
    return -1;
}

// ===================== FD_DEV helpers =====================

// notify_fd — notify device driver via fd (uses sys_fdev_pid to find target)
int notify_fd(int fd) {
    pid_t target_pid = sys_fdev_pid(fd);
    if (target_pid < 0) { errno = EBADF; return -1; }
    if (target_pid == 0) { errno = ENODEV; return -1; }
    return sys_notify(target_pid);
}

// msg_fd — send variable-length message to device driver via fd
int msg_fd(int fd, const void *msg_buf, size_t msg_len,
           void *reply_buf, size_t reply_len) {
    pid_t target_pid = sys_fdev_pid(fd);
    if (target_pid < 0) { errno = EBADF; return -1; }
    if (target_pid == 0) { errno = ENODEV; return -1; }
    return sys_msg(target_pid, (void*)msg_buf, msg_len, reply_buf, reply_len);
}

// poll — wait for events (kernel-implemented via SYS_POLL)
int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
    int64_t ret = __syscall3(SYS_POLL, (int64_t)(uintptr_t)fds,
        (int64_t)nfds, (int64_t)timeout_ms);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

// dup2 — duplicate fd
int dup2(int old_fd, int new_fd) {
    int r = sys_dup2(old_fd, new_fd);
    if (r < 0) { errno = -r; return -1; }
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
    int64_t r = sys_lseek(fd, offset, whence);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (off_t)r;
}

// ===================== stat (via sys_stat syscall) =====================
int stat(const char *path, struct stat *st) {
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
    if (!path) { errno = EFAULT; return -1; }

    // F_OK: just check if file exists via stat
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    (void)mode; // ignore R_OK/W_OK/X_OK — no permissions in FAT32
    return 0;
}

// ===================== unlink (via sys_unlink syscall) =====================
int unlink(const char *path) {
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
    // Use ioctl TCGETS to detect tty devices
    long rc = sys_ioctl(fd, TCGETS, 0);
    return (rc == 0) ? 1 : 0;
}

// ===================== tcgetattr / tcsetattr =====================
int tcgetattr(int fd, struct termios *termios_p) {
    long rc = sys_ioctl(fd, TCGETS, (uint64_t)termios_p);
    if (rc < 0) { errno = (int)(-rc); return -1; }
    return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    uint32_t cmd;
    switch (optional_actions) {
    case TCSANOW:   cmd = TCSETS;  break;
    case TCSADRAIN: cmd = TCSETSW; break;
    case TCSAFLUSH: cmd = TCSETSF; break;
    default: errno = EINVAL; return -1;
    }
    long rc = sys_ioctl(fd, cmd, (uint64_t)termios_p);
    if (rc < 0) { errno = (int)(-rc); return -1; }
    return 0;
}

// ===================== ttyname =====================
char *ttyname(int fd) {
    if (!isatty(fd)) return NULL;
    static char name[32];
    int index = -1;
    long rc = sys_ioctl(fd, TIOCGPTN, (uint64_t)&index);
    if (rc < 0 || index < 0) return NULL;
    // Build "/dev/ptsN"
    const char *prefix = "/dev/pts";
    int pos = 0;
    for (int i = 0; prefix[i]; i++) name[pos++] = prefix[i];
    if (index == 0) {
        name[pos++] = '0';
    } else {
        char tmp[8]; int tpos = 0; int n = index;
        while (n > 0) { tmp[tpos++] = '0' + (n % 10); n /= 10; }
        for (int i = tpos - 1; i >= 0; i--) name[pos++] = tmp[i];
    }
    name[pos] = '\0';
    return name;
}

// ===================== ioctl =====================
int ioctl(int fd, uint32_t cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    uint64_t arg = va_arg(ap, uint64_t);
    va_end(ap);

    // Always use a 64B stack buffer for kernel communication.
    // For proxy path, kernel writes 56B reply into this buffer via sys_resp.
    // This avoids the caller's original arg being smaller than 56B.
    uint8_t buf[64];
    __builtin_memset(buf, 0, sizeof(buf));

    uint16_t arg_size = _IOC_SIZE(cmd);
    if (arg_size > 48) { errno = EINVAL; return -1; }

    // Copy-in: user arg → buf (only if direction includes WRITE)
    if ((_IOC_DIR(cmd) & _IOC_WRITE) && arg != 0 && arg_size > 0)
        __builtin_memcpy(buf, (const void *)arg, arg_size);

    long rc = sys_ioctl(fd, cmd, (uint64_t)(uintptr_t)buf);
    if (rc < 0) { errno = (int)(-rc); return -1; }

    // Copy-out: buf → user arg (only if direction includes READ)
    if ((_IOC_DIR(cmd) & _IOC_READ) && arg != 0 && arg_size > 0)
        __builtin_memcpy((void *)arg, buf, arg_size);

    return (int)rc;
}

// ===================== fstat =====================
int fstat(int fd, struct stat *st) {
    if (!st) { errno = EFAULT; return -1; }
    long rc = sys_fstat(fd, (uint64_t)st);
    if (rc < 0) { errno = (int)(-rc); return -1; }
    return 0;
}

// ===================== mkdir (via sys_mkdir syscall) =====================
int mkdir(const char *path, mode_t mode) {
    (void)mode; // FAT32 doesn't support permissions
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
#include "common/dirent.h"

#define GETDENTS_BUF_SIZE 4096

struct __dir {
    int    dd_fd;
    uint8_t *buf;
    size_t  buf_pos;
    size_t  buf_len;
};

DIR *opendir(const char *name) {
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
