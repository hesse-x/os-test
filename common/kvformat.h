#ifndef COMMON_KVFORMAT_H
#define COMMON_KVFORMAT_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * kvformat — shared format string parser (kvprintf-style).
 *
 * Supports: %%, %c, %s, %d, %u, %o, %x, %X, %p
 * Flags:  '-' (left align), '0' (zero pad)
 * Width:  numeric digits after '%' / '%0'
 * Length: 'l' (long), 'll' (long long), 'z' (size_t)
 *
 * Calls putc(c, arg) for each output character.
 * Returns total character count.
 */
int kvformat(void (*putc)(char c, void *arg), void *arg,
             const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_KVFORMAT_H */
