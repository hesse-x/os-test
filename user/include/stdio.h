#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif

#endif /* _STDIO_H */