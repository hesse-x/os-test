#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <pthread.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FILE structure */
typedef struct _FILE {
    int fd;              /* file descriptor (0=stdin, 1=stdout, 2=stderr) */
    char *buf;           /* I/O buffer */
    int buf_size;        /* buffer capacity */
    int buf_pos;         /* current write position in buffer */
    int buf_mode;        /* _IONBF / _IOLBF / _IOFBF */
    int flags;           /* _F_WRITE / _F_READ / _F_EOF / _F_ERR */
    void (*write_fn)(struct _FILE *, const char *, int len); /* output function */
    int (*read_fn)(struct _FILE *, char *, int len);        /* input function */
    off_t offset;        /* current file offset (for fseek/ftell) */
    int ungot;           /* ungetc pushback (-1 if none) */
    pthread_mutex_t lock; /* per-FILE lock for flockfile/funlockfile */
} FILE;

/* Constants */
#define EOF        (-1)
#define _IONBF     0   /* no buffering */
#define _IOLBF     1   /* line buffering */
#define _IOFBF     2   /* full buffering */

#define _F_WRITE   1
#define _F_READ    2
#define _F_EOF     4
#define _F_ERR     8

#define BUFSIZ     4096

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Standard streams */
LIBC_EXPORT extern FILE *stdin;
LIBC_EXPORT extern FILE *stdout;
LIBC_EXPORT extern FILE *stderr;

/* Output functions */
LIBC_EXPORT int printf(const char *fmt, ...);
LIBC_EXPORT int vprintf(const char *fmt, va_list ap);
LIBC_EXPORT int fprintf(FILE *f, const char *fmt, ...);
LIBC_EXPORT int vfprintf(FILE *f, const char *fmt, va_list ap);
LIBC_EXPORT int putchar(int c);
LIBC_EXPORT int putchar_unlocked(int c);
LIBC_EXPORT int fputc(int c, FILE *f);
LIBC_EXPORT int fputc_unlocked(int c, FILE *f);
LIBC_EXPORT int fputs(const char *s, FILE *f);
LIBC_EXPORT int puts(const char *s);
LIBC_EXPORT int fflush(FILE *f);
LIBC_EXPORT int fgetc(FILE *f);
LIBC_EXPORT int fgetc_unlocked(FILE *f);
LIBC_EXPORT int getchar(void);
LIBC_EXPORT int getchar_unlocked(void);
LIBC_EXPORT int ungetc(int c, FILE *f);

/* sprintf family */
LIBC_EXPORT int sprintf(char *buf, const char *fmt, ...);
LIBC_EXPORT int snprintf(char *buf, size_t n, const char *fmt, ...);
LIBC_EXPORT int vsprintf(char *buf, const char *fmt, va_list ap);
LIBC_EXPORT int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* perror */
LIBC_EXPORT void perror(const char *s);

/* File I/O */
LIBC_EXPORT FILE *fopen(const char *path, const char *mode);
LIBC_EXPORT FILE *fdopen(int fd, const char *mode);
LIBC_EXPORT FILE *freopen(const char *path, const char *mode, FILE *f);
LIBC_EXPORT int fclose(FILE *f);
LIBC_EXPORT int fileno(FILE *f);
LIBC_EXPORT int feof(FILE *f);
LIBC_EXPORT int ferror(FILE *f);
LIBC_EXPORT void clearerr(FILE *f);
LIBC_EXPORT size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
LIBC_EXPORT size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
LIBC_EXPORT size_t fread_unlocked(void *ptr, size_t size, size_t nmemb, FILE *f);
LIBC_EXPORT size_t fwrite_unlocked(const void *ptr, size_t size, size_t nmemb, FILE *f);
LIBC_EXPORT int fseek(FILE *f, long offset, int whence);
LIBC_EXPORT long ftell(FILE *f);
LIBC_EXPORT void rewind(FILE *f);
LIBC_EXPORT int setbuf(FILE *f, char *buf);
LIBC_EXPORT int setvbuf(FILE *f, char *buf, int mode, size_t size);

/* File locking (pthread mutex per FILE) */
LIBC_EXPORT void flockfile(FILE *f);
LIBC_EXPORT void funlockfile(FILE *f);
LIBC_EXPORT int ftrylockfile(FILE *f);

#ifdef __cplusplus
}
#endif

#endif /* _STDIO_H */
