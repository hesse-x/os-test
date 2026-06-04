#include "kernel/kernel.h"
#include "multiboot2.h"
#include <stdint.h>

#define BOOT_HEADER_ATTR __attribute__((section(".multiboot"), aligned(8)))
#define STACK_ATTR __attribute__((section(".stack"), aligned(16)))

constexpr size_t header_length = sizeof(multiboot_header) +
                                 sizeof(multiboot_header_tag_framebuffer) +
                                 sizeof(multiboot_header_tag);
constexpr size_t checksum = -(MULTIBOOT2_HEADER_MAGIC + 0 + header_length);

static const multiboot_header header BOOT_HEADER_ATTR = {
    .magic = MULTIBOOT2_HEADER_MAGIC,
    .architecture = 0,
    .header_length = header_length,
    .checksum = checksum};

static const multiboot_header_tag_framebuffer frame_tag BOOT_HEADER_ATTR = {
    .type = MULTIBOOT2_H_TAG_FRAMEBUFFER,
    .flags = 0,
    .size = sizeof(multiboot_header_tag_framebuffer),
    .width = 0,
    .height = 0,
    .depth = 0,
};

static const multiboot_header_tag end_tag BOOT_HEADER_ATTR = {
    .type = MULTIBOOT2_H_TAG_END,
    .flags = 0,
    .size = sizeof(multiboot_header_tag)};

extern "C" {
extern const uint8_t stack_bottom[8192] STACK_ATTR = {0};
}
