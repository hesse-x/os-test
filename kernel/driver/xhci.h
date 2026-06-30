#ifndef KERNEL_XHCI_H
#define KERNEL_XHCI_H

void xhci_init();
void xhci_poll();

struct dev_driver;
extern struct dev_driver xhci_driver;

#endif // KERNEL_XHCI_H
