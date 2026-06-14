#ifndef _SYS_PROCESS_H
#define _SYS_PROCESS_H

#include <sys/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

pid_t spawn(const void *elf, size_t size, int iopl);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PROCESS_H */
