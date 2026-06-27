#ifndef KERNEL_DISPLAY_H
#define KERNEL_DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel/sparse.h"
#include "common/ioctl.h"
#include "common/font_metrics.h"
#include "common/display.h"

struct proc_t;
struct pci_device;

// VBE DISPI MMIO register indices (bochs-display BAR0)
#define VBE_DISPI_INDEX_ID          0x00
#define VBE_DISPI_INDEX_XRES        0x01
#define VBE_DISPI_INDEX_YRES        0x02
#define VBE_DISPI_INDEX_BPP         0x03
#define VBE_DISPI_INDEX_ENABLE      0x04
#define VBE_DISPI_INDEX_BANK        0x05
// VBE DISPI MMIO register layout (QEMU bochs-display BAR2):
// VBE registers start at PCI_VGA_BOCHS_OFFSET = 0x500 within BAR2
// Each register is 16-bit, at offset 0x500 + index * 2
#define VBE_DISPI_MMIO_OFFSET(idx)  (0x500 + (idx) * 2)

#define VBE_DISPI_ENABLED           0x01
#define VBE_DISPI_LFB_ENABLED       0x40
#define VBE_DISPI_ID_VERSION        0xB0C5

// Kernel display subsystem state
struct display_state {
    uint8_t __iomem *front_fb;      // front buffer MMIO address (PCI BAR1)
    uint8_t  *back_buffer;          // back buffer kernel virtual address
    uint64_t  back_buffer_phys;     // back buffer physical address
    uint64_t  back_buffer_npages;   // back buffer page count
    uint16_t __iomem *vbe_mmio;    // BAR0 MMIO base (VBE DISPI registers)
    struct pci_device *pci_dev;     // PCI device reference
    uint32_t  fb_width;
    uint32_t  fb_height;
    uint32_t  fb_pitch;
    uint32_t  fb_bpp;
    uint32_t  fb_size;
    bool      initialized;          // back buffer allocated
};

// Global display state (defined in display.c)
extern struct display_state g_display;

// PCI discovery + VBE modeset + BAR mapping (no boot_info dependency)
void display_init(void);

// Request handler (legacy, called by sys_dev_req fallback)
int display_req_handler(uint32_t req_type, void *req_data, uint32_t req_len,
                        void *resp_data, uint32_t resp_len);

// ioctl handler (new, called via dev_ops callback)
long display_ioctl(uint32_t cmd, void *arg);

// mmap handler: returns mapped address, 0=failure
uint64_t display_mmap_handler(struct proc_t *proc, size_t size);
uint64_t display_mmap_handler_ioctl(struct proc_t *proc, uint64_t size);

// Device registration (called from vfs_init)
void display_dev_register(void);

#endif
