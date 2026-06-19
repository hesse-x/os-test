#include "kernel/fb.h"
#include "kernel/serial.h"
#include "arch/x64/paging.h"
#include "common/macro.h"
#include "common/shm.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ===================== Global fb info for KMS driver =====================

kms_fb_info g_fb_info = {0};

// ===================== init_fb =====================

void init_fb(boot_info *bi) {
  serial_puts("init_fb: fb_addr=");
  serial_put_hex(bi->fb_addr);
  serial_puts("\n");
  if (bi->fb_addr == 0) {
    serial_puts("init_fb: fb_addr=0, returning\n");
    return;
  }

  uint64_t fb_phys = bi->fb_addr;
  uint32_t fb_pitch = bi->fb_pitch;
  uint32_t fb_w = bi->fb_width;
  uint32_t fb_h = bi->fb_height;
  uint32_t fb_bpp = bi->fb_bpp;
  uint32_t fb_bytes = fb_pitch * fb_h;

  // Map framebuffer using 2MB huge pages in the device mapping area
  uint64_t fb_2mb_start = fb_phys & ~0x1FFFFFULL;
  uint64_t fb_2mb_end = ALIGN_UP(fb_phys + fb_bytes, 0x200000);
  size_t num_fb_2mb = (fb_2mb_end - fb_2mb_start) / 0x200000;

  // Find first free PDPT_hh entry (after RAM mappings)
  int pdpt_start = 510;
  for (int i = pdpt_start; i < 512; i++) {
    if (pdpt_hh[i] == 0) {
      pdpt_start = i;
      break;
    }
  }

  // Allocate a PD for the framebuffer
  uint64_t *fb_pd = (uint64_t *)bump_alloc(4096);
  uintptr_t fb_pd_phys = PHY_ADDR((uintptr_t)fb_pd);

  // Clear PD
  for (int i = 0; i < 512; i++) {
    fb_pd[i] = 0;
  }

  // Fill PD with 2MB huge pages mapping the framebuffer physical region
  for (size_t n = 0; n < num_fb_2mb; n++) {
    uint64_t page_phys = fb_2mb_start + (uint64_t)n * 0x200000;
    fb_pd[n] = page_phys | 0x83;  // Present + RW + PS (2MB huge page)
  }

  // Install PD into PDPT_hh
  pdpt_hh[pdpt_start] = fb_pd_phys | 0x03;

  uint64_t fb_vma_base = device_vma_base;
  device_vma_base += (uint64_t)num_fb_2mb * 0x200000;

  flush_tlb();

  // Save fb info for KMS driver
  g_fb_info.width = fb_w;
  g_fb_info.height = fb_h;
  g_fb_info.pitch = fb_pitch;
  g_fb_info.bpp = fb_bpp;
  g_fb_info.fb_vaddr = 0x700000;  // fixed user-space address for KMS process
  g_fb_info.fb_size = fb_bytes;
  g_fb_info.fb_phys = fb_phys;
}
