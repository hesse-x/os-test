#include "xos/errno.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明：本文件内函数互相调用（strdup 用 memcpy、strerror_r 用 strerror、
 * bcopy 用 memmove、bcmp 用 memcmp），C++ 需先声明 */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
const char *strerror(int errnum);
size_t strlen(const char *s);

size_t strlen(const char *s) {
  size_t n = 0;
  while (*s++)
    n++;
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *(const unsigned char *)a - *(const unsigned char *)b;
}

int strncmp(const char *a, const char *b, size_t n) {
  while (n && *a && *a == *b) {
    a++;
    b++;
    n--;
  }
  if (!n)
    return 0;
  return *(const unsigned char *)a - *(const unsigned char *)b;
}

char *strcpy(char *dst, const char *src) {
  char *d = dst;
  while ((*d++ = *src++))
    ;
  return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
  char *d = dst;
  while (n && (*d++ = *src++))
    n--;
  while (n--)
    *d++ = '\0';
  return dst;
}

char *strcat(char *dst, const char *src) {
  char *d = dst + strlen(dst);
  while ((*d++ = *src++))
    ;
  return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
  char *d = dst + strlen(dst);
  while (n && *src) {
    *d++ = *src++;
    n--;
  }
  *d = '\0';
  return dst;
}

char *strchr(const char *s, int c) {
  while (*s) {
    if (*s == (char)c)
      return (char *)s;
    s++;
  }
  if (c == '\0')
    return (char *)s;
  return nullptr;
}

char *strrchr(const char *s, int c) {
  const char *last = nullptr;
  for (;;) {
    if (*s == (char)c)
      last = s;
    if (!*s)
      break;
    s++;
  }
  return (char *)last;
}

char *strpbrk(const char *s, const char *accept) {
  while (*s) {
    const char *a = accept;
    while (*a) {
      if (*s == *a)
        return (char *)s;
      a++;
    }
    s++;
  }
  return nullptr;
}

size_t strspn(const char *s, const char *accept) {
  size_t n = 0;
  while (*s) {
    const char *a = accept;
    int found = 0;
    while (*a) {
      if (*s == *a) {
        found = 1;
        break;
      }
      a++;
    }
    if (!found)
      break;
    n++;
    s++;
  }
  return n;
}

size_t strcspn(const char *s, const char *reject) {
  size_t n = 0;
  while (*s) {
    const char *r = reject;
    int found = 0;
    while (*r) {
      if (*s == *r) {
        found = 1;
        break;
      }
      r++;
    }
    if (found)
      break;
    n++;
    s++;
  }
  return n;
}

void *memchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  while (n--) {
    if (*p == (unsigned char)c)
      return (void *)p;
    p++;
  }
  return nullptr;
}

void *memmem(const void *haystack, size_t haystacklen, const void *needle,
             size_t needlelen) {
  if (needlelen == 0)
    return (void *)haystack;
  if (haystacklen < needlelen)
    return nullptr;
  const unsigned char *h = (const unsigned char *)haystack;
  const unsigned char *n = (const unsigned char *)needle;
  for (size_t i = 0; i <= haystacklen - needlelen; i++) {
    size_t j = 0;
    for (; j < needlelen; j++)
      if (h[i + j] != n[j])
        break;
    if (j == needlelen)
      return (void *)(h + i);
  }
  return nullptr;
}

char *strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (!p) {
    errno = ENOMEM;
    return nullptr;
  }
  return (char *)memcpy(p, s, n);
}

char *strndup(const char *s, size_t n) {
  size_t len = 0;
  while (len < n && s[len])
    len++;
  char *p = (char *)malloc(len + 1);
  if (!p) {
    errno = ENOMEM;
    return nullptr;
  }
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

int strerror_r(int errnum, char *buf, size_t buflen) {
  if (!buf || buflen == 0)
    return EINVAL;
  const char *msg = strerror(errnum);
  size_t len = 0;
  while (msg[len])
    len++;
  if (len >= buflen) {
    size_t copy = buflen - 1;
    memcpy(buf, msg, copy);
    buf[copy] = '\0';
    return ERANGE;
  }
  memcpy(buf, msg, len + 1);
  return 0;
}

/* BSD legacy aliases */
void bcopy(const void *src, void *dst, size_t n) { memmove(dst, src, n); }

int bcmp(const void *s1, const void *s2, size_t n) {
  return memcmp(s1, s2, n) != 0 ? 1 : 0;
}

char *index(const char *s, int c) { return strchr(s, c); }
char *rindex(const char *s, int c) { return strrchr(s, c); }

void *memcpy(void *dst, const void *src, size_t n) {
  char *d = (char *)dst;
  const char *s = (const char *)src;
  while (n--)
    *d++ = *s++;
  return dst;
}

void *memset(void *s, int c, size_t n) {
  char *p = (char *)s;
  while (n--)
    *p++ = (char)c;
  return s;
}

void *memmove(void *dst, const void *src, size_t n) {
  char *d = (char *)dst;
  const char *s = (const char *)src;
  if (d < s) {
    while (n--)
      *d++ = *s++;
  } else {
    d += n;
    s += n;
    while (n--)
      *--d = *--s;
  }
  return dst;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *a = (const unsigned char *)s1;
  const unsigned char *b = (const unsigned char *)s2;
  while (n--) {
    if (*a != *b)
      return (int)(*a - *b);
    a++;
    b++;
  }
  return 0;
}

char *strstr(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;
  while (*haystack) {
    const char *h = haystack;
    const char *n = needle;
    while (*h && *n && *h == *n) {
      h++;
      n++;
    }
    if (!*n)
      return (char *)haystack;
    haystack++;
  }
  return nullptr;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
  if (!str)
    str = *saveptr;
  if (!str)
    return nullptr;
  // Skip leading delimiters
  while (*str) {
    const char *d = delim;
    int is_delim = 0;
    while (*d) {
      if (*str == *d) {
        is_delim = 1;
        break;
      }
      d++;
    }
    if (!is_delim)
      break;
    str++;
  }
  if (!*str) {
    *saveptr = nullptr;
    return nullptr;
  }
  char *token = str;
  // Find end of token
  while (*str) {
    const char *d = delim;
    int is_delim = 0;
    while (*d) {
      if (*str == *d) {
        is_delim = 1;
        break;
      }
      d++;
    }
    if (is_delim) {
      *str = '\0';
      *saveptr = str + 1;
      return token;
    }
    str++;
  }
  *saveptr = nullptr;
  return token;
}

char *strtok(char *str, const char *delim) {
  static char *saveptr = nullptr;
  return strtok_r(str, delim, &saveptr);
}

const char *strerror(int errnum) {
  switch (errnum) {
  case 0:
    return "Success";
  case 1:
    return "Operation not permitted";
  case 2:
    return "No such file or directory";
  case 3:
    return "No such process";
  case 4:
    return "Interrupted system call";
  case 5:
    return "I/O error";
  case 6:
    return "No such device or address";
  case 7:
    return "Argument list too long";
  case 8:
    return "Exec format error";
  case 9:
    return "Bad file number";
  case 10:
    return "No child processes";
  case 11:
    return "Try again";
  case 12:
    return "Out of memory";
  case 13:
    return "Permission denied";
  case 14:
    return "Bad address";
  case 15:
    return "Block device required";
  case 16:
    return "Device or resource busy";
  case 17:
    return "File exists";
  case 22:
    return "Invalid argument";
  case 23:
    return "Too many open files";
  case 24:
    return "Too many open files in system";
  case 25:
    return "Not a tty";
  case 27:
    return "File too large";
  case 28:
    return "No space left on device";
  case 30:
    return "Read-only file system";
  case 45:
    return "Operation not supported";
  case 47:
    return "Operation would block";
  case 48:
    return "Operation already in progress";
  case 49:
    return "Socket operation on non-socket";
  case 50:
    return "Destination address required";
  case 51:
    return "Message too long";
  case 52:
    return "Protocol wrong type for socket";
  case 53:
    return "Protocol not available";
  case 54:
    return "Protocol not supported";
  case 55:
    return "Socket type not supported";
  case 56:
    return "Operation not supported on transport endpoint";
  case 57:
    return "Protocol family not supported";
  case 58:
    return "Address family not supported by protocol";
  case 59:
    return "Address already in use";
  case 60:
    return "Cannot assign requested address";
  case 61:
    return "Network is down";
  case 62:
    return "Network is unreachable";
  case 63:
    return "Network dropped connection on reset";
  case 64:
    return "Software caused connection abort";
  case 65:
    return "Connection reset by peer";
  case 66:
    return "No buffer space available";
  case 67:
    return "Transport endpoint is already connected";
  case 68:
    return "Transport endpoint is not connected";
  case 69:
    return "Cannot send after transport endpoint shutdown";
  case 70:
    return "Too many references: cannot splice";
  case 71:
    return "Connection timed out";
  case 72:
    return "Connection refused";
  case 73:
    return "Host is down";
  case 74:
    return "No route to host";
  case 75:
    return "Operation already in progress";
  case 76:
    return "Stale NFS file handle";
  case 77:
    return "Structure needs cleaning";
  case 78:
    return "Not a XENIX named type file";
  case 79:
    return "No XENIX semaphores available";
  case 80:
    return "Is a directory";
  case 81:
    return "Operation canceled";
  case 82:
    return "Too many levels of remote in path";
  case 84:
    return "Not a directory";
  case 87:
    return "Value too large for defined data type";
  case 90:
    return "Name not unique on network";
  case 91:
    return "File descriptor in bad state";
  case 92:
    return "Remote address changed";
  case 93:
    return "Can not access a needed shared library";
  case 94:
    return "Accessing a corrupted shared library";
  case 95:
    return ".lib section in a.out corrupted";
  case 96:
    return "Attempting to link in too many shared libraries";
  case 97:
    return "Cannot exec a shared library directly";
  case 98:
    return "Illegal byte sequence";
  case 99:
    return "Interrupted system call should be restarted";
  case 100:
    return "Streams pipe error";
  case 101:
    return "Too many users";
  case 102:
    return "Socket operation on non-socket";
  case 103:
    return "Destination address required";
  case 104:
    return "Message too long";
  case 105:
    return "Protocol wrong type for socket";
  case 106:
    return "Protocol not available";
  case 107:
    return "Protocol not supported";
  case 108:
    return "Socket type not supported";
  case 109:
    return "Not supported";
  case 110:
    return "Protocol family not supported";
  case 111:
    return "Address family not supported by protocol family";
  case 112:
    return "Address already in use";
  case 113:
    return "Cannot assign requested address";
  case 114:
    return "Network is down";
  case 115:
    return "Network is unreachable";
  case 116:
    return "Network dropped connection because of reset";
  case 117:
    return "Software caused connection abort";
  case 118:
    return "Connection reset by peer";
  case 119:
    return "No buffer space available";
  case 120:
    return "Transport endpoint is already connected";
  case 121:
    return "Transport endpoint is not connected";
  case 122:
    return "Cannot send after transport endpoint shutdown";
  case 123:
    return "Too many references: cannot splice";
  case 124:
    return "Connection timed out";
  case 125:
    return "Connection refused";
  case 126:
    return "Host is down";
  case 127:
    return "No route to host";
  default:
    return "Unknown error";
  }
}

void bzero(void *s, size_t n) {
  char *p = (char *)s;
  while (n--)
    *p++ = 0;
}

#ifdef __cplusplus
}
#endif