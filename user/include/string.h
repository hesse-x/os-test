#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT size_t strlen(const char *s);
LIBC_EXPORT int strcmp(const char *a, const char *b);
LIBC_EXPORT int strncmp(const char *a, const char *b, size_t n);
LIBC_EXPORT char *strcpy(char *dst, const char *src);
LIBC_EXPORT char *strncpy(char *dst, const char *src, size_t n);
LIBC_EXPORT char *strcat(char *dst, const char *src);
LIBC_EXPORT char *strncat(char *dst, const char *src, size_t n);
LIBC_EXPORT char *strchr(const char *s, int c);
LIBC_EXPORT char *strrchr(const char *s, int c);
LIBC_EXPORT char *strpbrk(const char *s, const char *accept);
LIBC_EXPORT size_t strspn(const char *s, const char *accept);
LIBC_EXPORT size_t strcspn(const char *s, const char *reject);
LIBC_EXPORT void *memchr(const void *s, int c, size_t n);
LIBC_EXPORT void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen);
LIBC_EXPORT int memcmp(const void *s1, const void *s2, size_t n);
LIBC_EXPORT char *strstr(const char *haystack, const char *needle);
LIBC_EXPORT char *strtok(char *str, const char *delim);
LIBC_EXPORT char *strtok_r(char *str, const char *delim, char **saveptr);
LIBC_EXPORT char *strdup(const char *s);
LIBC_EXPORT char *strndup(const char *s, size_t n);
LIBC_EXPORT const char *strerror(int errnum);
LIBC_EXPORT int strerror_r(int errnum, char *buf, size_t buflen);
LIBC_EXPORT void bzero(void *s, size_t n);
/* BSD legacy aliases */
LIBC_EXPORT void bcopy(const void *src, void *dst, size_t n);
LIBC_EXPORT int bcmp(const void *s1, const void *s2, size_t n);
LIBC_EXPORT char *index(const char *s, int c);
LIBC_EXPORT char *rindex(const char *s, int c);

LIBC_EXPORT void *memcpy(void *dst, const void *src, size_t n);
LIBC_EXPORT void *memset(void *s, int c, size_t n);
LIBC_EXPORT void *memmove(void *dst, const void *src, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* _STRING_H */