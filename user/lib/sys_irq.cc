#include <sys/irq.h>
#include <errno.h>
#include "common/syscall.h"

int irq_bind(int irq) {
    int r = sys_irq_bind(irq);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}
