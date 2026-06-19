#ifndef _ASSERT_H
#define _ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

void __assert_fail(const char *expr, const char *file, int line);

#ifdef __cplusplus
}
#endif

#define assert(expr) \
    ((void)((expr) || (__assert_fail(#expr, __FILE__, __LINE__), 0)))

#endif /* _ASSERT_H */
