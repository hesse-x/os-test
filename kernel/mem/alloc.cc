#include "kernel/mem/alloc.h"
#include "arch/x64/paging.h"
#include "common/efi.h"
#include "common/common.h"
#include "common/macro.h"
#include "kernel/fb.h"
#include "kernel/serial.h"

// ===================== Global variable definitions =====================
size_t total_page_frames = 0;
Page *BFCAllocator::frames = NULL;
Page *BFCAllocator::free_list = NULL;

BFCAllocator bfc_alloc;

// ===================== BFCAllocator implementation =====================
void BFCAllocator::init() {
  // init_mem 中完成初始化
}

Page *BFCAllocator::alloc_page(size_t n) {
  if (n == 0 || free_list == NULL) {
    return NULL;
  }

  Page *cur = free_list;
  Page *prev = NULL;

  while (cur != NULL) {
    if (cur->cont_page_num >= n) {
      if (cur->cont_page_num == n) {
        if (prev == NULL) {
          free_list = cur->next;
        } else {
          prev->next = cur->next;
        }
        if (cur->next != NULL) {
          cur->next->prev = prev;
        }
        cur->status = PageStatus::USED;
        return cur;
      } else {
        size_t remaining = cur->cont_page_num - n;
        cur->cont_page_num = n;
        cur->status = PageStatus::USED;

        Page *new_block = cur + n;
        new_block->status = PageStatus::FREE;
        new_block->cont_page_num = remaining;
        new_block->prev = prev;
        new_block->next = cur->next;

        if (prev == NULL) {
          free_list = new_block;
        } else {
          prev->next = new_block;
        }
        if (cur->next != NULL) {
          cur->next->prev = new_block;
        }

        cur->prev = NULL;
        cur->next = NULL;
        return cur;
      }
    }
    prev = cur;
    cur = cur->next;
  }

  return NULL;
}

Page *BFCAllocator::free_page(Page *page, size_t n) {
  if (page == NULL || n == 0) {
    return NULL;
  }

  page->status = PageStatus::FREE;
  page->cont_page_num = n;

  if (free_list == NULL) {
    page->prev = NULL;
    page->next = NULL;
    free_list = page;
    return page;
  }

  Page *cur = free_list;
  Page *prev = NULL;

  while (cur != NULL && cur < page) {
    prev = cur;
    cur = cur->next;
  }

  page->prev = prev;
  page->next = cur;

  if (prev == NULL) {
    free_list = page;
  } else {
    prev->next = page;
  }

  if (cur != NULL) {
    cur->prev = page;
  }

  if (prev != NULL && prev + prev->cont_page_num == page) {
    prev->cont_page_num += page->cont_page_num;
    prev->next = cur;
    if (cur != NULL) {
      cur->prev = prev;
    }
    page = prev;
  }

  if (cur != NULL && page + page->cont_page_num == cur) {
    page->cont_page_num += cur->cont_page_num;
    page->next = cur->next;
    if (cur->next != NULL) {
      cur->next->prev = page;
    }
  }

  return page;
}

size_t BFCAllocator::free_page_nums() const {
  size_t total = 0;
  Page *cur = free_list;

  while (cur != NULL) {
    total += cur->cont_page_num;
    cur = cur->next;
  }

  return total;
}

// ===================== EFI mmap iteration helpers =====================
// EFI mmap 地址是物理地址，转虚拟: phys + VMA_BASE
static efi_memory_descriptor_t *get_efi_desc(boot_info *bi, size_t index) {
  uintptr_t mmap_virt = (uintptr_t)bi->mmap_addr + VMA_BASE;
  return (efi_memory_descriptor_t *)(mmap_virt + index * bi->mmap_desc_size);
}

// ===================== init_mem =====================
void init_mem(boot_info *bi) {
  serial_puts("init_mem: mmap_addr=");
  serial_put_hex(bi->mmap_addr);
  serial_puts(" mmap_size=");
  serial_put_hex(bi->mmap_size);
  serial_puts(" desc_size=");
  serial_put_hex(bi->mmap_desc_size);
  serial_puts("\n");

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
  uintptr_t kernel_end_phys = PHY_ADDR((uintptr_t)kernel_end);
  bump_init_phys(kernel_end_phys);

  // 3. Bump 分配 frames 数组
  size_t frames_size = total_page_frames * sizeof(Page);
  Page *frames = (Page *)bump_alloc(frames_size);

  // 4. 初始化 frames 为 RESERVED
  for (size_t i = 0; i < total_page_frames; i++) {
    frames[i].status = PageStatus::RESERVED;
    frames[i].cont_page_num = 1;
    frames[i].prev = NULL;
    frames[i].next = NULL;
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
        frames[page_idx + j].status = PageStatus::FREE;
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
    frames[i].status = PageStatus::USED;
  }

  // 9. 建立 free list
  int state = 0;
  Page *prev = NULL;
  Page **cur_page = &BFCAllocator::free_list;
  size_t cont_page_num = 0;
  for (size_t i = 0; i < total_page_frames; i++) {
    PageStatus status = frames[i].status;
    switch (state) {
    case 0: {
      if (status == PageStatus::FREE) {
        state = 1;
        *cur_page = frames + i;
        (*cur_page)->prev = prev;
        prev = *cur_page;
        cont_page_num = 1;
      }
      break;
    }
    case 1: {
      if (status != PageStatus::FREE) {
        state = 0;
        (*cur_page)->cont_page_num = cont_page_num;
        cont_page_num = 0;
        cur_page = &((*cur_page)->next);
      } else {
        cont_page_num++;
      }
      break;
    }
    }
  }
  if (state == 1) {
    (*cur_page)->cont_page_num = cont_page_num;
  }

  // 10. 设置 BFCAllocator::frames
  BFCAllocator::frames = frames;

  // 11. 初始化 framebuffer
  init_fb(bi);
}

// ===================== Address conversion =====================
uint64_t page_to_phys(Page *p) {
    return (uint64_t)(p - BFCAllocator::frames) * PAGE_SIZE;
}

uint64_t phys_to_virt(uint64_t phys) {
    return phys + VMA_BASE;
}
