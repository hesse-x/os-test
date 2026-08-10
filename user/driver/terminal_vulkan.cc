/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

#include <wayland-client.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "terminal_rect.frag.spv.h"
#include "terminal_rect.vert.spv.h"
#include "terminal_text.frag.spv.h"
#include "terminal_text.vert.spv.h"
#include "terminal_vulkan.h"

namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr uint64_t kFenceTimeoutNs = 5ull * 1000 * 1000 * 1000;
constexpr uint64_t kAcquireTimeoutNs = 16ull * 1000 * 1000;
constexpr uint32_t kEnumerationLimit = 256;

struct Vertex {
  float x, y;
  float u, v;
  float color[4];
};

struct FrameSlot {
  VkSemaphore acquired;
  VkFence fence;
  VkCommandBuffer command;
  VkBuffer vertex_buffer;
  VkDeviceMemory vertex_memory;
  void *vertices;
  VkDeviceSize capacity;
  bool coherent;
  bool submitted;
};

struct SwapchainState {
  VkSwapchainKHR handle;
  VkFormat format;
  VkColorSpaceKHR color_space;
  VkExtent2D extent;
  uint32_t image_count;
  VkImage *images;
  VkImageView *views;
  VkFramebuffer *framebuffers;
  VkSemaphore *render_finished;
  int *images_in_flight;
};

} // namespace

struct TerminalVulkanRenderer {
  wl_display *display;
  wl_surface *wayland_surface;
  wl_callback *frame_callback;
  TerminalVkFrameReady frame_ready;
  void *frame_data;

  VkInstance instance;
  VkSurfaceKHR surface;
  VkPhysicalDevice physical_device;
  VkDevice device;
  uint32_t queue_family;
  VkQueue queue;
  VkPhysicalDeviceMemoryProperties memory_properties;
  VkPhysicalDeviceProperties properties;

  VkCommandPool command_pool;
  FrameSlot frames[kFramesInFlight];
  uint32_t frame_index;

  SwapchainState swap;
  VkRenderPass render_pass;
  VkPipelineLayout rect_layout;
  VkPipelineLayout text_layout;
  VkPipeline rect_pipeline;
  VkPipeline text_pipeline;

  VkDescriptorSetLayout descriptor_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set;
  VkImage atlas;
  VkDeviceMemory atlas_memory;
  VkImageView atlas_view;
  VkSampler atlas_sampler;
  VkFormat atlas_format;
  uint32_t atlas_width;
  uint32_t atlas_height;
  uint64_t atlas_generation;
  bool atlas_ready;

  uint32_t logical_width;
  uint32_t logical_height;
  uint32_t buffer_scale;
  uint64_t swapchain_generation;
  bool recreate;
  bool fatal;
  bool opaque;
  bool rendering;
};

namespace {

static const char *vk_result_name(VkResult result) {
  switch (result) {
  case VK_SUCCESS:
    return "VK_SUCCESS";
  case VK_NOT_READY:
    return "VK_NOT_READY";
  case VK_TIMEOUT:
    return "VK_TIMEOUT";
  case VK_SUBOPTIMAL_KHR:
    return "VK_SUBOPTIMAL_KHR";
  case VK_ERROR_OUT_OF_HOST_MEMORY:
    return "VK_ERROR_OUT_OF_HOST_MEMORY";
  case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
  case VK_ERROR_INITIALIZATION_FAILED:
    return "VK_ERROR_INITIALIZATION_FAILED";
  case VK_ERROR_DEVICE_LOST:
    return "VK_ERROR_DEVICE_LOST";
  case VK_ERROR_SURFACE_LOST_KHR:
    return "VK_ERROR_SURFACE_LOST_KHR";
  case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
    return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
  case VK_ERROR_OUT_OF_DATE_KHR:
    return "VK_ERROR_OUT_OF_DATE_KHR";
  default:
    return "VK_RESULT_UNKNOWN";
  }
}

static bool fail(const char *stage, VkResult result) {
  fprintf(stderr, "[TERM-VK] FATAL stage=%s result=%s(%d)\n", stage,
          vk_result_name(result), result);
  return false;
}

static bool checked_mul(size_t a, size_t b, size_t *out) {
  if (a != 0 && b > SIZE_MAX / a)
    return false;
  *out = a * b;
  return true;
}

static bool has_extension(const VkExtensionProperties *extensions,
                          uint32_t count, const char *name) {
  for (uint32_t i = 0; i < count; ++i)
    if (strcmp(extensions[i].extensionName, name) == 0)
      return true;
  return false;
}

static bool enumerate_instance_extensions(VkExtensionProperties **out,
                                          uint32_t *out_count) {
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    uint32_t count = 0;
    VkResult result =
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    if (result != VK_SUCCESS || count == 0 || count > kEnumerationLimit)
      return fail("enumerate-instance-extension-count", result);
    auto *items = static_cast<VkExtensionProperties *>(
        calloc(count, sizeof(VkExtensionProperties)));
    if (!items)
      return fail("allocate-instance-extensions", VK_ERROR_OUT_OF_HOST_MEMORY);
    uint32_t actual = count;
    result = vkEnumerateInstanceExtensionProperties(nullptr, &actual, items);
    if (result == VK_SUCCESS) {
      *out = items;
      *out_count = actual;
      return true;
    }
    free(items);
    if (result != VK_INCOMPLETE)
      return fail("enumerate-instance-extensions", result);
  }
  return fail("enumerate-instance-extensions-retry", VK_INCOMPLETE);
}

static bool create_instance(TerminalVulkanRenderer *renderer) {
  VkExtensionProperties *available = nullptr;
  uint32_t count = 0;
  if (!enumerate_instance_extensions(&available, &count))
    return false;
  const char *required[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                            VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
  for (const char *name : required) {
    if (!has_extension(available, count, name)) {
      fprintf(stderr, "[TERM-VK] FATAL missing instance extension %s\n", name);
      free(available);
      return false;
    }
  }
  free(available);

  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "terminal";
  app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.pEngineName = "terminal";
  app_info.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &app_info;
  create_info.enabledExtensionCount = 2;
  create_info.ppEnabledExtensionNames = required;
  VkResult result =
      vkCreateInstance(&create_info, nullptr, &renderer->instance);
  return result == VK_SUCCESS || fail("create-instance", result);
}

static bool create_surface(TerminalVulkanRenderer *renderer) {
  VkWaylandSurfaceCreateInfoKHR create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
  create_info.display = renderer->display;
  create_info.surface = renderer->wayland_surface;
  VkResult result = vkCreateWaylandSurfaceKHR(renderer->instance, &create_info,
                                              nullptr, &renderer->surface);
  return result == VK_SUCCESS || fail("create-wayland-surface", result);
}

static bool device_has_swapchain(VkPhysicalDevice device) {
  uint32_t count = 0;
  VkResult result =
      vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
  if (result != VK_SUCCESS || count == 0 || count > kEnumerationLimit)
    return false;
  auto *extensions = static_cast<VkExtensionProperties *>(
      calloc(count, sizeof(VkExtensionProperties)));
  if (!extensions)
    return false;
  result =
      vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions);
  bool found =
      result == VK_SUCCESS &&
      has_extension(extensions, count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  free(extensions);
  return found;
}

static bool select_device(TerminalVulkanRenderer *renderer) {
  uint32_t count = 0;
  VkResult result =
      vkEnumeratePhysicalDevices(renderer->instance, &count, nullptr);
  if (result != VK_SUCCESS || count == 0 || count > kEnumerationLimit)
    return fail("enumerate-physical-device-count", result);
  auto *devices =
      static_cast<VkPhysicalDevice *>(calloc(count, sizeof(VkPhysicalDevice)));
  if (!devices)
    return fail("allocate-physical-devices", VK_ERROR_OUT_OF_HOST_MEMORY);
  result = vkEnumeratePhysicalDevices(renderer->instance, &count, devices);
  if (result != VK_SUCCESS) {
    free(devices);
    return fail("enumerate-physical-devices", result);
  }

  for (uint32_t d = 0; d < count && !renderer->physical_device; ++d) {
    if (!device_has_swapchain(devices[d]))
      continue;
    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &queue_count, nullptr);
    if (queue_count == 0 || queue_count > kEnumerationLimit)
      continue;
    auto *queues = static_cast<VkQueueFamilyProperties *>(
        calloc(queue_count, sizeof(VkQueueFamilyProperties)));
    if (!queues)
      continue;
    vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &queue_count, queues);
    for (uint32_t q = 0; q < queue_count; ++q) {
      VkBool32 present = VK_FALSE;
      result = vkGetPhysicalDeviceSurfaceSupportKHR(
          devices[d], q, renderer->surface, &present);
      fprintf(stderr,
              "[TERM-VK] queue device=%u family=%u flags=0x%x present=%u\n", d,
              q, queues[q].queueFlags,
              result == VK_SUCCESS ? present : VK_FALSE);
      if (result == VK_SUCCESS && present && queues[q].queueCount > 0 &&
          (queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        renderer->physical_device = devices[d];
        renderer->queue_family = q;
        break;
      }
    }
    free(queues);
  }
  free(devices);
  if (!renderer->physical_device)
    return fail("select-graphics-present-queue",
                VK_ERROR_INITIALIZATION_FAILED);
  vkGetPhysicalDeviceProperties(renderer->physical_device,
                                &renderer->properties);
  vkGetPhysicalDeviceMemoryProperties(renderer->physical_device,
                                      &renderer->memory_properties);
  fprintf(stderr, "[TERM-VK] device=%s api=%u.%u.%u queue_family=%u\n",
          renderer->properties.deviceName,
          VK_VERSION_MAJOR(renderer->properties.apiVersion),
          VK_VERSION_MINOR(renderer->properties.apiVersion),
          VK_VERSION_PATCH(renderer->properties.apiVersion),
          renderer->queue_family);
  return true;
}

static bool create_device(TerminalVulkanRenderer *renderer) {
  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = renderer->queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  create_info.queueCreateInfoCount = 1;
  create_info.pQueueCreateInfos = &queue_info;
  create_info.enabledExtensionCount = 1;
  create_info.ppEnabledExtensionNames = extensions;
  VkResult result = vkCreateDevice(renderer->physical_device, &create_info,
                                   nullptr, &renderer->device);
  if (result != VK_SUCCESS)
    return fail("create-device", result);
  vkGetDeviceQueue(renderer->device, renderer->queue_family, 0,
                   &renderer->queue);
  return true;
}

static uint32_t find_memory_type(TerminalVulkanRenderer *renderer,
                                 uint32_t type_bits,
                                 VkMemoryPropertyFlags required,
                                 VkMemoryPropertyFlags preferred,
                                 bool *coherent = nullptr) {
  uint32_t fallback = UINT32_MAX;
  for (uint32_t i = 0; i < renderer->memory_properties.memoryTypeCount; ++i) {
    if (!(type_bits & (1u << i)))
      continue;
    VkMemoryPropertyFlags flags =
        renderer->memory_properties.memoryTypes[i].propertyFlags;
    if ((flags & required) != required)
      continue;
    if (fallback == UINT32_MAX)
      fallback = i;
    if ((flags & preferred) == preferred) {
      if (coherent)
        *coherent = flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      return i;
    }
  }
  if (fallback != UINT32_MAX && coherent)
    *coherent =
        renderer->memory_properties.memoryTypes[fallback].propertyFlags &
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  return fallback;
}

static bool create_command_resources(TerminalVulkanRenderer *renderer) {
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex = renderer->queue_family;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkResult result = vkCreateCommandPool(renderer->device, &pool_info, nullptr,
                                        &renderer->command_pool);
  if (result != VK_SUCCESS)
    return fail("create-command-pool", result);
  VkCommandBuffer commands[kFramesInFlight]{};
  VkCommandBufferAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate.commandPool = renderer->command_pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = kFramesInFlight;
  result = vkAllocateCommandBuffers(renderer->device, &allocate, commands);
  if (result != VK_SUCCESS)
    return fail("allocate-command-buffers", result);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    renderer->frames[i].command = commands[i];
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    result = vkCreateSemaphore(renderer->device, &semaphore_info, nullptr,
                               &renderer->frames[i].acquired);
    if (result != VK_SUCCESS)
      return fail("create-acquire-semaphore", result);
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    result = vkCreateFence(renderer->device, &fence_info, nullptr,
                           &renderer->frames[i].fence);
    if (result != VK_SUCCESS)
      return fail("create-submit-fence", result);
  }
  return true;
}

static void destroy_vertex_buffer(TerminalVulkanRenderer *renderer,
                                  FrameSlot *slot) {
  if (slot->vertices)
    vkUnmapMemory(renderer->device, slot->vertex_memory);
  if (slot->vertex_buffer)
    vkDestroyBuffer(renderer->device, slot->vertex_buffer, nullptr);
  if (slot->vertex_memory)
    vkFreeMemory(renderer->device, slot->vertex_memory, nullptr);
  slot->vertices = nullptr;
  slot->vertex_buffer = VK_NULL_HANDLE;
  slot->vertex_memory = VK_NULL_HANDLE;
  slot->capacity = 0;
}

static bool ensure_vertex_buffer(TerminalVulkanRenderer *renderer,
                                 FrameSlot *slot, VkDeviceSize required) {
  if (required <= slot->capacity)
    return true;
  destroy_vertex_buffer(renderer, slot);
  VkDeviceSize capacity = 64 * 1024;
  while (capacity < required) {
    if (capacity > UINT64_MAX / 2)
      return fail("vertex-capacity-overflow", VK_ERROR_OUT_OF_HOST_MEMORY);
    capacity *= 2;
  }
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = capacity;
  buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult result = vkCreateBuffer(renderer->device, &buffer_info, nullptr,
                                   &slot->vertex_buffer);
  if (result != VK_SUCCESS)
    return fail("create-vertex-buffer", result);
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(renderer->device, slot->vertex_buffer,
                                &requirements);
  uint32_t type =
      find_memory_type(renderer, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &slot->coherent);
  if (type == UINT32_MAX)
    return fail("find-vertex-memory", VK_ERROR_FEATURE_NOT_PRESENT);
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  result = vkAllocateMemory(renderer->device, &allocate, nullptr,
                            &slot->vertex_memory);
  if (result != VK_SUCCESS)
    return fail("allocate-vertex-memory", result);
  result = vkBindBufferMemory(renderer->device, slot->vertex_buffer,
                              slot->vertex_memory, 0);
  if (result != VK_SUCCESS)
    return fail("bind-vertex-memory", result);
  result = vkMapMemory(renderer->device, slot->vertex_memory, 0, VK_WHOLE_SIZE,
                       0, &slot->vertices);
  if (result != VK_SUCCESS)
    return fail("map-vertex-memory", result);
  slot->capacity = capacity;
  return true;
}

static void destroy_swapchain(TerminalVulkanRenderer *renderer) {
  SwapchainState *swap = &renderer->swap;
  for (uint32_t i = 0; i < swap->image_count; ++i) {
    if (swap->render_finished && swap->render_finished[i])
      vkDestroySemaphore(renderer->device, swap->render_finished[i], nullptr);
    if (swap->framebuffers && swap->framebuffers[i])
      vkDestroyFramebuffer(renderer->device, swap->framebuffers[i], nullptr);
    if (swap->views && swap->views[i])
      vkDestroyImageView(renderer->device, swap->views[i], nullptr);
  }
  if (swap->handle)
    vkDestroySwapchainKHR(renderer->device, swap->handle, nullptr);
  free(swap->images);
  free(swap->views);
  free(swap->framebuffers);
  free(swap->render_finished);
  free(swap->images_in_flight);
  *swap = {};
}

static void destroy_pipelines(TerminalVulkanRenderer *renderer) {
  if (renderer->rect_pipeline)
    vkDestroyPipeline(renderer->device, renderer->rect_pipeline, nullptr);
  if (renderer->text_pipeline)
    vkDestroyPipeline(renderer->device, renderer->text_pipeline, nullptr);
  if (renderer->rect_layout)
    vkDestroyPipelineLayout(renderer->device, renderer->rect_layout, nullptr);
  if (renderer->text_layout)
    vkDestroyPipelineLayout(renderer->device, renderer->text_layout, nullptr);
  if (renderer->render_pass)
    vkDestroyRenderPass(renderer->device, renderer->render_pass, nullptr);
  renderer->rect_pipeline = VK_NULL_HANDLE;
  renderer->text_pipeline = VK_NULL_HANDLE;
  renderer->rect_layout = VK_NULL_HANDLE;
  renderer->text_layout = VK_NULL_HANDLE;
  renderer->render_pass = VK_NULL_HANDLE;
}

static bool choose_surface(TerminalVulkanRenderer *renderer,
                           VkSurfaceCapabilitiesKHR *caps,
                           VkSurfaceFormatKHR *chosen_format,
                           VkPresentModeKHR *chosen_mode,
                           VkCompositeAlphaFlagBitsKHR *alpha) {
  VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      renderer->physical_device, renderer->surface, caps);
  if (result != VK_SUCCESS)
    return fail("get-surface-capabilities", result);
  if (!(caps->supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    return fail("surface-color-attachment-unsupported",
                VK_ERROR_FORMAT_NOT_SUPPORTED);

  uint32_t format_count = 0;
  result = vkGetPhysicalDeviceSurfaceFormatsKHR(
      renderer->physical_device, renderer->surface, &format_count, nullptr);
  if (result != VK_SUCCESS || format_count == 0 ||
      format_count > kEnumerationLimit)
    return fail("get-surface-format-count", result);
  auto *formats = static_cast<VkSurfaceFormatKHR *>(
      calloc(format_count, sizeof(VkSurfaceFormatKHR)));
  if (!formats)
    return fail("allocate-surface-formats", VK_ERROR_OUT_OF_HOST_MEMORY);
  result = vkGetPhysicalDeviceSurfaceFormatsKHR(
      renderer->physical_device, renderer->surface, &format_count, formats);
  if (result != VK_SUCCESS) {
    free(formats);
    return fail("get-surface-formats", result);
  }
  bool found = false;
  if (format_count == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
    *chosen_format = {VK_FORMAT_B8G8R8A8_UNORM, formats[0].colorSpace};
    found = true;
  } else {
    const VkFormat preferred[] = {VK_FORMAT_B8G8R8A8_UNORM,
                                  VK_FORMAT_R8G8B8A8_UNORM};
    for (VkFormat wanted : preferred)
      for (uint32_t i = 0; i < format_count && !found; ++i)
        if (formats[i].format == wanted) {
          *chosen_format = formats[i];
          found = true;
        }
  }
  free(formats);
  if (!found)
    return fail("select-surface-format", VK_ERROR_FORMAT_NOT_SUPPORTED);

  uint32_t mode_count = 0;
  result = vkGetPhysicalDeviceSurfacePresentModesKHR(
      renderer->physical_device, renderer->surface, &mode_count, nullptr);
  if (result != VK_SUCCESS || mode_count == 0 || mode_count > kEnumerationLimit)
    return fail("get-present-mode-count", result);
  auto *modes = static_cast<VkPresentModeKHR *>(
      calloc(mode_count, sizeof(VkPresentModeKHR)));
  if (!modes)
    return fail("allocate-present-modes", VK_ERROR_OUT_OF_HOST_MEMORY);
  result = vkGetPhysicalDeviceSurfacePresentModesKHR(
      renderer->physical_device, renderer->surface, &mode_count, modes);
  found = false;
  if (result == VK_SUCCESS)
    for (uint32_t i = 0; i < mode_count; ++i)
      if (modes[i] == VK_PRESENT_MODE_FIFO_KHR)
        found = true;
  free(modes);
  if (!found)
    return fail("select-fifo-present-mode", VK_ERROR_INITIALIZATION_FAILED);
  *chosen_mode = VK_PRESENT_MODE_FIFO_KHR;

  if (caps->supportedCompositeAlpha &
      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
    *alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    renderer->opaque = false;
  } else if (caps->supportedCompositeAlpha &
             VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
    *alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    renderer->opaque = true;
    fprintf(stderr, "[TERM-VK] composite alpha fallback=OPAQUE\n");
  } else {
    return fail("select-composite-alpha", VK_ERROR_INITIALIZATION_FAILED);
  }
  return true;
}

static VkExtent2D choose_extent(TerminalVulkanRenderer *renderer,
                                const VkSurfaceCapabilitiesKHR &caps) {
  if (caps.currentExtent.width != UINT32_MAX)
    return caps.currentExtent;
  if (renderer->logical_width == 0 || renderer->logical_height == 0 ||
      renderer->buffer_scale == 0)
    return {0, 0};
  uint64_t width =
      static_cast<uint64_t>(renderer->logical_width) * renderer->buffer_scale;
  uint64_t height =
      static_cast<uint64_t>(renderer->logical_height) * renderer->buffer_scale;
  if (width > UINT32_MAX || height > UINT32_MAX)
    return {0, 0};
  VkExtent2D extent = {static_cast<uint32_t>(width),
                       static_cast<uint32_t>(height)};
  if (extent.width < caps.minImageExtent.width)
    extent.width = caps.minImageExtent.width;
  if (extent.width > caps.maxImageExtent.width)
    extent.width = caps.maxImageExtent.width;
  if (extent.height < caps.minImageExtent.height)
    extent.height = caps.minImageExtent.height;
  if (extent.height > caps.maxImageExtent.height)
    extent.height = caps.maxImageExtent.height;
  return extent;
}

static bool create_render_pass(TerminalVulkanRenderer *renderer,
                               VkFormat format) {
  VkAttachmentDescription color{};
  color.format = format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = 1;
  info.pAttachments = &color;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  info.dependencyCount = 1;
  info.pDependencies = &dependency;
  VkResult result = vkCreateRenderPass(renderer->device, &info, nullptr,
                                       &renderer->render_pass);
  return result == VK_SUCCESS || fail("create-render-pass", result);
}

static VkShaderModule create_shader(TerminalVulkanRenderer *renderer,
                                    const uint32_t *words, size_t word_count,
                                    const char *stage) {
  if (!words || word_count == 0 || words[0] != 0x07230203) {
    fail(stage, VK_ERROR_INITIALIZATION_FAILED);
    return VK_NULL_HANDLE;
  }
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = word_count * sizeof(uint32_t);
  info.pCode = words;
  VkShaderModule module = VK_NULL_HANDLE;
  VkResult result =
      vkCreateShaderModule(renderer->device, &info, nullptr, &module);
  if (result != VK_SUCCESS)
    fail(stage, result);
  return module;
}

static bool create_one_pipeline(TerminalVulkanRenderer *renderer, bool text,
                                const uint32_t *vert, size_t vert_words,
                                const uint32_t *frag, size_t frag_words,
                                VkPipelineLayout *out_layout,
                                VkPipeline *out_pipeline) {
  VkShaderModule vs = create_shader(renderer, vert, vert_words,
                                    text ? "create-text-vertex-shader"
                                         : "create-rect-vertex-shader");
  VkShaderModule fs = create_shader(renderer, frag, frag_words,
                                    text ? "create-text-fragment-shader"
                                         : "create-rect-fragment-shader");
  if (!vs || !fs) {
    if (vs)
      vkDestroyShaderModule(renderer->device, vs, nullptr);
    if (fs)
      vkDestroyShaderModule(renderer->device, fs, nullptr);
    return false;
  }
  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  push.size = sizeof(float) * 2;
  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  if (text) {
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &renderer->descriptor_layout;
  }
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  VkResult result = vkCreatePipelineLayout(renderer->device, &layout_info,
                                           nullptr, out_layout);
  if (result != VK_SUCCESS) {
    vkDestroyShaderModule(renderer->device, vs, nullptr);
    vkDestroyShaderModule(renderer->device, fs, nullptr);
    return fail("create-pipeline-layout", result);
  }
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";
  VkVertexInputBindingDescription binding{0, sizeof(Vertex),
                                          VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attributes[3] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x)},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
  };
  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount = 3;
  vertex_input.pVertexAttributeDescriptions = attributes;
  VkPipelineInputAssemblyStateCreateInfo assembly{};
  assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blending{};
  blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blending.attachmentCount = 1;
  blending.pAttachments = &blend;
  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                     VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  VkGraphicsPipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blending;
  info.pDynamicState = &dynamic;
  info.layout = *out_layout;
  info.renderPass = renderer->render_pass;
  result = vkCreateGraphicsPipelines(renderer->device, VK_NULL_HANDLE, 1, &info,
                                     nullptr, out_pipeline);
  vkDestroyShaderModule(renderer->device, vs, nullptr);
  vkDestroyShaderModule(renderer->device, fs, nullptr);
  return result == VK_SUCCESS || fail("create-graphics-pipeline", result);
}

static bool create_pipelines(TerminalVulkanRenderer *renderer) {
  if (!create_one_pipeline(renderer, false, terminal_rect_vert_spv,
                           terminal_rect_vert_spv_word_count,
                           terminal_rect_frag_spv,
                           terminal_rect_frag_spv_word_count,
                           &renderer->rect_layout, &renderer->rect_pipeline))
    return false;
  return create_one_pipeline(
      renderer, true, terminal_text_vert_spv, terminal_text_vert_spv_word_count,
      terminal_text_frag_spv, terminal_text_frag_spv_word_count,
      &renderer->text_layout, &renderer->text_pipeline);
}

static bool create_swapchain(TerminalVulkanRenderer *renderer) {
  VkSurfaceCapabilitiesKHR caps{};
  VkSurfaceFormatKHR format{};
  VkPresentModeKHR mode{};
  VkCompositeAlphaFlagBitsKHR alpha{};
  if (!choose_surface(renderer, &caps, &format, &mode, &alpha))
    return false;
  VkExtent2D extent = choose_extent(renderer, caps);
  if (extent.width == 0 || extent.height == 0)
    return false;
  uint32_t image_count = caps.minImageCount;
  if (image_count < UINT32_MAX)
    ++image_count;
  if (caps.maxImageCount && image_count > caps.maxImageCount)
    image_count = caps.maxImageCount;
  if (image_count < caps.minImageCount)
    return fail("select-swapchain-image-count", VK_ERROR_INITIALIZATION_FAILED);

  VkSwapchainCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  info.surface = renderer->surface;
  info.minImageCount = image_count;
  info.imageFormat = format.format;
  info.imageColorSpace = format.colorSpace;
  info.imageExtent = extent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = alpha;
  info.presentMode = mode;
  info.clipped = VK_TRUE;
  info.oldSwapchain = renderer->swap.handle;
  VkSwapchainKHR new_handle = VK_NULL_HANDLE;
  VkResult result =
      vkCreateSwapchainKHR(renderer->device, &info, nullptr, &new_handle);
  if (result != VK_SUCCESS)
    return fail("create-swapchain", result);

  result = vkDeviceWaitIdle(renderer->device);
  if (result != VK_SUCCESS) {
    vkDestroySwapchainKHR(renderer->device, new_handle, nullptr);
    return fail("wait-before-swapchain-replace", result);
  }
  destroy_swapchain(renderer);
  destroy_pipelines(renderer);
  renderer->swap.handle = new_handle;
  renderer->swap.format = format.format;
  renderer->swap.color_space = format.colorSpace;
  renderer->swap.extent = extent;
  uint32_t actual = 0;
  result =
      vkGetSwapchainImagesKHR(renderer->device, new_handle, &actual, nullptr);
  if (result != VK_SUCCESS || actual == 0 || actual > kEnumerationLimit)
    return fail("get-swapchain-image-count", result);
  renderer->swap.images =
      static_cast<VkImage *>(calloc(actual, sizeof(VkImage)));
  renderer->swap.views =
      static_cast<VkImageView *>(calloc(actual, sizeof(VkImageView)));
  renderer->swap.framebuffers =
      static_cast<VkFramebuffer *>(calloc(actual, sizeof(VkFramebuffer)));
  renderer->swap.render_finished =
      static_cast<VkSemaphore *>(calloc(actual, sizeof(VkSemaphore)));
  renderer->swap.images_in_flight =
      static_cast<int *>(malloc(actual * sizeof(int)));
  if (!renderer->swap.images || !renderer->swap.views ||
      !renderer->swap.framebuffers || !renderer->swap.render_finished ||
      !renderer->swap.images_in_flight)
    return fail("allocate-swapchain-resources", VK_ERROR_OUT_OF_HOST_MEMORY);
  renderer->swap.image_count = actual;
  for (uint32_t i = 0; i < actual; ++i)
    renderer->swap.images_in_flight[i] = -1;
  result = vkGetSwapchainImagesKHR(renderer->device, new_handle, &actual,
                                   renderer->swap.images);
  if (result != VK_SUCCESS)
    return fail("get-swapchain-images", result);
  if (!create_render_pass(renderer, format.format) ||
      !create_pipelines(renderer))
    return false;
  for (uint32_t i = 0; i < actual; ++i) {
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = renderer->swap.images[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format.format;
    view.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    result = vkCreateImageView(renderer->device, &view, nullptr,
                               &renderer->swap.views[i]);
    if (result != VK_SUCCESS)
      return fail("create-swapchain-image-view", result);
    VkFramebufferCreateInfo framebuffer{};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = renderer->render_pass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &renderer->swap.views[i];
    framebuffer.width = extent.width;
    framebuffer.height = extent.height;
    framebuffer.layers = 1;
    result = vkCreateFramebuffer(renderer->device, &framebuffer, nullptr,
                                 &renderer->swap.framebuffers[i]);
    if (result != VK_SUCCESS)
      return fail("create-swapchain-framebuffer", result);
    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    result = vkCreateSemaphore(renderer->device, &semaphore, nullptr,
                               &renderer->swap.render_finished[i]);
    if (result != VK_SUCCESS)
      return fail("create-present-semaphore", result);
  }
  ++renderer->swapchain_generation;
  renderer->recreate = false;
  fprintf(stderr,
          "[TERM-VK] swapchain generation=%llu format=%d colorspace=%d "
          "present=FIFO extent=%ux%u images=%u\n",
          static_cast<unsigned long long>(renderer->swapchain_generation),
          format.format, format.colorSpace, extent.width, extent.height,
          actual);
  return true;
}

static bool create_descriptor_resources(TerminalVulkanRenderer *renderer) {
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo layout{};
  layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout.bindingCount = 1;
  layout.pBindings = &binding;
  VkResult result = vkCreateDescriptorSetLayout(
      renderer->device, &layout, nullptr, &renderer->descriptor_layout);
  if (result != VK_SUCCESS)
    return fail("create-descriptor-layout", result);
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo pool{};
  pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool.maxSets = 1;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &size;
  result = vkCreateDescriptorPool(renderer->device, &pool, nullptr,
                                  &renderer->descriptor_pool);
  if (result != VK_SUCCESS)
    return fail("create-descriptor-pool", result);
  VkDescriptorSetAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate.descriptorPool = renderer->descriptor_pool;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &renderer->descriptor_layout;
  result = vkAllocateDescriptorSets(renderer->device, &allocate,
                                    &renderer->descriptor_set);
  return result == VK_SUCCESS || fail("allocate-descriptor-set", result);
}

static bool create_atlas(TerminalVulkanRenderer *renderer, uint32_t width,
                         uint32_t height) {
  VkFormatProperties props{};
  vkGetPhysicalDeviceFormatProperties(renderer->physical_device,
                                      VK_FORMAT_R8_UNORM, &props);
  renderer->atlas_format =
      (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
          ? VK_FORMAT_R8_UNORM
          : VK_FORMAT_R8G8B8A8_UNORM;
  VkImageCreateInfo image{};
  image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = renderer->atlas_format;
  image.extent = {width, height, 1};
  image.mipLevels = 1;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkResult result =
      vkCreateImage(renderer->device, &image, nullptr, &renderer->atlas);
  if (result != VK_SUCCESS)
    return fail("create-atlas-image", result);
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(renderer->device, renderer->atlas,
                               &requirements);
  uint32_t type = find_memory_type(renderer, requirements.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
  if (type == UINT32_MAX)
    return fail("find-atlas-memory", VK_ERROR_FEATURE_NOT_PRESENT);
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  result = vkAllocateMemory(renderer->device, &allocate, nullptr,
                            &renderer->atlas_memory);
  if (result != VK_SUCCESS)
    return fail("allocate-atlas-memory", result);
  result = vkBindImageMemory(renderer->device, renderer->atlas,
                             renderer->atlas_memory, 0);
  if (result != VK_SUCCESS)
    return fail("bind-atlas-memory", result);
  VkImageViewCreateInfo view{};
  view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view.image = renderer->atlas;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = renderer->atlas_format;
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view.subresourceRange.levelCount = 1;
  view.subresourceRange.layerCount = 1;
  result = vkCreateImageView(renderer->device, &view, nullptr,
                             &renderer->atlas_view);
  if (result != VK_SUCCESS)
    return fail("create-atlas-view", result);
  VkSamplerCreateInfo sampler{};
  sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler.magFilter = VK_FILTER_NEAREST;
  sampler.minFilter = VK_FILTER_NEAREST;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.maxLod = 0.0f;
  result = vkCreateSampler(renderer->device, &sampler, nullptr,
                           &renderer->atlas_sampler);
  if (result != VK_SUCCESS)
    return fail("create-atlas-sampler", result);
  VkDescriptorImageInfo image_info{renderer->atlas_sampler,
                                   renderer->atlas_view,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = renderer->descriptor_set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &image_info;
  vkUpdateDescriptorSets(renderer->device, 1, &write, 0, nullptr);
  renderer->atlas_width = width;
  renderer->atlas_height = height;
  return true;
}

static bool create_staging_buffer(TerminalVulkanRenderer *renderer,
                                  VkDeviceSize size, VkBuffer *buffer,
                                  VkDeviceMemory *memory, void **mapped,
                                  bool *coherent) {
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = size;
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult result = vkCreateBuffer(renderer->device, &info, nullptr, buffer);
  if (result != VK_SUCCESS)
    return fail("create-atlas-staging", result);
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(renderer->device, *buffer, &requirements);
  uint32_t type =
      find_memory_type(renderer, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, coherent);
  if (type == UINT32_MAX)
    return fail("find-atlas-staging-memory", VK_ERROR_FEATURE_NOT_PRESENT);
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  result = vkAllocateMemory(renderer->device, &allocate, nullptr, memory);
  if (result == VK_SUCCESS)
    result = vkBindBufferMemory(renderer->device, *buffer, *memory, 0);
  if (result == VK_SUCCESS)
    result = vkMapMemory(renderer->device, *memory, 0, size, 0, mapped);
  return result == VK_SUCCESS || fail("map-atlas-staging", result);
}

static bool upload_atlas(TerminalVulkanRenderer *renderer,
                         const TerminalFrame *frame) {
  if (!frame->atlas_pixels || frame->atlas_width == 0 ||
      frame->atlas_height == 0)
    return fail("validate-atlas", VK_ERROR_INITIALIZATION_FAILED);
  if (!renderer->atlas) {
    if (!create_atlas(renderer, frame->atlas_width, frame->atlas_height))
      return false;
  } else if (renderer->atlas_width != frame->atlas_width ||
             renderer->atlas_height != frame->atlas_height) {
    return fail("atlas-size-changed", VK_ERROR_INITIALIZATION_FAILED);
  }
  size_t pixels = 0;
  if (!checked_mul(frame->atlas_width, frame->atlas_height, &pixels))
    return fail("atlas-size-overflow", VK_ERROR_OUT_OF_HOST_MEMORY);
  size_t bytes = pixels;
  uint8_t *expanded = nullptr;
  if (renderer->atlas_format == VK_FORMAT_R8G8B8A8_UNORM) {
    if (!checked_mul(pixels, 4, &bytes))
      return fail("atlas-rgba-size-overflow", VK_ERROR_OUT_OF_HOST_MEMORY);
    expanded = static_cast<uint8_t *>(malloc(bytes));
    if (!expanded)
      return fail("allocate-atlas-rgba", VK_ERROR_OUT_OF_HOST_MEMORY);
    for (size_t i = 0; i < pixels; ++i) {
      expanded[i * 4 + 0] = frame->atlas_pixels[i];
      expanded[i * 4 + 1] = frame->atlas_pixels[i];
      expanded[i * 4 + 2] = frame->atlas_pixels[i];
      expanded[i * 4 + 3] = frame->atlas_pixels[i];
    }
  }
  VkResult result = vkDeviceWaitIdle(renderer->device);
  if (result != VK_SUCCESS) {
    free(expanded);
    return fail("wait-before-atlas-upload", result);
  }
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = nullptr;
  bool coherent = false;
  if (!create_staging_buffer(renderer, bytes, &staging, &memory, &mapped,
                             &coherent)) {
    free(expanded);
    return false;
  }
  memcpy(mapped, expanded ? expanded : frame->atlas_pixels, bytes);
  free(expanded);
  if (!coherent) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = memory;
    range.size = VK_WHOLE_SIZE;
    result = vkFlushMappedMemoryRanges(renderer->device, 1, &range);
    if (result != VK_SUCCESS) {
      vkUnmapMemory(renderer->device, memory);
      vkDestroyBuffer(renderer->device, staging, nullptr);
      vkFreeMemory(renderer->device, memory, nullptr);
      return fail("flush-atlas-staging", result);
    }
  }
  vkUnmapMemory(renderer->device, memory);
  FrameSlot *slot = &renderer->frames[0];
  vkResetCommandBuffer(slot->command, 0);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = vkBeginCommandBuffer(slot->command, &begin);
  if (result != VK_SUCCESS)
    return fail("begin-atlas-upload", result);
  VkImageMemoryBarrier before{};
  before.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  before.oldLayout = renderer->atlas_ready
                         ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                         : VK_IMAGE_LAYOUT_UNDEFINED;
  before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  before.image = renderer->atlas;
  before.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  before.subresourceRange.levelCount = 1;
  before.subresourceRange.layerCount = 1;
  before.srcAccessMask = renderer->atlas_ready ? VK_ACCESS_SHADER_READ_BIT : 0;
  before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(
      slot->command,
      renderer->atlas_ready ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &before);
  VkBufferImageCopy copy{};
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {frame->atlas_width, frame->atlas_height, 1};
  vkCmdCopyBufferToImage(slot->command, staging, renderer->atlas,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  VkImageMemoryBarrier after = before;
  after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  after.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(slot->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &after);
  result = vkEndCommandBuffer(slot->command);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &slot->command;
  if (result == VK_SUCCESS)
    result = vkQueueSubmit(renderer->queue, 1, &submit, VK_NULL_HANDLE);
  if (result == VK_SUCCESS)
    result = vkQueueWaitIdle(renderer->queue);
  vkDestroyBuffer(renderer->device, staging, nullptr);
  vkFreeMemory(renderer->device, memory, nullptr);
  if (result != VK_SUCCESS)
    return fail("submit-atlas-upload", result);
  renderer->atlas_ready = true;
  renderer->atlas_generation = frame->atlas_generation;
  return true;
}

static void append_quad(Vertex *vertices, size_t *cursor, float x, float y,
                        float width, float height, float u0, float v0, float u1,
                        float v1, const float color[4]) {
  const float pos[6][4] = {
      {x, y, u0, v0},          {x + width, y, u1, v0},
      {x, y + height, u0, v1}, {x, y + height, u0, v1},
      {x + width, y, u1, v0},  {x + width, y + height, u1, v1},
  };
  for (unsigned i = 0; i < 6; ++i) {
    Vertex *vertex = &vertices[(*cursor)++];
    vertex->x = pos[i][0];
    vertex->y = pos[i][1];
    vertex->u = pos[i][2];
    vertex->v = pos[i][3];
    memcpy(vertex->color, color, sizeof(vertex->color));
  }
}

static bool validate_batch(const void *items, size_t count) {
  return count == 0 || items != nullptr;
}

struct BatchOffsets {
  uint32_t background_first, background_count;
  uint32_t glyph_first, glyph_count;
  uint32_t decoration_first, decoration_count;
  uint32_t cursor_first, cursor_count;
  uint32_t cursor_glyph_first, cursor_glyph_count;
};

static bool build_vertices(TerminalVulkanRenderer *renderer,
                           const TerminalDrawList *list, FrameSlot *slot,
                           BatchOffsets *offsets, VkDeviceSize *used_bytes) {
  if (!list ||
      !validate_batch(list->backgrounds.items, list->backgrounds.count) ||
      !validate_batch(list->glyphs.items, list->glyphs.count) ||
      !validate_batch(list->decorations.items, list->decorations.count) ||
      !validate_batch(list->cursor.items, list->cursor.count) ||
      !validate_batch(list->cursor_glyphs.items, list->cursor_glyphs.count))
    return fail("validate-draw-list", VK_ERROR_INITIALIZATION_FAILED);
  size_t quads = list->backgrounds.count;
  if (SIZE_MAX - quads < list->glyphs.count ||
      SIZE_MAX - (quads += list->glyphs.count) < list->decorations.count ||
      SIZE_MAX - (quads += list->decorations.count) < list->cursor.count ||
      SIZE_MAX - (quads += list->cursor.count) < list->cursor_glyphs.count)
    return fail("draw-count-overflow", VK_ERROR_OUT_OF_HOST_MEMORY);
  quads += list->cursor_glyphs.count;
  size_t vertex_count = 0, bytes = 0;
  if (!checked_mul(quads, 6, &vertex_count) || vertex_count > UINT32_MAX ||
      !checked_mul(vertex_count, sizeof(Vertex), &bytes))
    return fail("vertex-size-overflow", VK_ERROR_OUT_OF_HOST_MEMORY);
  if (!ensure_vertex_buffer(renderer, slot, bytes ? bytes : sizeof(Vertex)))
    return false;
  auto *vertices = static_cast<Vertex *>(slot->vertices);
  size_t cursor = 0;
  offsets->background_first = cursor;
  for (size_t i = 0; i < list->backgrounds.count; ++i) {
    const TerminalRect &r = list->backgrounds.items[i];
    append_quad(vertices, &cursor, r.x, r.y, r.width, r.height, 0, 0, 0, 0,
                r.color);
  }
  offsets->background_count = cursor - offsets->background_first;
  offsets->glyph_first = cursor;
  for (size_t i = 0; i < list->glyphs.count; ++i) {
    const TerminalGlyphQuad &g = list->glyphs.items[i];
    append_quad(vertices, &cursor, g.x, g.y, g.width, g.height, g.u0, g.v0,
                g.u1, g.v1, g.color);
  }
  offsets->glyph_count = cursor - offsets->glyph_first;
  offsets->decoration_first = cursor;
  for (size_t i = 0; i < list->decorations.count; ++i) {
    const TerminalRect &r = list->decorations.items[i];
    append_quad(vertices, &cursor, r.x, r.y, r.width, r.height, 0, 0, 0, 0,
                r.color);
  }
  offsets->decoration_count = cursor - offsets->decoration_first;
  offsets->cursor_first = cursor;
  for (size_t i = 0; i < list->cursor.count; ++i) {
    const TerminalRect &r = list->cursor.items[i];
    append_quad(vertices, &cursor, r.x, r.y, r.width, r.height, 0, 0, 0, 0,
                r.color);
  }
  offsets->cursor_count = cursor - offsets->cursor_first;
  offsets->cursor_glyph_first = cursor;
  for (size_t i = 0; i < list->cursor_glyphs.count; ++i) {
    const TerminalGlyphQuad &g = list->cursor_glyphs.items[i];
    append_quad(vertices, &cursor, g.x, g.y, g.width, g.height, g.u0, g.v0,
                g.u1, g.v1, g.color);
  }
  offsets->cursor_glyph_count = cursor - offsets->cursor_glyph_first;
  *used_bytes = bytes;
  if (!slot->coherent && bytes) {
    VkDeviceSize atom = renderer->properties.limits.nonCoherentAtomSize;
    VkDeviceSize flush_size = (bytes + atom - 1) / atom * atom;
    if (flush_size > slot->capacity)
      flush_size = VK_WHOLE_SIZE;
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = slot->vertex_memory;
    range.size = flush_size;
    VkResult result = vkFlushMappedMemoryRanges(renderer->device, 1, &range);
    if (result != VK_SUCCESS)
      return fail("flush-vertex-memory", result);
  }
  return true;
}

static bool record_frame(TerminalVulkanRenderer *renderer, FrameSlot *slot,
                         uint32_t image_index, const BatchOffsets &offsets) {
  VkResult result = vkResetCommandBuffer(slot->command, 0);
  if (result != VK_SUCCESS)
    return fail("reset-frame-command", result);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = vkBeginCommandBuffer(slot->command, &begin);
  if (result != VK_SUCCESS)
    return fail("begin-frame-command", result);
  VkClearValue clear{};
  clear.color.float32[3] = renderer->opaque ? 1.0f : 0.0f;
  VkRenderPassBeginInfo pass{};
  pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  pass.renderPass = renderer->render_pass;
  pass.framebuffer = renderer->swap.framebuffers[image_index];
  pass.renderArea.extent = renderer->swap.extent;
  pass.clearValueCount = 1;
  pass.pClearValues = &clear;
  vkCmdBeginRenderPass(slot->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{0,
                      0,
                      static_cast<float>(renderer->swap.extent.width),
                      static_cast<float>(renderer->swap.extent.height),
                      0,
                      1};
  VkRect2D scissor{{0, 0}, renderer->swap.extent};
  vkCmdSetViewport(slot->command, 0, 1, &viewport);
  vkCmdSetScissor(slot->command, 0, 1, &scissor);
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(slot->command, 0, 1, &slot->vertex_buffer, &zero);
  float projection[2] = {static_cast<float>(renderer->logical_width),
                         static_cast<float>(renderer->logical_height)};
  auto draw_rects = [&](uint32_t first, uint32_t count) {
    if (!count)
      return;
    vkCmdBindPipeline(slot->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      renderer->rect_pipeline);
    vkCmdPushConstants(slot->command, renderer->rect_layout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(projection),
                       projection);
    vkCmdDraw(slot->command, count, 1, first, 0);
  };
  auto draw_text = [&](uint32_t first, uint32_t count) {
    if (!count)
      return;
    vkCmdBindPipeline(slot->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      renderer->text_pipeline);
    vkCmdBindDescriptorSets(slot->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer->text_layout, 0, 1,
                            &renderer->descriptor_set, 0, nullptr);
    vkCmdPushConstants(slot->command, renderer->text_layout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(projection),
                       projection);
    vkCmdDraw(slot->command, count, 1, first, 0);
  };
  draw_rects(offsets.background_first, offsets.background_count);
  draw_text(offsets.glyph_first, offsets.glyph_count);
  draw_rects(offsets.decoration_first, offsets.decoration_count);
  draw_rects(offsets.cursor_first, offsets.cursor_count);
  draw_text(offsets.cursor_glyph_first, offsets.cursor_glyph_count);
  vkCmdEndRenderPass(slot->command);
  result = vkEndCommandBuffer(slot->command);
  return result == VK_SUCCESS || fail("end-frame-command", result);
}

static void frame_done(void *data, wl_callback *callback, uint32_t time) {
  (void)time;
  auto *renderer = static_cast<TerminalVulkanRenderer *>(data);
  if (renderer->frame_callback == callback)
    renderer->frame_callback = nullptr;
  wl_callback_destroy(callback);
  if (!renderer->fatal && renderer->frame_ready)
    renderer->frame_ready(renderer->frame_data);
}

static const wl_callback_listener frame_listener = {.done = frame_done};

static void clear_frame_callback(TerminalVulkanRenderer *renderer);

static bool schedule_retry(TerminalVulkanRenderer *renderer) {
  renderer->frame_callback = wl_display_sync(renderer->display);
  if (!renderer->frame_callback ||
      wl_callback_add_listener(renderer->frame_callback, &frame_listener,
                               renderer) < 0) {
    clear_frame_callback(renderer);
    return fail("schedule-render-retry", VK_ERROR_INITIALIZATION_FAILED);
  }
  return true;
}

static void clear_frame_callback(TerminalVulkanRenderer *renderer) {
  if (renderer->frame_callback) {
    wl_callback_destroy(renderer->frame_callback);
    renderer->frame_callback = nullptr;
  }
}

} // namespace

bool terminal_vk_create(TerminalVulkanRenderer **out, wl_display *display,
                        wl_surface *surface, uint32_t logical_width,
                        uint32_t logical_height, uint32_t buffer_scale,
                        TerminalVkFrameReady frame_ready, void *frame_data) {
  if (!out || *out || !display || !surface || !logical_width ||
      !logical_height || !buffer_scale)
    return false;
  auto *renderer = static_cast<TerminalVulkanRenderer *>(
      calloc(1, sizeof(TerminalVulkanRenderer)));
  if (!renderer)
    return false;
  renderer->display = display;
  renderer->wayland_surface = surface;
  renderer->logical_width = logical_width;
  renderer->logical_height = logical_height;
  renderer->buffer_scale = buffer_scale;
  renderer->frame_ready = frame_ready;
  renderer->frame_data = frame_data;
  *out = renderer;
  if (!create_instance(renderer) || !create_surface(renderer) ||
      !select_device(renderer) || !create_device(renderer) ||
      !create_command_resources(renderer) ||
      !create_descriptor_resources(renderer)) {
    terminal_vk_destroy(out);
    return false;
  }
  renderer->recreate = true;
  return true;
}

void terminal_vk_resize(TerminalVulkanRenderer *renderer,
                        uint32_t logical_width, uint32_t logical_height,
                        uint32_t buffer_scale) {
  if (!renderer || buffer_scale == 0)
    return;
  if (renderer->logical_width != logical_width ||
      renderer->logical_height != logical_height ||
      renderer->buffer_scale != buffer_scale) {
    renderer->logical_width = logical_width;
    renderer->logical_height = logical_height;
    renderer->buffer_scale = buffer_scale;
    renderer->recreate = true;
  }
}

TerminalVkRenderResult terminal_vk_render(TerminalVulkanRenderer *renderer,
                                          const TerminalFrame *frame) {
  if (!renderer || !frame || renderer->fatal || renderer->rendering)
    return TERMINAL_VK_FATAL;
  if (renderer->frame_callback) {
    static bool callback_retry_logged;
    if (!callback_retry_logged) {
      fprintf(stderr, "[TERM-VK] render deferred: frame callback pending\n");
      callback_retry_logged = true;
    }
    return TERMINAL_VK_RETRY;
  }
  if (!frame->logical_width || !frame->logical_height)
    return TERMINAL_VK_RETRY;
  renderer->rendering = true;
  terminal_vk_resize(renderer, frame->logical_width, frame->logical_height,
                     renderer->buffer_scale);
  if (renderer->recreate || !renderer->swap.handle) {
    if (!create_swapchain(renderer)) {
      renderer->rendering = false;
      if (!renderer->logical_width || !renderer->logical_height)
        return TERMINAL_VK_RETRY;
      renderer->fatal = true;
      return TERMINAL_VK_FATAL;
    }
  }
  if (!renderer->atlas_ready ||
      renderer->atlas_generation != frame->atlas_generation) {
    if (!upload_atlas(renderer, frame)) {
      renderer->fatal = true;
      renderer->rendering = false;
      return TERMINAL_VK_FATAL;
    }
  }

  FrameSlot *slot = &renderer->frames[renderer->frame_index];
  VkResult result = VK_SUCCESS;
  if (slot->submitted) {
    result = vkWaitForFences(renderer->device, 1, &slot->fence, VK_TRUE,
                             kFenceTimeoutNs);
    if (result != VK_SUCCESS) {
      fail("wait-frame-fence", result);
      renderer->fatal = true;
      renderer->rendering = false;
      return TERMINAL_VK_FATAL;
    }
    for (uint32_t i = 0; i < renderer->swap.image_count; ++i)
      if (renderer->swap.images_in_flight[i] ==
          static_cast<int>(renderer->frame_index))
        renderer->swap.images_in_flight[i] = -1;
    slot->submitted = false;
  }
  BatchOffsets offsets{};
  VkDeviceSize used = 0;
  if (!build_vertices(renderer, frame->draw_list, slot, &offsets, &used)) {
    renderer->fatal = true;
    renderer->rendering = false;
    return TERMINAL_VK_FATAL;
  }
  uint32_t image_index = 0;
  result = vkAcquireNextImageKHR(renderer->device, renderer->swap.handle,
                                 kAcquireTimeoutNs, slot->acquired,
                                 VK_NULL_HANDLE, &image_index);
  if (result == VK_TIMEOUT || result == VK_NOT_READY) {
    static bool acquire_retry_logged;
    if (!acquire_retry_logged) {
      fprintf(stderr, "[TERM-VK] render retry: acquire result=%s\n",
              vk_result_name(result));
      acquire_retry_logged = true;
    }
    if (!schedule_retry(renderer)) {
      renderer->fatal = true;
      renderer->rendering = false;
      return TERMINAL_VK_FATAL;
    }
    renderer->rendering = false;
    return TERMINAL_VK_RETRY;
  }
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    renderer->recreate = true;
    renderer->rendering = false;
    return TERMINAL_VK_RETRY;
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    fail("acquire-swapchain-image", result);
    renderer->fatal = true;
    renderer->rendering = false;
    return TERMINAL_VK_FATAL;
  }
  if (result == VK_SUBOPTIMAL_KHR)
    renderer->recreate = true;
  int previous = renderer->swap.images_in_flight[image_index];
  if (previous >= 0) {
    result =
        vkWaitForFences(renderer->device, 1, &renderer->frames[previous].fence,
                        VK_TRUE, kFenceTimeoutNs);
    if (result != VK_SUCCESS) {
      fail("wait-swapchain-image-fence", result);
      renderer->fatal = true;
      renderer->rendering = false;
      return TERMINAL_VK_FATAL;
    }
  }
  renderer->swap.images_in_flight[image_index] = renderer->frame_index;
  if (!record_frame(renderer, slot, image_index, offsets)) {
    renderer->fatal = true;
    renderer->rendering = false;
    return TERMINAL_VK_FATAL;
  }
  result = vkResetFences(renderer->device, 1, &slot->fence);
  VkPipelineStageFlags wait_stage =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &slot->acquired;
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &slot->command;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &renderer->swap.render_finished[image_index];
  if (result == VK_SUCCESS)
    result = vkQueueSubmit(renderer->queue, 1, &submit, slot->fence);
  if (result != VK_SUCCESS) {
    fail("submit-frame", result);
    renderer->fatal = true;
    renderer->rendering = false;
    return TERMINAL_VK_FATAL;
  }
  slot->submitted = true;
  renderer->frame_callback = wl_surface_frame(renderer->wayland_surface);
  if (!renderer->frame_callback ||
      wl_callback_add_listener(renderer->frame_callback, &frame_listener,
                               renderer) < 0) {
    clear_frame_callback(renderer);
    fail("create-wayland-frame-callback", VK_ERROR_INITIALIZATION_FAILED);
    renderer->fatal = true;
    renderer->rendering = false;
    return TERMINAL_VK_FATAL;
  }
  VkPresentInfoKHR present{};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &renderer->swap.render_finished[image_index];
  present.swapchainCount = 1;
  present.pSwapchains = &renderer->swap.handle;
  present.pImageIndices = &image_index;
  result = vkQueuePresentKHR(renderer->queue, &present);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    clear_frame_callback(renderer);
    renderer->recreate = true;
    renderer->rendering = false;
    return TERMINAL_VK_RETRY;
  }
  if (result == VK_SUBOPTIMAL_KHR)
    renderer->recreate = true;
  else if (result != VK_SUCCESS) {
    clear_frame_callback(renderer);
    fail("present-frame", result);
    renderer->fatal = true;
    renderer->rendering = false;
    return TERMINAL_VK_FATAL;
  }
  static bool first_present = true;
  if (first_present) {
    fprintf(stderr, "[TERM-VK] present ok generation=%llu\n",
            static_cast<unsigned long long>(renderer->swapchain_generation));
    first_present = false;
  }
  static bool first_text_present = true;
  if (first_text_present && offsets.glyph_count) {
    fprintf(stderr,
            "[TERM-VK] text present vertices=%u atlas_generation=%llu\n",
            offsets.glyph_count,
            static_cast<unsigned long long>(renderer->atlas_generation));
    first_text_present = false;
  }
  renderer->frame_index = (renderer->frame_index + 1) % kFramesInFlight;
  renderer->rendering = false;
  return TERMINAL_VK_OK;
}

void terminal_vk_destroy(TerminalVulkanRenderer **renderer_ptr) {
  if (!renderer_ptr || !*renderer_ptr)
    return;
  TerminalVulkanRenderer *renderer = *renderer_ptr;
  renderer->fatal = true;
  clear_frame_callback(renderer);
  if (renderer->device)
    vkDeviceWaitIdle(renderer->device);
  destroy_swapchain(renderer);
  destroy_pipelines(renderer);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    destroy_vertex_buffer(renderer, &renderer->frames[i]);
    if (renderer->frames[i].acquired)
      vkDestroySemaphore(renderer->device, renderer->frames[i].acquired,
                         nullptr);
    if (renderer->frames[i].fence)
      vkDestroyFence(renderer->device, renderer->frames[i].fence, nullptr);
  }
  if (renderer->atlas_sampler)
    vkDestroySampler(renderer->device, renderer->atlas_sampler, nullptr);
  if (renderer->atlas_view)
    vkDestroyImageView(renderer->device, renderer->atlas_view, nullptr);
  if (renderer->atlas)
    vkDestroyImage(renderer->device, renderer->atlas, nullptr);
  if (renderer->atlas_memory)
    vkFreeMemory(renderer->device, renderer->atlas_memory, nullptr);
  if (renderer->descriptor_pool)
    vkDestroyDescriptorPool(renderer->device, renderer->descriptor_pool,
                            nullptr);
  if (renderer->descriptor_layout)
    vkDestroyDescriptorSetLayout(renderer->device, renderer->descriptor_layout,
                                 nullptr);
  if (renderer->command_pool)
    vkDestroyCommandPool(renderer->device, renderer->command_pool, nullptr);
  if (renderer->device)
    vkDestroyDevice(renderer->device, nullptr);
  if (renderer->surface)
    vkDestroySurfaceKHR(renderer->instance, renderer->surface, nullptr);
  if (renderer->instance)
    vkDestroyInstance(renderer->instance, nullptr);
  free(renderer);
  *renderer_ptr = nullptr;
}
