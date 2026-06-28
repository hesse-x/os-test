#include <sys/pci.h>
#include <errno.h>
#include "common/syscall.h"

int pci_dev_info(uint8_t bus, uint8_t dev, uint8_t func, struct pci_dev_info *out) {
    return sys_pci_dev_info(bus, dev, func, out);
}
