#include <sys/irq.h>
#include <errno.h>
#include "common/syscall.h"

int irq_bind(int irq) {
    return sys_irq_bind(irq);
}
