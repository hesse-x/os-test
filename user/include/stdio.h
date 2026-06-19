#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

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
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* Output functions */
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int putchar(int c);
int fputc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int puts(const char *s);
int fflush(FILE *f);
int fgetc(FILE *f);
int getchar(void);

/* sprintf family */
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* perror */
void perror(const char *s);

/* File I/O */
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
void rewind(FILE *f);

#ifdef __cplusplus
}
#endif

#endif /* _STDIO_H */
