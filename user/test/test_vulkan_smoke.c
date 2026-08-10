/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define WIDTH 64u
#define HEIGHT 64u
#define PIXEL_BYTES 4u
#define IMAGE_BYTES ((VkDeviceSize)WIDTH * HEIGHT * PIXEL_BYTES)
#define MANIFEST "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
#define ICD_PATH "/lib/libvulkan_lvp.so"

struct smoke_context {
  VkInstance instance;
  VkPhysicalDevice physical;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family;
  VkImage image;
  VkDeviceMemory image_memory;
  VkBuffer staging;
  VkDeviceMemory staging_memory;
  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
  VkFence fence;
  VkDeviceSize staging_allocation_size;
  int staging_coherent;
  VkPhysicalDeviceProperties properties;
};

static int vk_failed(const char *api, VkResult result) {
  if (result == VK_SUCCESS)
    return 0;
  fprintf(stderr, "[VULKAN-S1] %s failed: VkResult=%d\n", api, (int)result);
  return 1;
}

static int loader_preflight(void) {
  puts("[VULKAN-S1][01] loader preflight");
  if (setenv("VK_DRIVER_FILES", MANIFEST, 1) ||
      setenv("VK_ICD_FILENAMES", MANIFEST, 1) ||
      setenv("VK_LOADER_DEBUG", "error,warn,driver", 0)) {
    perror("setenv");
    return -1;
  }
  void *icd = dlopen(ICD_PATH, RTLD_NOW | RTLD_LOCAL);
  if (!icd) {
    fprintf(stderr, "[VULKAN-S1][01] dlopen(%s): %s\n", ICD_PATH, dlerror());
    return -1;
  }
  void *get_proc = dlsym(icd, "vk_icdGetInstanceProcAddr");
  void *negotiate = dlsym(icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
  if (!get_proc || !negotiate) {
    fprintf(stderr, "[VULKAN-S1][01] ICD entry points missing: %s\n",
            dlerror());
    dlclose(icd);
    return -1;
  }
  if (dlclose(icd)) {
    fprintf(stderr, "[VULKAN-S1][01] dlclose: %s\n", dlerror());
    return -1;
  }
  return 0;
}

static VkResult enumerate_devices(VkInstance instance, uint32_t *count,
                                  VkPhysicalDevice **devices) {
  for (unsigned retry = 0; retry < 4; ++retry) {
    uint32_t n = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance, &n, NULL);
    if (result != VK_SUCCESS)
      return result;
    if (!n || n > 64)
      return VK_ERROR_INITIALIZATION_FAILED;
    VkPhysicalDevice *list = calloc(n, sizeof(*list));
    if (!list)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t filled = n;
    result = vkEnumeratePhysicalDevices(instance, &filled, list);
    if (result == VK_SUCCESS) {
      *count = filled;
      *devices = list;
      return VK_SUCCESS;
    }
    free(list);
    if (result != VK_INCOMPLETE)
      return result;
  }
  return VK_INCOMPLETE;
}

static VkResult select_lavapipe(struct smoke_context *ctx) {
  uint32_t count = 0;
  VkPhysicalDevice *devices = NULL;
  VkResult result = enumerate_devices(ctx->instance, &count, &devices);
  if (result != VK_SUCCESS)
    return result;

  result = VK_ERROR_INITIALIZATION_FAILED;
  for (uint32_t i = 0; i < count; ++i) {
    VkPhysicalDeviceDriverProperties driver = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &driver,
    };
    vkGetPhysicalDeviceProperties2(devices[i], &props);
    printf("[VULKAN-S1][03] device=%s driver_id=%u driver=%s\n",
           props.properties.deviceName, (unsigned)driver.driverID,
           driver.driverName);
    if (driver.driverID != VK_DRIVER_ID_MESA_LLVMPIPE)
      continue;

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, NULL);
    if (!queue_count || queue_count > 256)
      continue;
    VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
    if (!queues) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      break;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, queues);
    for (uint32_t q = 0; q < queue_count; ++q) {
      VkQueueFlags flags = queues[q].queueFlags;
      if (queues[q].queueCount && (flags & VK_QUEUE_TRANSFER_BIT) &&
          (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
        ctx->physical = devices[i];
        ctx->queue_family = q;
        ctx->properties = props.properties;
        result = VK_SUCCESS;
        break;
      }
    }
    free(queues);
    if (result == VK_SUCCESS)
      break;
  }
  free(devices);
  return result;
}

static int find_memory_type(VkPhysicalDevice physical, uint32_t bits,
                            VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred, uint32_t *index,
                            int *coherent) {
  VkPhysicalDeviceMemoryProperties memory;
  vkGetPhysicalDeviceMemoryProperties(physical, &memory);
  int fallback = -1;
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
    if (!(bits & (1u << i)) || (flags & required) != required)
      continue;
    if ((flags & preferred) == preferred) {
      *index = i;
      if (coherent)
        *coherent = !!(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      return 0;
    }
    if (fallback < 0)
      fallback = (int)i;
  }
  if (fallback >= 0) {
    *index = (uint32_t)fallback;
    if (coherent) {
      VkMemoryPropertyFlags flags = memory.memoryTypes[*index].propertyFlags;
      *coherent = !!(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    return 0;
  }
  fprintf(stderr, "[VULKAN-S1][05] no memory type: bits=0x%x required=0x%x\n",
          bits, required);
  return -1;
}

static VkResult create_resources(struct smoke_context *ctx) {
  VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {WIDTH, HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage =
          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  VkResult result = vkCreateImage(ctx->device, &image_info, NULL, &ctx->image);
  if (result != VK_SUCCESS)
    return result;

  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(ctx->device, ctx->image, &req);
  uint32_t memory_type;
  if (find_memory_type(ctx->physical, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &memory_type,
                       NULL) &&
      find_memory_type(ctx->physical, req.memoryTypeBits, 0, 0, &memory_type,
                       NULL))
    return VK_ERROR_FEATURE_NOT_PRESENT;
  VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = memory_type,
  };
  result = vkAllocateMemory(ctx->device, &allocation, NULL, &ctx->image_memory);
  if (result != VK_SUCCESS)
    return result;
  result = vkBindImageMemory(ctx->device, ctx->image, ctx->image_memory, 0);
  if (result != VK_SUCCESS)
    return result;

  VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = IMAGE_BYTES,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  result = vkCreateBuffer(ctx->device, &buffer_info, NULL, &ctx->staging);
  if (result != VK_SUCCESS)
    return result;
  vkGetBufferMemoryRequirements(ctx->device, ctx->staging, &req);
  if (req.size < IMAGE_BYTES)
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  if (find_memory_type(ctx->physical, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &memory_type,
                       &ctx->staging_coherent))
    return VK_ERROR_FEATURE_NOT_PRESENT;
  allocation.allocationSize = req.size;
  allocation.memoryTypeIndex = memory_type;
  ctx->staging_allocation_size = req.size;
  result =
      vkAllocateMemory(ctx->device, &allocation, NULL, &ctx->staging_memory);
  if (result != VK_SUCCESS)
    return result;
  result =
      vkBindBufferMemory(ctx->device, ctx->staging, ctx->staging_memory, 0);
  if (result != VK_SUCCESS)
    return result;

  VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = ctx->queue_family,
  };
  result =
      vkCreateCommandPool(ctx->device, &pool_info, NULL, &ctx->command_pool);
  if (result != VK_SUCCESS)
    return result;
  VkCommandBufferAllocateInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = ctx->command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  result = vkAllocateCommandBuffers(ctx->device, &command_info,
                                    &ctx->command_buffer);
  if (result != VK_SUCCESS)
    return result;
  VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  return vkCreateFence(ctx->device, &fence_info, NULL, &ctx->fence);
}

static VkResult record_and_submit(struct smoke_context *ctx) {
  VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  VkResult result = vkBeginCommandBuffer(ctx->command_buffer, &begin);
  if (result != VK_SUCCESS)
    return result;
  VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = ctx->image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  vkCmdPipelineBarrier(ctx->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &barrier);
  VkClearColorValue color = {.float32 = {0.25f, 0.50f, 0.75f, 1.0f}};
  vkCmdClearColorImage(ctx->command_buffer, ctx->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                       &barrier.subresourceRange);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier(ctx->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &barrier);
  VkBufferImageCopy copy = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {WIDTH, HEIGHT, 1},
  };
  vkCmdCopyImageToBuffer(ctx->command_buffer, ctx->image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ctx->staging, 1,
                         &copy);
  result = vkEndCommandBuffer(ctx->command_buffer);
  if (result != VK_SUCCESS)
    return result;
  VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &ctx->command_buffer,
  };
  puts("[VULKAN-S1][06] submit and wait");
  result = vkQueueSubmit(ctx->queue, 1, &submit, ctx->fence);
  if (result != VK_SUCCESS)
    return result;
  return vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, 10000000000ull);
}

static int verify_pixels(struct smoke_context *ctx) {
  void *mapped = NULL;
  VkResult result = vkMapMemory(ctx->device, ctx->staging_memory, 0,
                                ctx->staging_allocation_size, 0, &mapped);
  if (vk_failed("vkMapMemory", result))
    return -1;
  if (!ctx->staging_coherent) {
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = ctx->staging_memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    result = vkInvalidateMappedMemoryRanges(ctx->device, 1, &range);
    if (vk_failed("vkInvalidateMappedMemoryRanges", result)) {
      vkUnmapMemory(ctx->device, ctx->staging_memory);
      return -1;
    }
  }
  const uint8_t expected[4] = {64, 128, 191, 255};
  const uint8_t *pixels = mapped;
  puts("[VULKAN-S1][07] verify pixels");
  for (uint32_t y = 0; y < HEIGHT; ++y) {
    for (uint32_t x = 0; x < WIDTH; ++x) {
      const uint8_t *p = pixels + ((size_t)y * WIDTH + x) * PIXEL_BYTES;
      for (unsigned c = 0; c < 4; ++c) {
        int delta = (int)p[c] - expected[c];
        if (delta < -1 || delta > 1) {
          fprintf(stderr,
                  "[VULKAN-S1][07] pixel (%u,%u) expected="
                  "%u,%u,%u,%u actual=%u,%u,%u,%u\n",
                  x, y, expected[0], expected[1], expected[2], expected[3],
                  p[0], p[1], p[2], p[3]);
          vkUnmapMemory(ctx->device, ctx->staging_memory);
          return -1;
        }
      }
    }
  }
  vkUnmapMemory(ctx->device, ctx->staging_memory);
  return 0;
}

static void destroy_context(struct smoke_context *ctx) {
  if (ctx->device) {
    vkDeviceWaitIdle(ctx->device);
    if (ctx->fence)
      vkDestroyFence(ctx->device, ctx->fence, NULL);
    if (ctx->command_pool)
      vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
    if (ctx->staging)
      vkDestroyBuffer(ctx->device, ctx->staging, NULL);
    if (ctx->staging_memory)
      vkFreeMemory(ctx->device, ctx->staging_memory, NULL);
    if (ctx->image)
      vkDestroyImage(ctx->device, ctx->image, NULL);
    if (ctx->image_memory)
      vkFreeMemory(ctx->device, ctx->image_memory, NULL);
    vkDestroyDevice(ctx->device, NULL);
  }
  if (ctx->instance)
    vkDestroyInstance(ctx->instance, NULL);
}

int main(void) {
  struct smoke_context ctx = {0};
  int status = 1;
  if (loader_preflight())
    return 1;

  uint32_t api_version = VK_API_VERSION_1_0;
  if (vk_failed("vkEnumerateInstanceVersion",
                vkEnumerateInstanceVersion(&api_version)) ||
      api_version < VK_API_VERSION_1_2) {
    fprintf(
        stderr, "[VULKAN-S1][02] Vulkan 1.2 loader required, got %u.%u.%u\n",
        VK_API_VERSION_MAJOR(api_version), VK_API_VERSION_MINOR(api_version),
        VK_API_VERSION_PATCH(api_version));
    return 1;
  }
  puts("[VULKAN-S1][02] create instance");
  VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vulkan_smoke",
      .apiVersion = VK_API_VERSION_1_2,
  };
  VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
  };
  if (vk_failed("vkCreateInstance",
                vkCreateInstance(&instance_info, NULL, &ctx.instance)))
    goto out;
  puts("[VULKAN-S1][03] select physical device");
  if (vk_failed("select_lavapipe", select_lavapipe(&ctx)))
    goto out;

  puts("[VULKAN-S1][04] create device/queue");
  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = ctx.queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
  };
  VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
  };
  if (vk_failed("vkCreateDevice",
                vkCreateDevice(ctx.physical, &device_info, NULL, &ctx.device)))
    goto out;
  vkGetDeviceQueue(ctx.device, ctx.queue_family, 0, &ctx.queue);

  puts("[VULKAN-S1][05] allocate resources");
  if (vk_failed("create_offscreen_resources", create_resources(&ctx)))
    goto out;
  VkResult result = record_and_submit(&ctx);
  if (result == VK_TIMEOUT)
    fprintf(stderr, "[VULKAN-S1][06] fence timeout after 10 seconds\n");
  if (vk_failed("submit_clear_and_readback", result) || verify_pixels(&ctx))
    goto out;
  status = 0;
out:
  destroy_context(&ctx);
  if (!status)
    puts("[PASS] vulkan_smoke");
  return status;
}
