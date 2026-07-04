#include <sys/irq.h>
#include <errno.h>
#include "syscall.h"

int irq_bind(int irq) {
    return sys_irq_bind(irq);
}
