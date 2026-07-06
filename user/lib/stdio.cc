/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>
#include <xos/errno.h>
#include <xos/fcntl.h>

#include "utils/kvformat.h"

/* ===================== sys_write based flush ===================== */

static void sys_write_flush(FILE *f, const char *data, int len) {
  sys_write(f->fd, data, len);
}

/* ===================== sys_read based fill ===================== */

static int sys_read_fill(FILE *f, char *buf, int len) {
  int64_t n = sys_read(f->fd, buf, len);
  if (n <= 0)
    return 0;
  return (int)n;
}

static FILE stdin_file = {0,
                          nullptr,
                          0,
                          0,
                          _IONBF,
                          _F_READ,
                          nullptr,
                          sys_read_fill,
                          0,
                          -1,
                          PTHREAD_MUTEX_INITIALIZER};

static char stdout_buf[256];
static FILE stdout_file = {
    1,      stdout_buf, sizeof(stdout_buf),       0,
    _IOLBF, _F_WRITE,   sys_write_flush,          nullptr,
    0,      -1,         PTHREAD_MUTEX_INITIALIZER};

static FILE stderr_file = {2,
                           nullptr,
                           0,
                           0,
                           _IONBF,
                           _F_WRITE,
                           sys_write_flush,
                           nullptr,
                           0,
                           -1,
                           PTHREAD_MUTEX_INITIALIZER};

FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

/* ===================== Buffer management ===================== */

static void file_flush(FILE *f) {
  if (f->buf_pos > 0 && f->write_fn) {
    f->write_fn(f, f->buf, f->buf_pos);
    f->buf_pos = 0;
  }
}

int fflush(FILE *f) {
  if (f)
    file_flush(f);
  return 0;
}

/* ===================== Character output ===================== */

static void file_putc_internal(FILE *f, char c) {
  if (f->buf_mode == _IONBF) {
    /* unbuffered: direct write */
    char tmp = c;
    if (f->write_fn)
      f->write_fn(f, &tmp, 1);
    return;
  }

  /* buffered: write to buffer */
  if (f->buf && f->buf_pos < f->buf_size) {
    f->buf[f->buf_pos++] = c;
  }

  /* line-buffered: flush on newline */
  if (f->buf_mode == _IOLBF && c == '\n') {
    file_flush(f);
  }

  /* full-buffered: flush when buffer full */
  if (f->buf_mode == _IOFBF && f->buf_pos >= f->buf_size) {
    file_flush(f);
  }
}

int putchar(int c) {
  file_putc_internal(stdout, (char)c);
  fflush(stdout);
  return c;
}

int putchar_unlocked(int c) { return putchar(c); }

int fputc(int c, FILE *f) {
  file_putc_internal(f, (char)c);
  return c;
}

int fputc_unlocked(int c, FILE *f) { return fputc(c, f); }

int fputs(const char *s, FILE *f) {
  while (*s)
    file_putc_internal(f, *s++);
  return 0;
}

int puts(const char *s) {
  fputs(s, stdout);
  file_putc_internal(stdout, '\n');
  return 0;
}

/* ===================== vfprintf ===================== */

struct file_writer {
  FILE *f;
};

static void file_putc_wrapper(char c, void *arg) {
  file_putc_internal(((struct file_writer *)arg)->f, c);
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
  struct file_writer w = {f};
  return kvformat(file_putc_wrapper, &w, fmt, ap);
}

int vprintf(const char *fmt, va_list ap) {
  int n = vfprintf(stdout, fmt, ap);
  fflush(stdout);
  return n;
}

/* ===================== printf / fprintf ===================== */

int printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(stdout, fmt, ap);
  va_end(ap);
  fflush(stdout);
  return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(f, fmt, ap);
  va_end(ap);
  fflush(f);
  return n;
}

/* ===================== Character input ===================== */

int fgetc(FILE *f) {
  if (f->ungot != -1) {
    int c = f->ungot;
    f->ungot = -1;
    return c;
  }
  char c;
  if (f->read_fn) {
    int n = f->read_fn(f, &c, 1);
    if (n <= 0)
      return EOF;
    return (unsigned char)c;
  }
  /* Fallback: direct sys_read */
  int64_t n = sys_read(f->fd, &c, 1);
  if (n <= 0)
    return EOF;
  return (unsigned char)c;
}

int fgetc_unlocked(FILE *f) { return fgetc(f); }

int getchar(void) { return fgetc(stdin); }

int getchar_unlocked(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
  if (c == EOF || !f)
    return EOF;
  if (f->ungot != -1)
    return EOF; /* only 1-character pushback is supported */
  f->ungot = (unsigned char)c;
  f->flags &= ~_F_EOF;
  return (unsigned char)c;
}

/* ===================== sprintf / snprintf ===================== */

typedef struct {
  char *buf;
  size_t capacity;
  size_t pos;
  int flags; // bit 0: use_capacity
} string_buf_t;

static void string_write_fn(FILE *f, const char *data, int len) {
  (void)f; // f is the FILE* passed from vfprintf
  string_buf_t *sb = (string_buf_t *)f->buf; // hijack buf pointer
  for (int i = 0; i < len; i++) {
    if (sb->flags & 1) {
      if (sb->pos >= sb->capacity)
        break; // snprintf: truncate
    }
    sb->buf[sb->pos++] = data[i];
  }
}

static int vsnprintf_impl(char *buf, size_t n, const char *fmt, va_list ap,
                          int use_capacity) {
  string_buf_t sb;
  sb.buf = buf;
  sb.capacity = n;
  sb.pos = 0;
  sb.flags = use_capacity ? 1 : 0;

  // Create a temporary FILE that writes to the string buffer
  FILE str_f;
  str_f.fd = -1;
  str_f.buf = (char *)&sb; // store string_buf_t pointer in buf
  str_f.buf_size = 0;
  str_f.buf_pos = 0;
  str_f.buf_mode = _IONBF;
  str_f.flags = _F_WRITE;
  str_f.write_fn = string_write_fn;
  str_f.read_fn = NULL;

  vfprintf(&str_f, fmt, ap);

  // Null-terminate
  if (use_capacity && sb.pos < sb.capacity) {
    sb.buf[sb.pos] = '\0';
  } else if (sb.pos > 0 && sb.pos >= sb.capacity && sb.capacity > 0) {
    sb.buf[sb.capacity - 1] = '\0';
  } else {
    sb.buf[sb.pos] = '\0';
  }

  return (int)sb.pos;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
  return vsnprintf_impl(buf, (size_t)-1, fmt, ap, 0);
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
  if (n == 0)
    return 0;
  return vsnprintf_impl(buf, n, fmt, ap, 1);
}

int sprintf(char *buf, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsprintf(buf, fmt, ap);
  va_end(ap);
  return n;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n2 = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return n2;
}

/* ===================== perror ===================== */

void perror(const char *s) {
  if (s && *s) {
    fputs(s, stderr);
    fputs(": ", stderr);
  }
  fputs(strerror(errno), stderr);
  fputc('\n', stderr);
}

/* ===================== fopen / fclose ===================== */

FILE *fopen(const char *path, const char *mode) {
  if (!path || !mode) {
    errno = EINVAL;
    return NULL;
  }

  int flags = 0;
  int file_flags = 0;

  // Parse mode string
  if (mode[0] == 'r') {
    flags = O_RDONLY;
    file_flags = _F_READ;
    if (mode[1] == '+') {
      flags = O_RDWR;
      file_flags = _F_READ | _F_WRITE;
    }
  } else if (mode[0] == 'w') {
    flags = O_WRONLY | O_CREAT | O_TRUNC;
    file_flags = _F_WRITE;
    if (mode[1] == '+') {
      flags = O_RDWR | O_CREAT | O_TRUNC;
      file_flags = _F_READ | _F_WRITE;
    }
  } else if (mode[0] == 'a') {
    flags = O_WRONLY | O_CREAT | O_APPEND;
    file_flags = _F_WRITE;
    if (mode[1] == '+') {
      flags = O_RDWR | O_CREAT | O_APPEND;
      file_flags = _F_READ | _F_WRITE;
    }
  } else {
    errno = EINVAL;
    return NULL;
  }

  int fd = open(path, flags);
  if (fd < 0)
    return NULL;

  FILE *f = (FILE *)malloc(sizeof(FILE));
  if (!f) {
    close(fd);
    errno = ENOMEM;
    return NULL;
  }

  char *buf = (char *)malloc(BUFSIZ);
  if (!buf) {
    free(f);
    close(fd);
    errno = ENOMEM;
    return NULL;
  }

  f->fd = fd;
  f->buf = buf;
  f->buf_size = BUFSIZ;
  f->buf_pos = 0;
  f->buf_mode = _IOFBF; // full buffering by default
  f->flags = file_flags;
  f->write_fn = sys_write_flush;
  f->read_fn = sys_read_fill;
  f->offset = 0;
  f->ungot = -1;
  f->lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;

  return f;
}

int fclose(FILE *f) {
  if (!f) {
    errno = EINVAL;
    return EOF;
  }
  fflush(f);
  if (f->fd >= 0)
    close(f->fd);
  if (f->buf)
    free(f->buf);
  free(f);
  return 0;
}

/* ===================== fread / fwrite ===================== */

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
  if (!ptr || !f || !(f->flags & _F_READ))
    return 0;
  size_t total = size * nmemb;
  size_t nread = 0;

  if (f->buf_mode != _IONBF && f->buf) {
    // Buffered read: consume buffer first
    while (nread < total && f->buf_pos < f->buf_size) {
      ((char *)ptr)[nread++] = f->buf[f->buf_pos++];
    }
    // Flush remaining part via direct read
    f->buf_pos = 0;
    f->buf_size = 0;
  }

  while (nread < total) {
    int n = f->read_fn
                ? f->read_fn(f, (char *)ptr + nread, (int)(total - nread))
                : (int)sys_read(f->fd, (char *)ptr + nread, total - nread);
    if (n <= 0) {
      f->flags |= _F_EOF;
      break;
    }
    nread += n;
  }

  return nread / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
  if (!ptr || !f || !(f->flags & _F_WRITE))
    return 0;
  size_t total = size * nmemb;
  for (size_t i = 0; i < total; i++) {
    file_putc_internal(f, ((const char *)ptr)[i]);
  }
  return nmemb;
}

/* ===================== fseek / ftell / rewind ===================== */

int fseek(FILE *f, long offset, int whence) {
  if (!f) {
    errno = EINVAL;
    return -1;
  }
  fflush(f); // flush buffer before seek
  if (f->fd < 0) {
    errno = EBADF;
    return -1;
  }
  off_t r = lseek(f->fd, (off_t)offset, whence);
  if (r < 0)
    return -1;
  f->offset = r;
  f->buf_pos = 0;
  f->buf_size = 0;
  f->flags &= ~_F_EOF;
  return 0;
}

long ftell(FILE *f) {
  if (!f) {
    errno = EINVAL;
    return -1;
  }
  // offset + buf_pos gives the logical position
  return (long)(f->offset + f->buf_pos);
}

void rewind(FILE *f) {
  if (f)
    fseek(f, 0, SEEK_SET);
}

/* ===================== fdopen / freopen / fileno ===================== */

FILE *fdopen(int fd, const char *mode) {
  if (fd < 0 || !mode) {
    errno = EINVAL;
    return NULL;
  }

  int file_flags = 0;
  if (mode[0] == 'r') {
    file_flags = _F_READ;
    if (mode[1] == '+')
      file_flags = _F_READ | _F_WRITE;
  } else if (mode[0] == 'w') {
    file_flags = _F_WRITE;
    if (mode[1] == '+')
      file_flags = _F_READ | _F_WRITE;
  } else if (mode[0] == 'a') {
    file_flags = _F_WRITE;
    if (mode[1] == '+')
      file_flags = _F_READ | _F_WRITE;
  } else {
    errno = EINVAL;
    return NULL;
  }

  FILE *f = (FILE *)malloc(sizeof(FILE));
  if (!f) {
    errno = ENOMEM;
    return NULL;
  }
  char *buf = (char *)malloc(BUFSIZ);
  if (!buf) {
    free(f);
    errno = ENOMEM;
    return NULL;
  }

  f->fd = fd;
  f->buf = buf;
  f->buf_size = BUFSIZ;
  f->buf_pos = 0;
  f->buf_mode = _IOFBF;
  f->flags = file_flags;
  f->write_fn = sys_write_flush;
  f->read_fn = sys_read_fill;
  f->offset = 0;
  f->ungot = -1;
  f->lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
  return f;
}

FILE *freopen(const char *path, const char *mode, FILE *f) {
  if (!f || !mode) {
    errno = EINVAL;
    return NULL;
  }
  if (f->fd >= 0) {
    fflush(f);
    close(f->fd);
  }
  if (!path) {
    errno = EINVAL;
    return NULL;
  }
  FILE *nf = fopen(path, mode);
  if (!nf)
    return NULL;
  /* Move the new fd into the old FILE, keeping the FILE* unchanged */
  f->fd = nf->fd;
  f->flags = nf->flags;
  f->offset = 0;
  f->ungot = -1;
  /* Free nf's shell (its fd has already been moved away) */
  nf->fd = -1;
  fclose(nf);
  return f;
}

int fileno(FILE *f) {
  if (!f) {
    errno = EINVAL;
    return -1;
  }
  return f->fd;
}

/* ===================== feof / ferror / clearerr ===================== */

int feof(FILE *f) { return f && (f->flags & _F_EOF) ? 1 : 0; }
int ferror(FILE *f) { return f && (f->flags & _F_ERR) ? 1 : 0; }
void clearerr(FILE *f) {
  if (f)
    f->flags &= ~(_F_EOF | _F_ERR);
}

/* ===================== setbuf / setvbuf ===================== */

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
  if (!f) {
    errno = EINVAL;
    return -1;
  }
  fflush(f);
  /* Free the old buffer (only for buffers allocated by fopen; not the static
   * streams) */
  if (f->buf && f != stdin && f != stdout && f != stderr) {
    free(f->buf);
    f->buf = nullptr;
  }
  f->buf_mode = mode;
  if (mode == _IONBF) {
    f->buf = nullptr;
    f->buf_size = 0;
  } else if (buf) {
    f->buf = buf;
    f->buf_size = (int)size;
  } else {
    f->buf = (char *)malloc(size ? size : BUFSIZ);
    f->buf_size = f->buf ? (int)(size ? size : BUFSIZ) : 0;
  }
  f->buf_pos = 0;
  return 0;
}

int setbuf(FILE *f, char *buf) {
  return setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

/* ===================== flockfile / funlockfile / ftrylockfile
 * ===================== */

void flockfile(FILE *f) {
  if (f)
    pthread_mutex_lock(&f->lock);
}
void funlockfile(FILE *f) {
  if (f)
    pthread_mutex_unlock(&f->lock);
}
int ftrylockfile(FILE *f) {
  return f ? pthread_mutex_trylock(&f->lock) : EINVAL;
}

/* ===================== fread_unlocked / fwrite_unlocked =====================
 */

size_t fread_unlocked(void *ptr, size_t size, size_t nmemb, FILE *f) {
  return fread(ptr, size, nmemb, f);
}

size_t fwrite_unlocked(const void *ptr, size_t size, size_t nmemb, FILE *f) {
  return fwrite(ptr, size, nmemb, f);
}
