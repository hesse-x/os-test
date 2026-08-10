/*
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include "drm/drm.h"
#include "drm/drm_fourcc.h"
#include "drm/drm_mode.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#ifdef UDMABUF_VULKAN_KMS
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#endif
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define MANIFEST "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
#define ICD_PATH "/lib/libvulkan_lvp.so"
#define CLEAR_R 17u
#define CLEAR_G 149u
#define CLEAR_B 53u
#define CLEAR_A 255u

struct context {
  int drm_fd;
  int prime_fd;
  int map_fd;
#ifdef UDMABUF_VULKAN_KMS
  int memfd;
  int udmabuf_fd;
#endif
  void *map;
  struct drm_mode_create_dumb dumb;
  uint32_t fb_id;
  uint32_t bootstrap_fb_id;
  struct drm_mode_create_dumb bootstrap;
  uint32_t connector_id;
  uint32_t crtc_id;
  struct drm_mode_modeinfo mode;

  VkInstance instance;
  VkPhysicalDevice physical;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family;
  VkImage image;
  VkDeviceMemory memory;
  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
  VkFence fence;
};

static int fail_errno(const char *api) {
  fprintf(stderr, "[VULKAN-S2] %s failed: errno=%d (%s)\n", api, errno,
          strerror(errno));
  return -1;
}

static int fail_vk(const char *api, VkResult result) {
  fprintf(stderr, "[VULKAN-S2] %s failed: VkResult=%d\n", api, (int)result);
  return -1;
}

static int loader_preflight(void) {
  puts("[VULKAN-S2][01] loader/lavapipe preflight");
  if (setenv("VK_DRIVER_FILES", MANIFEST, 1) ||
      setenv("VK_ICD_FILENAMES", MANIFEST, 1) ||
      setenv("VK_LOADER_DEBUG", "error,warn,driver", 0))
    return fail_errno("setenv");
  void *icd = dlopen(ICD_PATH, RTLD_NOW | RTLD_LOCAL);
  if (!icd) {
    fprintf(stderr, "[VULKAN-S2] dlopen(%s): %s\n", ICD_PATH, dlerror());
    return -1;
  }
  int ok = dlsym(icd, "vk_icdGetInstanceProcAddr") != NULL &&
           dlsym(icd, "vk_icdNegotiateLoaderICDInterfaceVersion") != NULL;
  dlclose(icd);
  return ok ? 0 : -1;
}

static int select_mode(struct context *ctx) {
  puts("[VULKAN-S2][02] open DRM and select mode");
  ctx->drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (ctx->drm_fd < 0)
    return fail_errno("open(card0)");
  if (ioctl(ctx->drm_fd, DRM_IOCTL_SET_MASTER, 0) < 0 && errno != EBUSY)
    return fail_errno("DRM_IOCTL_SET_MASTER");

  struct drm_mode_card_res res = {0};
  if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 ||
      res.count_crtcs != 1 || res.count_connectors != 1)
    return fail_errno("DRM_IOCTL_MODE_GETRESOURCES(count)");
  uint32_t crtc = 0, connector = 0;
  res.crtc_id_ptr = (uintptr_t)&crtc;
  res.connector_id_ptr = (uintptr_t)&connector;
  if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
    return fail_errno("DRM_IOCTL_MODE_GETRESOURCES(ids)");
  ctx->crtc_id = crtc;
  ctx->connector_id = connector;

  struct drm_mode_crtc current = {.crtc_id = crtc};
  if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_GETCRTC, &current) < 0)
    return fail_errno("DRM_IOCTL_MODE_GETCRTC");
  if (current.mode_valid) {
    ctx->mode = current.mode;
  } else {
    struct drm_mode_get_connector conn = {.connector_id = connector};
    if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
      return fail_errno("DRM_IOCTL_MODE_GETCONNECTOR(count)");
    if (!conn.count_modes || conn.count_modes > 4096)
      return -1;
    struct drm_mode_modeinfo *modes =
        calloc(conn.count_modes, sizeof(struct drm_mode_modeinfo));
    if (!modes)
      return fail_errno("calloc(connector modes)");
    conn.modes_ptr = (uintptr_t)modes;
    int rc = ioctl(ctx->drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
    if (!rc && conn.count_modes)
      ctx->mode = modes[0];
    free(modes);
    if (rc < 0)
      return fail_errno("DRM_IOCTL_MODE_GETCONNECTOR(modes)");
  }
  if (!ctx->mode.hdisplay || !ctx->mode.vdisplay)
    return -1;
  return 0;
}

static int create_dumb_and_prime(struct context *ctx) {
#ifdef UDMABUF_VULKAN_KMS
  puts("[VULKAN-S2.5][03] create memfd/udmabuf and import into DRM");
  ctx->dumb.width = ctx->mode.hdisplay;
  ctx->dumb.height = ctx->mode.vdisplay;
  ctx->dumb.bpp = 32;
  ctx->dumb.pitch = ctx->dumb.width * 4;
  uint64_t bytes = (uint64_t)ctx->dumb.pitch * ctx->dumb.height;
  ctx->dumb.size = (bytes + 4095) & ~4095ULL;

  ctx->memfd = memfd_create("udmabuf-vulkan-kms", MFD_ALLOW_SEALING);
  if (ctx->memfd < 0)
    return fail_errno("memfd_create");
  if (ftruncate(ctx->memfd, (off_t)ctx->dumb.size) < 0 ||
      fcntl(ctx->memfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0)
    return fail_errno("prepare sealed memfd");
  ctx->udmabuf_fd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
  if (ctx->udmabuf_fd < 0)
    return fail_errno("open(/dev/udmabuf)");
  struct udmabuf_create create = {
      .memfd = (uint32_t)ctx->memfd,
      .flags = UDMABUF_FLAGS_CLOEXEC,
      .size = ctx->dumb.size,
  };
  ctx->prime_fd = ioctl(ctx->udmabuf_fd, UDMABUF_CREATE, &create);
  if (ctx->prime_fd < 0)
    return fail_errno("UDMABUF_CREATE");

  struct drm_prime_handle prime = {.fd = ctx->prime_fd};
  if (ioctl(ctx->drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0)
    return fail_errno("DRM_IOCTL_PRIME_FD_TO_HANDLE");
  ctx->dumb.handle = prime.handle;
  close(ctx->memfd);
  ctx->memfd = -1;
  return 0;
#else
  puts("[VULKAN-S2][03] create dumb and export PRIME fd");
  ctx->dumb.width = ctx->mode.hdisplay;
  ctx->dumb.height = ctx->mode.vdisplay;
  ctx->dumb.bpp = 32;
  if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &ctx->dumb) < 0)
    return fail_errno("DRM_IOCTL_MODE_CREATE_DUMB");
  if ((uint64_t)ctx->dumb.pitch * ctx->dumb.height > ctx->dumb.size)
    return -1;
  struct drm_prime_handle prime = {.handle = ctx->dumb.handle,
                                   .flags = DRM_CLOEXEC | DRM_RDWR};
  if (ioctl(ctx->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0)
    return fail_errno("DRM_IOCTL_PRIME_HANDLE_TO_FD");
  ctx->prime_fd = prime.fd;
  return 0;
#endif
}

static int prime_preflight(struct context *ctx) {
  puts("[VULKAN-S2][04] verify PRIME fd semantics");
  struct stat st;
  if (fstat(ctx->prime_fd, &st) || (uint64_t)st.st_size != ctx->dumb.size ||
      !S_ISREG(st.st_mode) || lseek(ctx->prime_fd, 0, SEEK_END) != st.st_size ||
      lseek(ctx->prime_fd, 0, SEEK_SET) != 0)
    return fail_errno("PRIME fstat/lseek");
  struct pollfd pfd = {.fd = ctx->prime_fd, .events = POLLIN | POLLOUT};
  if (poll(&pfd, 1, 0) != 1 ||
      (pfd.revents & (POLLIN | POLLOUT)) != (POLLIN | POLLOUT))
    return fail_errno("PRIME poll");
  ctx->map_fd = fcntl(ctx->prime_fd, F_DUPFD_CLOEXEC, 0);
  if (ctx->map_fd < 0)
    return fail_errno("F_DUPFD_CLOEXEC");
  ctx->map = mmap(NULL, ctx->dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  ctx->map_fd, 0);
  if (ctx->map == MAP_FAILED) {
    ctx->map = NULL;
    return fail_errno("PRIME mmap");
  }
  return 0;
}

static int has_extension(VkPhysicalDevice physical, const char *wanted) {
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) !=
          VK_SUCCESS ||
      !count || count > 1024)
    return 0;
  VkExtensionProperties *props = calloc(count, sizeof(*props));
  if (!props)
    return 0;
  VkResult result =
      vkEnumerateDeviceExtensionProperties(physical, NULL, &count, props);
  int found = 0;
  if (result == VK_SUCCESS) {
    for (uint32_t i = 0; i < count; i++)
      found |= strcmp(props[i].extensionName, wanted) == 0;
  }
  free(props);
  return found;
}

static int init_vulkan(struct context *ctx) {
  puts("[VULKAN-S2][05] query external-memory/modifier capabilities");
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "vulkan_drm_smoke",
                           .apiVersion = VK_API_VERSION_1_2};
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &app};
  VkResult result = vkCreateInstance(&ici, NULL, &ctx->instance);
  if (result != VK_SUCCESS)
    return fail_vk("vkCreateInstance", result);
  uint32_t count = 1;
  result = vkEnumeratePhysicalDevices(ctx->instance, &count, &ctx->physical);
  if (result != VK_SUCCESS || count != 1)
    return fail_vk("vkEnumeratePhysicalDevices", result);

  VkPhysicalDeviceDriverProperties driver = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 props = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &driver};
  vkGetPhysicalDeviceProperties2(ctx->physical, &props);
  if (driver.driverID != VK_DRIVER_ID_MESA_LLVMPIPE)
    return -1;

  const char *extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                              VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                              VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
                              VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
  for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
    if (!has_extension(ctx->physical, extensions[i])) {
      fprintf(stderr, "[VULKAN-S2] missing device extension %s\n",
              extensions[i]);
      return -1;
    }
  }

  uint32_t qcount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical, &qcount, NULL);
  VkQueueFamilyProperties *queues = calloc(qcount, sizeof(*queues));
  if (!queues)
    return -1;
  vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical, &qcount, queues);
  ctx->queue_family = UINT32_MAX;
  for (uint32_t i = 0; i < qcount; i++) {
    if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      ctx->queue_family = i;
      break;
    }
  }
  free(queues);
  if (ctx->queue_family == UINT32_MAX)
    return -1;

  VkDrmFormatModifierPropertiesEXT modifier = {0};
  VkDrmFormatModifierPropertiesListEXT modifier_list = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
      .drmFormatModifierCount = 1,
      .pDrmFormatModifierProperties = &modifier};
  VkFormatProperties2 format_props = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &modifier_list};
  vkGetPhysicalDeviceFormatProperties2(ctx->physical, VK_FORMAT_B8G8R8A8_UNORM,
                                       &format_props);
  VkFormatFeatureFlags required = VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
  if (modifier_list.drmFormatModifierCount != 1 ||
      modifier.drmFormatModifier != DRM_FORMAT_MOD_LINEAR ||
      modifier.drmFormatModifierPlaneCount != 1 ||
      (modifier.drmFormatModifierTilingFeatures & required) != required)
    return -1;

  VkPhysicalDeviceExternalImageFormatInfo external = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info = {
      .sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
      .pNext = &external,
      .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
  VkPhysicalDeviceImageFormatInfo2 image_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .pNext = &mod_info,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .type = VK_IMAGE_TYPE_2D,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
  VkExternalImageFormatProperties external_props = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
  VkImageFormatProperties2 image_props = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
      .pNext = &external_props};
  result = vkGetPhysicalDeviceImageFormatProperties2(ctx->physical, &image_info,
                                                     &image_props);
  VkExternalMemoryProperties memory_props =
      external_props.externalMemoryProperties;
  if (result != VK_SUCCESS)
    return fail_vk("vkGetPhysicalDeviceImageFormatProperties2", result);
  if (!(memory_props.externalMemoryFeatures &
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
      !(memory_props.compatibleHandleTypes &
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)) {
    fprintf(stderr,
            "[VULKAN-S2] DMA-BUF import unsupported: features=%#x "
            "compatible=%#x export-from-imported=%#x\n",
            memory_props.externalMemoryFeatures,
            memory_props.compatibleHandleTypes,
            memory_props.exportFromImportedHandleTypes);
    return -1;
  }

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {.sType =
                                     VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 .queueFamilyIndex = ctx->queue_family,
                                 .queueCount = 1,
                                 .pQueuePriorities = &priority};
  VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                            .queueCreateInfoCount = 1,
                            .pQueueCreateInfos = &qci,
                            .enabledExtensionCount =
                                sizeof(extensions) / sizeof(extensions[0]),
                            .ppEnabledExtensionNames = extensions};
  result = vkCreateDevice(ctx->physical, &dci, NULL, &ctx->device);
  if (result != VK_SUCCESS)
    return fail_vk("vkCreateDevice", result);
  vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);
  return 0;
}

static int import_image(struct context *ctx) {
  puts("[VULKAN-S2][06] create image and import memory");
  VkSubresourceLayout plane = {.offset = 0, .rowPitch = ctx->dumb.pitch};
  VkImageDrmFormatModifierExplicitCreateInfoEXT explicit = {
      .sType =
          VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
      .drmFormatModifierPlaneCount = 1,
      .pPlaneLayouts = &plane};
  VkExternalMemoryImageCreateInfo external = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &explicit,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                           .pNext = &external,
                           .imageType = VK_IMAGE_TYPE_2D,
                           .format = VK_FORMAT_B8G8R8A8_UNORM,
                           .extent = {ctx->dumb.width, ctx->dumb.height, 1},
                           .mipLevels = 1,
                           .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT,
                           .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                           .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  VkResult result = vkCreateImage(ctx->device, &ici, NULL, &ctx->image);
  if (result != VK_SUCCESS)
    return fail_vk("vkCreateImage", result);
  VkMemoryRequirements requirements;
  vkGetImageMemoryRequirements(ctx->device, ctx->image, &requirements);
  if (requirements.size > ctx->dumb.size)
    return -1;

  PFN_vkGetMemoryFdPropertiesKHR get_fd_properties =
      (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
          ctx->device, "vkGetMemoryFdPropertiesKHR");
  if (!get_fd_properties)
    return -1;
  VkMemoryFdPropertiesKHR fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  result = get_fd_properties(ctx->device,
                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                             ctx->prime_fd, &fd_props);
  uint32_t bits = fd_props.memoryTypeBits & requirements.memoryTypeBits;
  if (result != VK_SUCCESS || !bits)
    return fail_vk("vkGetMemoryFdPropertiesKHR", result);
  uint32_t memory_type = 0;
  while (!(bits & (1u << memory_type)))
    memory_type++;

  VkImportMemoryFdInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = ctx->prime_fd};
  VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &import,
      .image = ctx->image};
  VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .pNext = &dedicated,
                              .allocationSize = requirements.size,
                              .memoryTypeIndex = memory_type};
  result = vkAllocateMemory(ctx->device, &mai, NULL, &ctx->memory);
  if (result != VK_SUCCESS)
    return fail_vk("vkAllocateMemory(import)", result);
  ctx->prime_fd = -1; /* Vulkan owns it after successful import. */
  result = vkBindImageMemory(ctx->device, ctx->image, ctx->memory, 0);
  return result == VK_SUCCESS ? 0 : fail_vk("vkBindImageMemory", result);
}

static int clear_image(struct context *ctx) {
  puts("[VULKAN-S2][07] submit clear and wait fence");
  VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = ctx->queue_family};
  VkResult result =
      vkCreateCommandPool(ctx->device, &pool_info, NULL, &ctx->command_pool);
  if (result != VK_SUCCESS)
    return fail_vk("vkCreateCommandPool", result);
  VkCommandBufferAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = ctx->command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
  result = vkAllocateCommandBuffers(ctx->device, &alloc, &ctx->command_buffer);
  if (result != VK_SUCCESS)
    return fail_vk("vkAllocateCommandBuffers", result);
  VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  if ((result = vkBeginCommandBuffer(ctx->command_buffer, &begin)) !=
      VK_SUCCESS)
    return fail_vk("vkBeginCommandBuffer", result);

  VkImageSubresourceRange range = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                   .levelCount = 1,
                                   .layerCount = 1};
  VkImageMemoryBarrier acquire = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
      .dstQueueFamilyIndex = ctx->queue_family,
      .image = ctx->image,
      .subresourceRange = range};
  vkCmdPipelineBarrier(ctx->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &acquire);
  VkClearColorValue color = {
      .float32 = {CLEAR_R / 255.0f, CLEAR_G / 255.0f, CLEAR_B / 255.0f, 1.0f}};
  vkCmdClearColorImage(ctx->command_buffer, ctx->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
  VkImageMemoryBarrier release = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = 0,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = ctx->queue_family,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
      .image = ctx->image,
      .subresourceRange = range};
  vkCmdPipelineBarrier(ctx->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                       NULL, 1, &release);
  if ((result = vkEndCommandBuffer(ctx->command_buffer)) != VK_SUCCESS)
    return fail_vk("vkEndCommandBuffer", result);
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if ((result = vkCreateFence(ctx->device, &fci, NULL, &ctx->fence)) !=
      VK_SUCCESS)
    return fail_vk("vkCreateFence", result);
  VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                         .commandBufferCount = 1,
                         .pCommandBuffers = &ctx->command_buffer};
  if ((result = vkQueueSubmit(ctx->queue, 1, &submit, ctx->fence)) !=
      VK_SUCCESS)
    return fail_vk("vkQueueSubmit", result);
  result = vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE,
                           10ULL * 1000 * 1000 * 1000);
  return result == VK_SUCCESS ? 0 : fail_vk("vkWaitForFences", result);
}

static int verify_pixels(struct context *ctx) {
  puts("[VULKAN-S2] verify Vulkan writes through shared pages");
  const uint8_t expected[4] = {CLEAR_B, CLEAR_G, CLEAR_R, CLEAR_A};
  const uint8_t *bytes = ctx->map;
  for (uint32_t y = 0; y < ctx->dumb.height; y++) {
    const uint8_t *row = bytes + (size_t)y * ctx->dumb.pitch;
    for (uint32_t x = 0; x < ctx->dumb.width; x++) {
      if (memcmp(row + (size_t)x * 4, expected, sizeof(expected))) {
        fprintf(stderr, "[VULKAN-S2] pixel mismatch at %u,%u: %u,%u,%u,%u\n", x,
                y, row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]);
        return -1;
      }
    }
  }
  return 0;
}

static int add_fb(int fd, const struct drm_mode_create_dumb *dumb,
                  uint32_t *fb_id) {
  struct drm_mode_fb_cmd2 fb = {.width = dumb->width,
                                .height = dumb->height,
                                .pixel_format = DRM_FORMAT_XRGB8888};
  fb.handles[0] = dumb->handle;
  fb.pitches[0] = dumb->pitch;
  if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0)
    return fail_errno("DRM_IOCTL_MODE_ADDFB2");
  *fb_id = fb.fb_id;
  return 0;
}

static int present(struct context *ctx) {
  puts("[VULKAN-S2][08] add framebuffer and page flip");
  if (add_fb(ctx->drm_fd, &ctx->dumb, &ctx->fb_id))
    return -1;
  struct drm_mode_crtc current = {.crtc_id = ctx->crtc_id};
  if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_GETCRTC, &current) < 0)
    return fail_errno("DRM_IOCTL_MODE_GETCRTC");
  if (!current.mode_valid || !current.fb_id) {
    ctx->bootstrap.width = ctx->dumb.width;
    ctx->bootstrap.height = ctx->dumb.height;
    ctx->bootstrap.bpp = 32;
    if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &ctx->bootstrap) < 0 ||
        add_fb(ctx->drm_fd, &ctx->bootstrap, &ctx->bootstrap_fb_id))
      return -1;
    struct drm_mode_crtc set = {.crtc_id = ctx->crtc_id,
                                .fb_id = ctx->bootstrap_fb_id,
                                .set_connectors_ptr =
                                    (uintptr_t)&ctx->connector_id,
                                .count_connectors = 1,
                                .mode_valid = 1,
                                .mode = ctx->mode};
    if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0)
      return fail_errno("DRM_IOCTL_MODE_SETCRTC");
  }
  const uint64_t cookie = 0x56324b4d53534d4bULL;
  struct drm_mode_crtc_page_flip flip = {.crtc_id = ctx->crtc_id,
                                         .fb_id = ctx->fb_id,
                                         .flags = DRM_MODE_PAGE_FLIP_EVENT,
                                         .user_data = cookie};
  if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0)
    return fail_errno("DRM_IOCTL_MODE_PAGE_FLIP");
  struct pollfd pfd = {.fd = ctx->drm_fd, .events = POLLIN};
  if (poll(&pfd, 1, 1000) != 1)
    return fail_errno("page-flip poll");
  struct drm_event_vblank event;
  ssize_t n = read(ctx->drm_fd, &event, sizeof(event));
  if (n != sizeof(event) || event.base.type != DRM_EVENT_FLIP_COMPLETE ||
      event.base.length != sizeof(event) || event.user_data != cookie)
    return -1;
  puts("[VULKAN-S2][09] shared pixels and page-flip event verified");
  sleep(1);
  return 0;
}

static void cleanup(struct context *ctx) {
  if (ctx->device)
    vkDeviceWaitIdle(ctx->device);
  if (ctx->fence)
    vkDestroyFence(ctx->device, ctx->fence, NULL);
  if (ctx->command_pool)
    vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
  if (ctx->image)
    vkDestroyImage(ctx->device, ctx->image, NULL);
  if (ctx->memory)
    vkFreeMemory(ctx->device, ctx->memory, NULL);
  if (ctx->device)
    vkDestroyDevice(ctx->device, NULL);
  if (ctx->instance)
    vkDestroyInstance(ctx->instance, NULL);
  if (ctx->map)
    munmap(ctx->map, ctx->dumb.size);
  if (ctx->map_fd >= 0)
    close(ctx->map_fd);
  if (ctx->prime_fd >= 0)
    close(ctx->prime_fd);
#ifdef UDMABUF_VULKAN_KMS
  if (ctx->memfd >= 0)
    close(ctx->memfd);
  if (ctx->udmabuf_fd >= 0)
    close(ctx->udmabuf_fd);
#endif
  if (ctx->drm_fd >= 0) {
    if (ctx->bootstrap_fb_id)
      ioctl(ctx->drm_fd, DRM_IOCTL_MODE_RMFB, &ctx->bootstrap_fb_id);
    if (ctx->bootstrap.handle)
      ioctl(ctx->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &ctx->bootstrap);
    if (ctx->fb_id)
      ioctl(ctx->drm_fd, DRM_IOCTL_MODE_RMFB, &ctx->fb_id);
    if (ctx->dumb.handle)
      ioctl(ctx->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &ctx->dumb);
    close(ctx->drm_fd);
  }
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  struct context ctx = {
      .drm_fd = -1,
      .prime_fd = -1,
      .map_fd = -1,
#ifdef UDMABUF_VULKAN_KMS
      .memfd = -1,
      .udmabuf_fd = -1,
#endif
  };
  int status = 1;
  if (loader_preflight() || select_mode(&ctx) || create_dumb_and_prime(&ctx) ||
      prime_preflight(&ctx) || init_vulkan(&ctx) || import_image(&ctx) ||
      clear_image(&ctx) || verify_pixels(&ctx) || present(&ctx))
    goto out;
  status = 0;
out:
  cleanup(&ctx);
  if (!status)
#ifdef UDMABUF_VULKAN_KMS
    puts("[PASS] udmabuf_vulkan_kms_smoke");
#else
    puts("[PASS] vulkan_drm_smoke");
#endif
  return status;
}
