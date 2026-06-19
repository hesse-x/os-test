#ifndef _SYS_PCI_H
#define _SYS_PCI_H

#include <sys/types.h>
#include <stdint.h>

// struct pci_dev_info_bar and struct pci_dev_info are defined in common/syscall.h
// (included transitively through the kernel interface)
#include "common/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

int pci_dev_info(uint8_t bus, uint8_t dev, uint8_t func, struct pci_dev_info *out);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PCI_H */
