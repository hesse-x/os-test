#include "kernel/display.h"
#include "kernel/proc.h"
#include "kernel/devtmpfs.h"
#include "kernel/serial.h"
#include "kernel/mem/alloc.h"
#include "kernel/mem/slab.h"
#include "arch/x64/paging.h"
#include "common/dev.h"
#include "common/errno.h"
#include <string.h>

// Global display state
struct display_state g_display;

// KMS device ops (driver_pid=0 means kernel device)
static struct dev_ops kms_dev_ops = {
    .driver_pid = 0,
    .device_type = DEV_KMS,
};

int display_req_handler(uint32_t req_type, void *req_data, uint32_t req_len,
                        void *resp_data, uint32_t resp_len) {
    if (req_type == DISPLAY_REQ_CREATE_BUF) {
        if (req_len < sizeof(struct display_create_buf_req) ||
            resp_len < sizeof(struct display_create_buf_resp))
            return -EINVAL;

        struct display_create_buf_req *req = (struct display_create_buf_req *)req_data;
        struct display_create_buf_resp *resp = (struct display_create_buf_resp *)resp_data;

        // Validate parameters
        if (req->width != 800 || req->height != 600 || req->bpp != 32)
            return -EINVAL;

        // Already initialized
        if (g_display.initialized)
            return -EBUSY;

        // Allocate back buffer
        uint32_t pitch = req->width * 4;
        uint32_t size = pitch * req->height;
        size_t npages = (size + 4095) / 4096;

        Page *pages = bfc_alloc_page(npages);
        if (!pages) return -ENOMEM;

        uint64_t phys = (__force uint64_t)page_to_phys(pages);
        uint8_t *vaddr = (__force uint8_t *)phys_to_virt((__force phys_addr_t)phys);
        __memset(vaddr, 0, npages * PAGE_SIZE);

        g_display.back_buffer = vaddr;
        g_display.back_buffer_phys = phys;
        g_display.back_buffer_npages = npages;
        g_display.initialized = true;

        resp->pitch = pitch;
        resp->size = size;
        resp->rows = req->height / DISPLAY_FONT_HEIGHT;
        resp->cols = req->width / DISPLAY_FONT_WIDTH;
        resp->result = 0;
        return 0;

    } else if (req_type == DISPLAY_REQ_FLIP) {
        if (resp_len < sizeof(struct display_flip_resp))
            return -EINVAL;

        struct display_flip_resp *resp = (struct display_flip_resp *)resp_data;

        if (!g_display.initialized) {
            resp->result = -ENOENT;
            return -ENOENT;
        }

        __memcpy((void __force *)g_display.front_fb, g_display.back_buffer, g_display.fb_size);
        resp->result = 0;
        return 0;
    }

    return -EINVAL;
}

uint64_t display_mmap_handler(struct proc_t *proc, size_t size) {
    if (!g_display.initialized) return 0;

    // Map back buffer physical pages into user address space
    size_t npages = g_display.back_buffer_npages;
    uint64_t *pml4 = (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3);
    uint64_t vaddr = proc->mmap_brk;
    uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

    for (size_t i = 0; i < npages; i++) {
        uint64_t page_phys = g_display.back_buffer_phys + i * PAGE_SIZE;
        if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, page_phys, pte_flags)) {
            // Cleanup on failure
            for (size_t j = 0; j < i; j++)
                unmap_user_pages(pml4, vaddr + j * PAGE_SIZE, vaddr + (j + 1) * PAGE_SIZE, 1);
            return 0;
        }
    }

    // Create mmap_region_t for proc_reap cleanup
    mmap_region_t *region = (mmap_region_t *)kmalloc(sizeof(mmap_region_t));
    if (!region) {
        for (size_t i = 0; i < npages; i++)
            unmap_user_pages(pml4, vaddr + i * PAGE_SIZE, vaddr + (i + 1) * PAGE_SIZE, 1);
        return 0;
    }

    region->vaddr = vaddr;
    region->size = npages * PAGE_SIZE;
    region->phys = 0;
    region->shm_obj = NULL;
    region->next = proc->mmap_regions;
    proc->mmap_regions = region;
    proc->mmap_brk = vaddr + npages * PAGE_SIZE;

    return vaddr;
}

void display_dev_register(void) {
    int rc = devtmpfs_create("kms", DEV_KMS, &kms_dev_ops);
    if (rc != 0) {
        serial_printf("display_dev_register: failed (rc=%d)\n", rc);
    } else {
        serial_printf("display_dev_register: /dev/kms registered\n");
    }
}
