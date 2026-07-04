#ifndef _SYS_PCI_H
#define _SYS_PCI_H

#include <sys/types.h>
#include <stdint.h>

// struct pci_dev_info_bar and struct pci_dev_info are defined in
// xos/syscall_nums.h (pulled in transitively via user/include/syscall.h).
#include "syscall.h"
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int pci_dev_info(uint8_t bus, uint8_t dev, uint8_t func, struct pci_dev_info *out);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PCI_H */
