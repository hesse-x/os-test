#include "syscall.h"
#include <stdint.h>
#include <sys/pci.h>

int pci_dev_info(uint8_t bus, uint8_t dev, uint8_t func,
                 struct pci_dev_info *out) {
  return sys_pci_dev_info(bus, dev, func, out);
}
