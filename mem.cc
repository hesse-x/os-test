#include "mem.h"
#include "common.h"
#include "fb.h"

// ===================== GDT =====================
static gdt_entry_t gdt[3];
static gdt_ptr_t gdt_reg;

void set_gdt_gate(int n, uint32_t base, uint32_t limit, uint8_t access,
                  uint8_t gran) {
  gdt[n].limit_low = L16(limit);
  gdt[n].base_low = L16(base);
  gdt[n].base_middle = (base >> 16) & 0xFF;
  gdt[n].access = access;
  gdt[n].granularity = ((gran & 0x0F) << 4) | ((limit >> 16) & 0x0F);
  gdt[n].base_high = (base >> 24) & 0xFF;
}

void set_gdt() {
  gdt_reg.base = (uint32_t)&gdt;
  gdt_reg.limit = sizeof(gdt) - 1;
  __asm__ volatile("lgdt (%0)" : : "r"(&gdt_reg));
  __asm__ volatile(
      "movw $0x10, %%ax\n"
      "movw %%ax, %%ds\n"
      "movw %%ax, %%es\n"
      "movw %%ax, %%fs\n"
      "movw %%ax, %%gs\n"
      "movw %%ax, %%ss\n"
      "ljmp $0x08, $1f\n"
      "1:\n" :::"eax");
}

void gdt_init() {
  set_gdt_gate(0, 0, 0, 0, 0);                   // null segment
  set_gdt_gate(1, 0, 0xFFFFFFFF, 0x9A, 0x0C);    // code: ER, ring0, 4K granularity, 32-bit
  set_gdt_gate(2, 0, 0xFFFFFFFF, 0x92, 0x0C);    // data: RW, ring0, 4K granularity, 32-bit
  set_gdt();
}

// ===================== 页表 =====================
__attribute__((aligned(4096))) uint32_t page_directory[1024];
__attribute__((aligned(4096))) uint32_t page_table[1024];

// ===================== enable_page =====================
// 在物理地址运行，由 start.S 调用
// 设置 identity map + higher-half 初始映射后返回
extern "C" void enable_page() {
  // GOTOFF 自动给出物理地址（因 enable_page 在物理地址运行）

  // 清零 PD 和 PT
  for (int i = 0; i < 1024; i++) {
    page_directory[i] = 0;
    page_table[i] = 0;
  }

  // 填充 PT：物理 0x0-0x3FFFFF → 4KB页，present + writable
  for (int i = 0; i < 1024; i++) {
    page_table[i] = (i * 4096) | 0x03;
  }

  // PD[0] = PT 物理地址 | flags（identity map: virt 0-4MB → phys 0-4MB）
  page_directory[0] = ((uintptr_t)page_table) | 0x03;

  // PD[768] = PT 物理地址 | flags（higher-half: virt 0xC0000000-0xC0400000 → phys 0-4MB）
  page_directory[768] = ((uintptr_t)page_table) | 0x03;

  // 启用分页
  __asm__ volatile(
      "movl %0, %%cr3\n"
      "movl %%cr0, %%eax\n"
      "orl $0x80000000, %%eax\n"
      "movl %%eax, %%cr0\n"
      :
      : "r"((uintptr_t)page_directory)
      : "eax", "memory");
}

// ===================== 全局变量定义 =====================
size_t total_page_frames = 0;
Page *BFCAllocator::frames = NULL;
Page *BFCAllocator::free_list = NULL;
uintptr_t device_vma_base = 0;

// ===================== Bump 分配器 =====================
// 极简物理内存分配器，仅在 BFC 初始化前使用
// 返回虚拟地址（phys + VMA_BASE），kernel_main_higher 可直接读写
static uintptr_t bump_next_phys; // 下一个空闲物理页地址

static void bump_init(uintptr_t start) {
  bump_next_phys = ALIGN_UP(start, PAGE_SIZE);
}

// 返回虚拟地址，kernel_main 可直接读写
void *bump_alloc(size_t size) {
  uintptr_t phys = bump_next_phys;
  bump_next_phys += ALIGN_UP(size, PAGE_SIZE);
  return (void *)(phys + VMA_BASE);
}

// ===================== BFCAllocator 实现 =====================
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

// ===================== multiboot 标签查找 =====================

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

// ===================== init_mem =====================
// 在虚拟地址运行（kernel_main 调用）

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
  bump_init(kernel_end_phys);

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

  // 8. 扩展 higher-half 映射：为超出 4MB 的物理 RAM 块分配 PT
  //    PD[768] 已映射初始 4MB, PD[769] 对应 0xC0400000...
  size_t max_4mb_block = (size_t)(max_phys_addr / 0x400000);
  for (size_t n = 1; n <= max_4mb_block; n++) {
    uint32_t *pt = (uint32_t *)bump_alloc(4096);
    uintptr_t pt_phys = PHY_ADDR((uintptr_t)pt);

    uint32_t phys_base = (uint32_t)(n * 0x400000);
    for (int i = 0; i < 1024; i++) {
      pt[i] = (phys_base + i * 4096) | 0x03;
    }

    page_directory[768 + n] = pt_phys | 0x03;
  }

  // 9. 设备映射区：RAM 映射之后的逻辑地址，类似 Linux ioremap 区域
  //    device_vma_base = ALIGN_UP(VMA_BASE + max_phys_addr, 4MB)
  //    显存物理地址映射到此区域，不是 identity map
  device_vma_base =
      ALIGN_UP(VMA_BASE + (uintptr_t)max_phys_addr, 0x400000);

  // 10. 刷新 TLB
  __asm__ volatile("movl %0, %%cr3\n" ::"r"(PHY_ADDR((uintptr_t)page_directory))
                   : "memory");

  // 12. 标记内核 + bump 分配的页为 USED
  uintptr_t used_start = KERNEL_LMA_BASE;
  uintptr_t used_end = bump_next_phys;
  size_t used_page_idx_start = PHY_TO_PAGE(used_start);
  size_t used_page_idx_end = PHY_TO_PAGE(ALIGN_UP(used_end, PAGE_SIZE));
  for (size_t i = used_page_idx_start;
       i < used_page_idx_end && i < total_page_frames; i++) {
    frames[i].status = PageStatus::USED;
  }

  // 13. 建立 free list
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

  // 14. 设置 BFCAllocator::frames
  BFCAllocator::frames = frames;

  // 15. 初始化 framebuffer（必须在 bump_alloc 和 device_vma_base 就绪之后）
  init_fb(mbi_addr);
}