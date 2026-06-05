#include "kernel/mem/alloc.h"
#include "arch/x86/paging.h"
#include "common/common.h"
#include "common/macro.h"
#include "arch/x86/multiboot2.h"
#include "driver/fb.h"

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

// ===================== multiboot tag lookup =====================
static multiboot_tag_mmap *find_mmap_tag(uintptr_t mbi_addr) {
  multiboot_tag *tag = (multiboot_tag *)(mbi_addr + 8);
  while (tag->type != MULTIBOOT_TAG_TYPE_END) {
    if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
      return (multiboot_tag_mmap *)tag;
    }
    tag = (multiboot_tag *)((uintptr_t)tag +
                             ALIGN_UP(tag->size, MULTIBOOT_TAG_ALIGN));
  }
  return NULL;
}

// ===================== init_mem =====================
static size_t compute_total_page_frames(multiboot_tag_mmap *mmap_tag) {
  uint64_t max_phys_addr = 0;
  size_t num_entries =
      (mmap_tag->size - offsetof(multiboot_tag_mmap, entries)) /
      mmap_tag->entry_size;
  const multiboot_mmap_entry *entry = mmap_tag->entries;
  for (size_t i = 0; i < num_entries; i++) {
    if (entry[i].type == MULTIBOOT_MEMORY_AVAILABLE) {
      uint64_t entry_end = entry[i].addr + entry[i].len;
      if (entry_end > max_phys_addr) {
        max_phys_addr = entry_end;
      }
    }
  }
  return GET_PAGE_NUM(max_phys_addr);
}

// Forward declaration from arch/x86/paging.cc
extern "C" uintptr_t bump_end_phys();

void init_mem(uintptr_t mbi_addr) {
  // 1. 解析 multiboot2 标签
  multiboot_tag_mmap *mmap_tag = find_mmap_tag(mbi_addr);
  if (mmap_tag == NULL) {
    return;
  }

  // 2. 计算总页帧数（仅 AVAILABLE 内存）
  total_page_frames = compute_total_page_frames(mmap_tag);

  // 3. Bump 分配器初始化
  uintptr_t kernel_end_phys = PHY_ADDR((uintptr_t)kernel_end);
  bump_init_phys(kernel_end_phys);

  // 4. Bump 分配 frames 数组
  size_t frames_size = total_page_frames * sizeof(Page);
  Page *frames = (Page *)bump_alloc(frames_size);

  // 5. 初始化 frames 为 RESERVED
  for (size_t i = 0; i < total_page_frames; i++) {
    frames[i].status = PageStatus::RESERVED;
    frames[i].cont_page_num = 1;
    frames[i].prev = NULL;
    frames[i].next = NULL;
  }

  // 6. 根据 mmap 标记 FREE 页
  size_t num_entries =
      (mmap_tag->size - offsetof(multiboot_tag_mmap, entries)) /
      mmap_tag->entry_size;
  const multiboot_mmap_entry *entry = mmap_tag->entries;
  for (size_t i = 0; i < num_entries; i++) {
    if (entry[i].type == MULTIBOOT_MEMORY_AVAILABLE) {
      uint64_t entry_addr = entry[i].addr;
      uint64_t entry_len = entry[i].len;
      size_t page_num = GET_PAGE_NUM(entry_len);
      size_t page_idx = PHY_TO_PAGE(ALIGN_UP(entry_addr, PAGE_SIZE));
      for (size_t j = 0; j < page_num && page_idx + j < total_page_frames;
           j++) {
        frames[page_idx + j].status = PageStatus::FREE;
      }
    }
  }

  // 7. 计算 AVAILABLE 内存最大物理地址（用于 higher-half RAM 映射范围）
  uint64_t max_phys_addr = 0;
  for (size_t i = 0; i < num_entries; i++) {
    if (entry[i].type == MULTIBOOT_MEMORY_AVAILABLE) {
      uint64_t entry_end = entry[i].addr + entry[i].len;
      if (entry_end > max_phys_addr) {
        max_phys_addr = entry_end;
      }
    }
  }

  // 8. 扩展 higher-half 映射 + 设备映射区（arch 层）
  extend_mapping(max_phys_addr);

  // 9. 刷新 TLB
  flush_tlb();

  // 10. 标记内核 + bump 分配的页为 USED
  uintptr_t used_start = KERNEL_LMA_BASE;
  uintptr_t used_end = bump_end_phys();
  size_t used_page_idx_start = PHY_TO_PAGE(used_start);
  size_t used_page_idx_end = PHY_TO_PAGE(ALIGN_UP(used_end, PAGE_SIZE));
  for (size_t i = used_page_idx_start;
       i < used_page_idx_end && i < total_page_frames; i++) {
    frames[i].status = PageStatus::USED;
  }

  // 11. 建立 free list
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

  // 12. 设置 BFCAllocator::frames
  BFCAllocator::frames = frames;

  // 13. 初始化 framebuffer（必须在 bump_alloc 和 device_vma_base 就绪之后）
  init_fb(mbi_addr);
}
