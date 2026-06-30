// kernel/driver/init.c — Driver initialization sequence
// Extracted from kernel/kernel.c (phase 5 step 5.2)

#include "kernel/driver/driver.h"
#include "kernel/driver/pci.h"
#include "kernel/driver/xhci.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/trap.h"

// Driver definitions (in respective .c files)
extern dev_driver_t ahci_driver;
extern dev_driver_t xhci_driver;
extern dev_driver_t display_driver;
extern dev_driver_t serial_driver;

void driver_init(void) {
    pci_init();
    printk(LOG_INFO, "driver_init: pci_init done\n");

    // Register all built-in drivers
    driver_register(&ahci_driver);
    driver_register(&xhci_driver);
    driver_register(&display_driver);
    driver_register(&serial_driver);

    // PCI class/vendor auto-match: calls init() for matched drivers
    driver_pci_match();

    // Register xHCI timer poll hook (called periodically from timer IRQ handler)
    timer_poll_hook = xhci_poll;

    printk(LOG_INFO, "driver_init: done\n");
}
