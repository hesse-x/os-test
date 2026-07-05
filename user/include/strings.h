#ifndef _STRINGS_H
#define _STRINGS_H

#include <sys/cdefs.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BSD legacy string functions — also declared in <string.h> for
 * compatibility.
 *
 * NOTE: strcasecmp/strncasecmp are defined here per POSIX, not in <string.h>.
 */

LIBC_EXPORT void bzero(void *s, size_t n);
LIBC_EXPORT void bcopy(const void *src, void *dst, size_t n);
LIBC_EXPORT int bcmp(const void *s1, const void *s2, size_t n);
LIBC_EXPORT char *index(const char *s, int c);
LIBC_EXPORT char *rindex(const char *s, int c);
LIBC_EXPORT int strcasecmp(const char *s1, const char *s2);
LIBC_EXPORT int strncasecmp(const char *s1, const char *s2, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* _STRINGS_H */
