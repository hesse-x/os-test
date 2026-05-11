// 最简C语言内核，直接操作VGA文本缓冲区打印字符串
#include <stddef.h>
#include <stdint.h>

#include "macro.h"
#include "multiboot2.h"

static void *frame_buffer;
static size_t frame_size;

static void fill_white() {
  char *s = (char *)frame_buffer;
  for (int32_t i = 0; i < frame_size; i++) {
    s[i] = 0xFF;
  }

}

static void init_screen(multiboot_tag_framebuffer *info) {
  frame_buffer = (void *)info->common.framebuffer_addr;
  uint32_t pitch = info->common.framebuffer_pitch;
  uint32_t height = info->common.framebuffer_height;
  frame_size = pitch * height;
  fill_white();
}

static multiboot_tag *readinfo(multiboot_tag *info) {
  int16_t type = info->type;
  switch (type) {
  case MULTIBOOT_TAG_TYPE_END: {
    return NULL;
  }
    //    case MULTIBOOT_TAG_TYPE_CMDLINE: {
    //      multiboot_tag_string *cur =
    //        (multiboot_tag_string *)info;
    //      return (multiboot_tag *)(cur + 1);
    //    }
    //    case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME: {
    //      multiboot_tag_string *cur =
    //        (multiboot_tag_string *)info;
    //      return (multiboot_tag *)(cur + 1);
    //    }
    //    case MULTIBOOT_TAG_TYPE_MODULE: {
    //      multiboot_tag_module *cur =
    //        (multiboot_tag_module *)info;
    //      return (multiboot_tag *)(cur + 1);
    //    }
    //    case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO: {
    //      multiboot_tag_basic_meminfo *cur =
    //        (multiboot_tag_basic_meminfo *)info;
    //      return (multiboot_tag *)(cur + 1);
    //    }
  case MULTIBOOT_TAG_TYPE_FRAMEBUFFER: {
    multiboot_tag_framebuffer *cur = (multiboot_tag_framebuffer *)info;
    init_screen(cur);
    break;
  }
    //    case MULTIBOOT_TAG_TYPE_LOAD_BASE_ADDR: {
    //      multiboot_tag_load_base_addr *cur =
    //        (multiboot_tag_load_base_addr *)info;
    //      return (multiboot_tag *)(cur + 1);
    //    }
  }
  return (multiboot_tag *)((uintptr_t)info +
                           ALIGN_UP(info->size, MULTIBOOT_TAG_ALIGN));
}

extern "C" {
// 内核主函数（入口）
void kernel_main(int32_t magic_num, uintptr_t addr) {
  multiboot_tag *cur = (multiboot_tag *)(addr + 8);
  while (cur != NULL) {
    cur = readinfo(cur);
  }
}
} // extern C
