#ifndef _SYS_IRQ_H
#define _SYS_IRQ_H

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int irq_bind(int irq);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IRQ_H */
