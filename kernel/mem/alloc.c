#include "kernel/mem/alloc.h"
#include "arch/x64/paging.h"
#include "kernel/efi.h"
#include "arch/x64/memlayout.h"
#include "common/macro.h"
#include "kernel/log.h"
#include "kernel/mem/kasan.h"
#include "kernel/mem/slab.h"

// ===================== Global variable definitions =====================
size_t total_page_frames = 0;
Page *bfc_frames = NULL;
Page *bfc_free_list = NULL;

spinlock_t bfc_lock = SPINLOCK_INIT;

// ===================== BFC allocator implementation =====================
void bfc_init(void) {
  // init_mem 中完成初始化
}

__attribute__((no_sanitize("kernel-address")))
Page *bfc_alloc_page(size_t n) {
  if (n == 0) return NULL;

  uint64_t flags;
  spin_lock_irqsave(&bfc_lock, &flags);

  if (bfc_free_list == NULL) {
    spin_unlock_irqrestore(&bfc_lock, flags);
    return NULL;
  }

  Page *cur = bfc_free_list;
  Page *prev = NULL;

  while (cur != NULL) {
    if (cur->bfc.cont_page_num >= n) {
      if (cur->bfc.cont_page_num == n) {
        if (prev == NULL) {
          bfc_free_list = cur->bfc.next;
        } else {
          prev->bfc.next = cur->bfc.next;
        }
        if (cur->bfc.next != NULL) {
          cur->bfc.next->bfc.prev = prev;
        }
        for (size_t i = 0; i < n; i++) {
          cur[i].status = PAGE_USED;
          refcount_set(&cur[i].p_refcount, 1);
        }
        spin_unlock_irqrestore(&bfc_lock, flags);
        memstat_add(&kernel_mem_stats.used_pages, (int)n);
        kasan_bfc_alloc((__force void *)phys_to_virt((__force phys_addr_t)page_to_phys(cur)), n * PAGE_SIZE);
        return cur;
      } else {
        size_t remaining = cur->bfc.cont_page_num - n;
        cur->bfc.cont_page_num = n;
        for (size_t i = 0; i < n; i++) {
          cur[i].status = PAGE_USED;
          refcount_set(&cur[i].p_refcount, 1);
        }

        Page *new_block = cur + n;
        new_block->status = PAGE_FREE;
        new_block->bfc.cont_page_num = remaining;
        new_block->bfc.prev = prev;
        new_block->bfc.next = cur->bfc.next;

        if (prev == NULL) {
          bfc_free_list = new_block;
        } else {
          prev->bfc.next = new_block;
        }
        if (cur->bfc.next != NULL) {
          cur->bfc.next->bfc.prev = new_block;
        }

        cur->bfc.prev = NULL;
        cur->bfc.next = NULL;
        spin_unlock_irqrestore(&bfc_lock, flags);
        memstat_add(&kernel_mem_stats.used_pages, (int)n);
        kasan_bfc_alloc((__force void *)phys_to_virt((__force phys_addr_t)page_to_phys(cur)), n * PAGE_SIZE);
        return cur;
      }
    }
    prev = cur;
    cur = cur->bfc.next;
  }

  spin_unlock_irqrestore(&bfc_lock, flags);
  return NULL;
}

__attribute__((no_sanitize("kernel-address")))
Page *bfc_free_page(Page *page, size_t n) {
  if (page == NULL || n == 0) {
    return NULL;
  }

  uint64_t flags;
  spin_lock_irqsave(&bfc_lock, &flags);

  memstat_sub(&kernel_mem_stats.used_pages, (int)n);
  page->status = PAGE_FREE;
  page->bfc.cont_page_num = n;

  if (bfc_free_list == NULL) {
    page->bfc.prev = NULL;
    page->bfc.next = NULL;
    bfc_free_list = page;
    spin_unlock_irqrestore(&bfc_lock, flags);
    return page;
  }

  Page *cur = bfc_free_list;
  Page *prev = NULL;

  while (cur != NULL && cur < page) {
    prev = cur;
    cur = cur->bfc.next;
  }

  page->bfc.prev = prev;
  page->bfc.next = cur;

  if (prev == NULL) {
    bfc_free_list = page;
  } else {
    prev->bfc.next = page;
  }

  if (cur != NULL) {
    cur->bfc.prev = page;
  }

  if (prev != NULL && prev + prev->bfc.cont_page_num == page) {
    prev->bfc.cont_page_num += page->bfc.cont_page_num;
    prev->bfc.next = cur;
    if (cur != NULL) {
      cur->bfc.prev = prev;
    }
    page = prev;
  }

  if (cur != NULL && page + page->bfc.cont_page_num == cur) {
    page->bfc.cont_page_num += cur->bfc.cont_page_num;
    page->bfc.next = cur->bfc.next;
    if (cur->bfc.next != NULL) {
      cur->bfc.next->bfc.prev = page;
    }
  }

  spin_unlock_irqrestore(&bfc_lock, flags);
  return page;
}

__attribute__((no_sanitize("kernel-address")))
Page *bfc_alloc_page_low(size_t n) {
  if (n == 0) return NULL;

  uint64_t flags;
  spin_lock_irqsave(&bfc_lock, &flags);

  if (bfc_free_list == NULL) {
    spin_unlock_irqrestore(&bfc_lock, flags);
    return NULL;
  }

  Page *cur = bfc_free_list;
  Page *prev = NULL;

  while (cur != NULL) {
    uint64_t phys = (uint64_t)(cur - bfc_frames) * PAGE_SIZE;
    if (cur->bfc.cont_page_num >= n && phys + (uint64_t)n * PAGE_SIZE <= 0x100000000ULL) {
      // Same split logic as alloc_page
      if (cur->bfc.cont_page_num == n) {
        if (prev == NULL) {
          bfc_free_list = cur->bfc.next;
        } else {
          prev->bfc.next = cur->bfc.next;
        }
        if (cur->bfc.next != NULL) {
          cur->bfc.next->bfc.prev = prev;
        }
        for (size_t i = 0; i < n; i++) {
          cur[i].status = PAGE_USED;
          refcount_set(&cur[i].p_refcount, 1);
        }
        spin_unlock_irqrestore(&bfc_lock, flags);
        memstat_add(&kernel_mem_stats.used_pages, (int)n);
        kasan_bfc_alloc((__force void *)phys_to_virt((__force phys_addr_t)page_to_phys(cur)), n * PAGE_SIZE);
        return cur;
      } else {
        size_t remaining = cur->bfc.cont_page_num - n;
        cur->bfc.cont_page_num = n;
        for (size_t i = 0; i < n; i++) {
          cur[i].status = PAGE_USED;
          refcount_set(&cur[i].p_refcount, 1);
        }

        Page *new_block = cur + n;
        new_block->status = PAGE_FREE;
        new_block->bfc.cont_page_num = remaining;
        new_block->bfc.prev = prev;
        new_block->bfc.next = cur->bfc.next;

        if (prev == NULL) {
          bfc_free_list = new_block;
        } else {
          prev->bfc.next = new_block;
        }
        if (cur->bfc.next != NULL) {
          cur->bfc.next->bfc.prev = new_block;
        }

        cur->bfc.prev = NULL;
        cur->bfc.next = NULL;
        spin_unlock_irqrestore(&bfc_lock, flags);
        memstat_add(&kernel_mem_stats.used_pages, (int)n);
        kasan_bfc_alloc((__force void *)phys_to_virt((__force phys_addr_t)page_to_phys(cur)), n * PAGE_SIZE);
        return cur;
      }
    }
    prev = cur;
    cur = cur->bfc.next;
  }

  spin_unlock_irqrestore(&bfc_lock, flags);
  return NULL;
}

__attribute__((no_sanitize("kernel-address")))
size_t bfc_free_page_nums(void) {
  uint64_t flags;
  spin_lock_irqsave(&bfc_lock, &flags);
  size_t total = 0;
  Page *cur = bfc_free_list;

  while (cur != NULL) {
    total += cur->bfc.cont_page_num;
    cur = cur->bfc.next;
  }

  spin_unlock_irqrestore(&bfc_lock, flags);
  return total;
}

// ===================== EFI mmap iteration helpers =====================
// EFI mmap 地址是物理地址，转虚拟: phys + VMA_BASE
static efi_memory_descriptor_t *get_efi_desc(boot_info *bi, size_t index) {
  uintptr_t mmap_virt = (uintptr_t)bi->mmap_addr + VMA_BASE;
  return (efi_memory_descriptor_t *)(mmap_virt + index * bi->mmap_desc_size);
}

// ===================== init_mem =====================
__attribute__((no_sanitize("kernel-address")))
void init_mem(boot_info *bi) {
  printk(LOG_INFO, "init_mem: mmap_addr=0x%016X mmap_size=0x%016X desc_size=0x%016X\n",
                 bi->mmap_addr, bi->mmap_size, bi->mmap_desc_size);

  size_t desc_count = bi->mmap_size / bi->mmap_desc_size;

  // 1. 计算 AVAILABLE 内存最大物理地址 + 总页帧数
  uint64_t max_phys_addr = 0;
  for (size_t i = 0; i < desc_count; i++) {
    efi_memory_descriptor_t *desc = get_efi_desc(bi, i);
    if (desc->type == EfiConventionalMemory) {
      uint64_t end = desc->physical_start + desc->number_of_pages * 4096;
      if (end > max_phys_addr) max_phys_addr = end;
    }
  }
  total_page_frames = GET_PAGE_NUM(max_phys_addr);

  // 2. Bump 分配器初始化
  uintptr_t kernel_end_phys = (__force uintptr_t)PHY_ADDR((uintptr_t)kernel_end);
  bump_init_phys(kernel_end_phys);

  // 3. Bump 分配 frames 数组
  size_t frames_size = total_page_frames * sizeof(Page);
  Page *frames = (Page *)bump_alloc(frames_size);

  // 4. 初始化 frames 为 RESERVED
  for (size_t i = 0; i < total_page_frames; i++) {
    frames[i].status = PAGE_RESERVED;
    frames[i].bfc.cont_page_num = 1;
    frames[i].bfc.prev = NULL;
    frames[i].bfc.next = NULL;
    refcount_set(&frames[i].p_refcount, 0);
  }

  // 5. 根据 EFI mmap 标记 FREE 页
  for (size_t i = 0; i < desc_count; i++) {
    efi_memory_descriptor_t *desc = get_efi_desc(bi, i);
    if (desc->type == EfiConventionalMemory) {
      uint64_t addr = desc->physical_start;
      uint64_t len = desc->number_of_pages * 4096;
      size_t page_num = GET_PAGE_NUM(len);
      size_t page_idx = PHY_TO_PAGE(ALIGN_UP(addr, PAGE_SIZE));
      for (size_t j = 0; j < page_num && page_idx + j < total_page_frames;
           j++) {
        frames[page_idx + j].status = PAGE_FREE;
      }
    }
  }

  // 6. 扩展 higher-half 映射 + 设备映射区
  extend_mapping(max_phys_addr);

  // 7. 刷新 TLB
  flush_tlb();

  // 8. 标记内核 + bump 分配的页为 USED
  uintptr_t used_start = bi->kernel_phys;
  uintptr_t used_end = bump_end_phys();
  size_t used_page_idx_start = PHY_TO_PAGE(used_start);
  size_t used_page_idx_end = PHY_TO_PAGE(ALIGN_UP(used_end, PAGE_SIZE));
  for (size_t i = used_page_idx_start;
       i < used_page_idx_end && i < total_page_frames; i++) {
    frames[i].status = PAGE_USED;
    refcount_set(&frames[i].p_refcount, 1);
  }

  // 9. 建立 free list
  int state = 0;
  Page *prev = NULL;
  Page **cur_page = &bfc_free_list;
  size_t cont_page_num = 0;
  for (size_t i = 0; i < total_page_frames; i++) {
    page_status_t status = frames[i].status;
    switch (state) {
    case 0: {
      if (status == PAGE_FREE) {
        state = 1;
        *cur_page = frames + i;
        (*cur_page)->bfc.prev = prev;
        prev = *cur_page;
        cont_page_num = 1;
      }
      break;
    }
    case 1: {
      if (status != PAGE_FREE) {
        state = 0;
        (*cur_page)->bfc.cont_page_num = cont_page_num;
        cont_page_num = 0;
        cur_page = &((*cur_page)->bfc.next);
      } else {
        cont_page_num++;
      }
      break;
    }
    }
  }
  if (state == 1) {
    (*cur_page)->bfc.cont_page_num = cont_page_num;
  }

  // 10. 设置 bfc_frames
  bfc_frames = frames;
}

// ===================== Address conversion =====================
__attribute__((no_sanitize("kernel-address")))
phys_addr_t page_to_phys(Page *p) {
    return (__force phys_addr_t)((uint64_t)(p - bfc_frames) * PAGE_SIZE);
}

__attribute__((no_sanitize("kernel-address")))
kern_vaddr_t phys_to_virt(phys_addr_t phys) {
    return (__force kern_vaddr_t)((__force uint64_t)phys + VMA_BASE);
}
