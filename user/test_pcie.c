#include <stdio.h>
#include <stdint.h>
#include <sys/pci.h>

static const char *bar_type_name(uint8_t type) {
    switch (type) {
        case 0: return "MMIO32";
        case 1: return "IO";
        case 2: return "MMIO64";
        default: return "???";
    }
}

int main(void) {
    printf("=== PCIe device scan ===\n");

    int found = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                struct pci_dev_info info;
                int rc = pci_dev_info(bus, dev, func, &info);
                if (rc != 0) continue;

                printf("%02x:%02x.%x vendor=%04x device=%04x class=%04x",
                       bus, dev, func,
                       info.vendor_id, info.device_id, info.class_code);
                if (info.irq_pin)
                    printf(" irq_pin=%d irq_line=%d", info.irq_pin, info.irq_line);

                for (int i = 0; i < info.num_bars; i++) {
                    if (info.bars[i].size == 0) continue;
                    printf(" bar%d[%s]=0x%lx+0x%lx",
                           i, bar_type_name(info.bars[i].type),
                           (unsigned long)info.bars[i].phys,
                           (unsigned long)info.bars[i].size);
                }
                printf("\n");
                found++;
            }
        }
    }

    printf("Total: %d devices\n", found);
    return 0;
}
