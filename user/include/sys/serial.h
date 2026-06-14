#ifndef _SYS_SERIAL_H
#define _SYS_SERIAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int serial_write(const char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SERIAL_H */
