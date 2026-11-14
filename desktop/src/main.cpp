#include "app_state.hpp"
#include "core/serve/daemon_control.hpp"
#include "i18n.hpp"
#include "views/common.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui_freetype.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using upix::views::DialogBridge;
using upix::views::MmapFontRegion;
using upix::views::UiViewState;
using upix::views::VulkanTexture;
using upix::views::kTopbarH;
using upix::views::renderModelsPage;
using upix::views::renderPairingModal;
using upix::views::renderRemotePage;
using upix::views::renderSettingsPage;
using upix::views::renderTopBar;
using upix::views::renderWorkbench;

namespace {

static VkAllocationCallbacks* g_Allocator = nullptr;
static VkInstance g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice g_Device = VK_NULL_HANDLE;
static uint32_t g_QueueFamily = (uint32_t)-1;
static VkQueue g_Queue = VK_NULL_HANDLE;
static VkPipelineCache g_PipelineCache = VK_NULL_HANDLE;
static ImGui_ImplVulkanH_Window g_MainWindowData;
static uint32_t g_MinImageCount = 2;
static bool g_SwapChainRebuild = false;
static bool g_OffscreenMode = false;

struct OffscreenTarget {
    uint32_t width = 1280;
    uint32_t height = 840;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkClearValue clearValue{};
};
static OffscreenTarget g_Offscreen;

// Dedicated upload command pool + fence (isolated from swapchain frame pools)
static VkCommandPool g_UploadPool = VK_NULL_HANDLE;
static VkCommandBuffer g_UploadCmd = VK_NULL_HANDLE;
static VkFence g_UploadFence = VK_NULL_HANDLE;

struct PendingTexDestroy {
    VulkanTexture tex;
    uint64_t retireFrame = 0;
};
static std::vector<PendingTexDestroy> g_DestroyQueue;
static uint64_t g_FrameCounter = 0;

static void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS) return;
    std::fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0) std::abort();
}

static bool IsExtensionAvailable(const std::vector<VkExtensionProperties>& props, const char* ext) {
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, ext) == 0) return true;
    }
    return false;
}

static uint32_t FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

static VkPhysicalDevice SelectGuiPhysicalDevice(VkInstance instance) {
    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    if (gpu_count == 0) return VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());

    for (VkPhysicalDevice dev : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            return dev;
    }
    for (VkPhysicalDevice dev : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            return dev;
    }
    return gpus[0];
}

static void SetupVulkan(std::vector<const char*> instance_extensions, bool withSwapchain) {
    VkResult err;
    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

    uint32_t props_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &props_count, nullptr);
    std::vector<VkExtensionProperties> props(props_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &props_count, props.data());

    if (IsExtensionAvailable(props, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    if (IsExtensionAvailable(props, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif

    create_info.enabledExtensionCount = (uint32_t)instance_extensions.size();
    create_info.ppEnabledExtensionNames = instance_extensions.empty() ? nullptr : instance_extensions.data();
    err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
    check_vk_result(err);

    g_PhysicalDevice = SelectGuiPhysicalDevice(g_Instance);
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);

    std::vector<const char*> device_extensions;
    if (withSwapchain) {
        device_extensions.push_back("VK_KHR_swapchain");
    }

    uint32_t dev_props_count = 0;
    vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &dev_props_count, nullptr);
    std::vector<VkExtensionProperties> dev_props(dev_props_count);
    vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &dev_props_count, dev_props.data());
    // MoltenVK requires VK_KHR_portability_subset whenever the device advertises it;
    // check by string so vulkan_beta.h is not needed.
    if (IsExtensionAvailable(dev_props, "VK_KHR_portability_subset"))
        device_extensions.push_back("VK_KHR_portability_subset");

    const float queue_priority[] = {1.0f};
    VkDeviceQueueCreateInfo queue_info[1] = {};
    queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info[0].queueFamilyIndex = g_QueueFamily;
    queue_info[0].queueCount = 1;
    queue_info[0].pQueuePriorities = queue_priority;

    VkDeviceCreateInfo dev_info = {};
    dev_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dev_info.queueCreateInfoCount = 1;
    dev_info.pQueueCreateInfos = queue_info;
    dev_info.enabledExtensionCount = (uint32_t)device_extensions.size();
    dev_info.ppEnabledExtensionNames = device_extensions.empty() ? nullptr : device_extensions.data();
    err = vkCreateDevice(g_PhysicalDevice, &dev_info, g_Allocator, &g_Device);
    check_vk_result(err);
    vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = g_QueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check_vk_result(vkCreateCommandPool(g_Device, &poolInfo, g_Allocator, &g_UploadPool));

    VkCommandBufferAllocateInfo cmdAlloc = {};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = g_UploadPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    check_vk_result(vkAllocateCommandBuffers(g_Device, &cmdAlloc, &g_UploadCmd));

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    check_vk_result(vkCreateFence(g_Device, &fenceInfo, g_Allocator, &g_UploadFence));
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height, VkImageUsageFlags extra_usage) {
    wd->Surface = surface;
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE) {
        std::fprintf(stderr, "Error: no WSI support on physical device\n");
        std::exit(-1);
    }

    const VkFormat requestSurfaceImageFormat[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM
    };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat,
        (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

    VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        g_PhysicalDevice, wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));

    ImGui_ImplVulkanH_CreateOrResizeWindow(
        g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily,
        g_Allocator, width, height, g_MinImageCount, extra_usage);
}

static void CleanupVulkanWindow() {
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

static void DestroyVulkanTextureImmediate(VulkanTexture& tex) {
    if (tex.desc) ImGui_ImplVulkan_RemoveTexture(tex.desc);
    if (tex.descNearest) ImGui_ImplVulkan_RemoveTexture(tex.descNearest);
    if (tex.sampler) vkDestroySampler(g_Device, tex.sampler, g_Allocator);
    if (tex.samplerNearest) vkDestroySampler(g_Device, tex.samplerNearest, g_Allocator);
    if (tex.view) vkDestroyImageView(g_Device, tex.view, g_Allocator);
    if (tex.image) vkDestroyImage(g_Device, tex.image, g_Allocator);
    if (tex.memory) vkFreeMemory(g_Device, tex.memory, g_Allocator);
    tex = VulkanTexture{};
}

static void FlushDestroyQueue(bool forceAll = false) {
    auto it = g_DestroyQueue.begin();
    while (it != g_DestroyQueue.end()) {
        if (forceAll || g_FrameCounter >= it->retireFrame) {
            DestroyVulkanTextureImmediate(it->tex);
            it = g_DestroyQueue.erase(it);
        } else {
            ++it;
        }
    }
}

static void QueueDestroyVulkanTexture(VulkanTexture& tex) {
    if (!tex.valid() && tex.image == VK_NULL_HANDLE) return;
    const uint64_t lag = g_OffscreenMode
        ? 2ULL
        : static_cast<uint64_t>(std::max<uint32_t>(2u, g_MainWindowData.ImageCount)) + 1;
    g_DestroyQueue.push_back(PendingTexDestroy{tex, g_FrameCounter + lag});
    tex = VulkanTexture{};
}

static void CleanupVulkan() {
    if (g_UploadFence) {
        vkDestroyFence(g_Device, g_UploadFence, g_Allocator);
        g_UploadFence = VK_NULL_HANDLE;
    }
    if (g_UploadPool) {
        vkDestroyCommandPool(g_Device, g_UploadPool, g_Allocator);
        g_UploadPool = VK_NULL_HANDLE;
    }
    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
    VkSemaphore image_acquired_semaphore  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
        check_vk_result(err);
        err = vkResetFences(g_Device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
    vkCmdEndRenderPass(fd->CommandBuffer);

    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd) {
    if (g_SwapChainRebuild) return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

static bool CaptureVkImageToPng(VkImage image, VkImageLayout currentLayout, VkFormat format,
                                uint32_t w, uint32_t h, const std::string& outPath) {
    vkDeviceWaitIdle(g_Device);
    VkDeviceSize bufSize = (VkDeviceSize)w * h * 4;

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = bufSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufInfo, g_Allocator, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(g_Device, stagingBuf, &memReq);
    uint32_t memIdx = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memIdx == UINT32_MAX) {
        vkDestroyBuffer(g_Device, stagingBuf, g_Allocator);
        return false;
    }
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, stagingBuf, g_Allocator);
        return false;
    }
    vkBindBufferMemory(g_Device, stagingBuf, stagingMem, 0);

    vkResetFences(g_Device, 1, &g_UploadFence);
    vkResetCommandPool(g_Device, g_UploadPool, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_UploadCmd, &beginInfo);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = currentLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g_UploadCmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(g_UploadCmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = currentLayout;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(g_UploadCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(g_UploadCmd);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_UploadCmd;
    vkQueueSubmit(g_Queue, 1, &submit, g_UploadFence);
    vkWaitForFences(g_Device, 1, &g_UploadFence, VK_TRUE, UINT64_MAX);

    void* mapped = nullptr;
    vkMapMemory(g_Device, stagingMem, 0, bufSize, 0, &mapped);
    upix::Image img((int)w, (int)h, 3);
    const uint8_t* src = static_cast<const uint8_t*>(mapped);
    uint8_t* dst = img.data();
    const bool isBgra = (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB);
    for (uint32_t i = 0; i < w * h; ++i) {
        if (isBgra) {
            dst[i * 3 + 0] = src[i * 4 + 2];
            dst[i * 3 + 1] = src[i * 4 + 1];
            dst[i * 3 + 2] = src[i * 4 + 0];
        } else {
            dst[i * 3 + 0] = src[i * 4 + 0];
            dst[i * 3 + 1] = src[i * 4 + 1];
            dst[i * 3 + 2] = src[i * 4 + 2];
        }
    }
    vkUnmapMemory(g_Device, stagingMem);
    vkDestroyBuffer(g_Device, stagingBuf, g_Allocator);
    vkFreeMemory(g_Device, stagingMem, g_Allocator);

    std::error_code ec;
    fs::path p(outPath);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    auto saved = img.save(outPath);
    return saved.has_value();
}

static bool CaptureSwapchainToPng(ImGui_ImplVulkanH_Window* wd, const std::string& outPath) {
    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    return CaptureVkImageToPng(fd->Backbuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                               wd->SurfaceFormat.format, (uint32_t)wd->Width, (uint32_t)wd->Height, outPath);
}

static void CreateOffscreenTarget(OffscreenTarget* ot, uint32_t w, uint32_t h) {
    ot->width = w;
    ot->height = h;
    ot->format = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = ot->format;
    imgInfo.extent = {w, h, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check_vk_result(vkCreateImage(g_Device, &imgInfo, g_Allocator, &ot->image));

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(g_Device, ot->image, &memReq);
    uint32_t memIdx = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memIdx == UINT32_MAX) {
        std::fprintf(stderr, "[vulkan] Failed to find device local memory for offscreen target\n");
        std::abort();
    }
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memIdx;
    check_vk_result(vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &ot->memory));
    check_vk_result(vkBindImageMemory(g_Device, ot->image, ot->memory, 0));

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = ot->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = ot->format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check_vk_result(vkCreateImageView(g_Device, &viewInfo, g_Allocator, &ot->view));

    VkAttachmentDescription attachment = {};
    attachment.format = ot->format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo = {};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &attachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dep;
    check_vk_result(vkCreateRenderPass(g_Device, &rpInfo, g_Allocator, &ot->renderPass));

    VkFramebufferCreateInfo fbInfo = {};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = ot->renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &ot->view;
    fbInfo.width = w;
    fbInfo.height = h;
    fbInfo.layers = 1;
    check_vk_result(vkCreateFramebuffer(g_Device, &fbInfo, g_Allocator, &ot->framebuffer));

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = g_QueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check_vk_result(vkCreateCommandPool(g_Device, &poolInfo, g_Allocator, &ot->commandPool));

    VkCommandBufferAllocateInfo cmdAlloc = {};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = ot->commandPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    check_vk_result(vkAllocateCommandBuffers(g_Device, &cmdAlloc, &ot->commandBuffer));

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check_vk_result(vkCreateFence(g_Device, &fenceInfo, g_Allocator, &ot->fence));
}

static void DestroyOffscreenTarget(OffscreenTarget* ot) {
    if (ot->fence) vkDestroyFence(g_Device, ot->fence, g_Allocator);
    if (ot->commandPool) vkDestroyCommandPool(g_Device, ot->commandPool, g_Allocator);
    if (ot->framebuffer) vkDestroyFramebuffer(g_Device, ot->framebuffer, g_Allocator);
    if (ot->renderPass) vkDestroyRenderPass(g_Device, ot->renderPass, g_Allocator);
    if (ot->view) vkDestroyImageView(g_Device, ot->view, g_Allocator);
    if (ot->image) vkDestroyImage(g_Device, ot->image, g_Allocator);
    if (ot->memory) vkFreeMemory(g_Device, ot->memory, g_Allocator);
    *ot = OffscreenTarget{};
}

static void OffscreenFrameRender(OffscreenTarget* ot, ImDrawData* draw_data) {
    check_vk_result(vkWaitForFences(g_Device, 1, &ot->fence, VK_TRUE, UINT64_MAX));
    check_vk_result(vkResetFences(g_Device, 1, &ot->fence));
    check_vk_result(vkResetCommandPool(g_Device, ot->commandPool, 0));

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(ot->commandBuffer, &beginInfo));

    VkRenderPassBeginInfo rpBegin = {};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = ot->renderPass;
    rpBegin.framebuffer = ot->framebuffer;
    rpBegin.renderArea.extent = {ot->width, ot->height};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &ot->clearValue;
    vkCmdBeginRenderPass(ot->commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(draw_data, ot->commandBuffer);
    vkCmdEndRenderPass(ot->commandBuffer);
    check_vk_result(vkEndCommandBuffer(ot->commandBuffer));

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &ot->commandBuffer;
    check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, ot->fence));
}

static bool CaptureOffscreenToPng(OffscreenTarget* ot, const std::string& outPath) {
    check_vk_result(vkWaitForFences(g_Device, 1, &ot->fence, VK_TRUE, UINT64_MAX));
    return CaptureVkImageToPng(ot->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               ot->format, ot->width, ot->height, outPath);
}

static void DownscaleRgbaIfNeeded(std::vector<uint8_t>& rgba, int& w, int& h, int maxDim) {
    if (w <= maxDim && h <= maxDim) return;
    double scale = static_cast<double>(maxDim) / static_cast<double>(std::max(w, h));
    int nw = std::max(1, static_cast<int>(std::lround(w * scale)));
    int nh = std::max(1, static_cast<int>(std::lround(h * scale)));
    std::vector<uint8_t> out(static_cast<size_t>(nw) * nh * 4, 255);
    for (int y = 0; y < nh; ++y) {
        float sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(h) / static_cast<float>(nh) - 0.5f;
        int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, h - 1);
        int y1 = std::min(h - 1, y0 + 1);
        float fy = std::clamp(sy - y0, 0.0f, 1.0f);
        for (int x = 0; x < nw; ++x) {
            float sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(w) / static_cast<float>(nw) - 0.5f;
            int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, w - 1);
            int x1 = std::min(w - 1, x0 + 1);
            float fx = std::clamp(sx - x0, 0.0f, 1.0f);
            for (int c = 0; c < 4; ++c) {
                float p00 = rgba[(static_cast<size_t>(y0) * w + x0) * 4 + c];
                float p10 = rgba[(static_cast<size_t>(y0) * w + x1) * 4 + c];
                float p01 = rgba[(static_cast<size_t>(y1) * w + x0) * 4 + c];
                float p11 = rgba[(static_cast<size_t>(y1) * w + x1) * 4 + c];
                float top = p00 + (p10 - p00) * fx;
                float bot = p01 + (p11 - p01) * fx;
                out[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>(std::clamp(top + (bot - top) * fy, 0.0f, 255.0f));
            }
        }
    }
    rgba = std::move(out);
    w = nw;
    h = nh;
}

static bool UploadVulkanTextureFromRgba(const std::vector<uint8_t>& rgbaIn, int w, int h, VulkanTexture& outTex, bool withNearest = false) {
    if (w <= 0 || h <= 0 || rgbaIn.size() < static_cast<size_t>(w) * h * 4) return false;
    std::vector<uint8_t> rgba = rgbaIn;
    DownscaleRgbaIfNeeded(rgba, w, h, 4096);

    QueueDestroyVulkanTexture(outTex);
    VulkanTexture tex;
    VkDeviceSize uploadSize = static_cast<VkDeviceSize>(w) * h * 4;

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent = {(uint32_t)w, (uint32_t)h, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_Device, &imgInfo, g_Allocator, &tex.image) != VK_SUCCESS) {
        DestroyVulkanTextureImmediate(tex);
        return false;
    }

    VkMemoryRequirements imgReq;
    vkGetImageMemoryRequirements(g_Device, tex.image, &imgReq);
    uint32_t imgMemIdx = FindMemoryType(imgReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imgMemIdx == UINT32_MAX) {
        DestroyVulkanTextureImmediate(tex);
        return false;
    }
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = imgReq.size;
    allocInfo.memoryTypeIndex = imgMemIdx;
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &tex.memory) != VK_SUCCESS) {
        DestroyVulkanTextureImmediate(tex);
        return false;
    }
    if (vkBindImageMemory(g_Device, tex.image, tex.memory, 0) != VK_SUCCESS) {
        DestroyVulkanTextureImmediate(tex);
        return false;
    }

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(g_Device, &viewInfo, g_Allocator, &tex.view) != VK_SUCCESS) {
        DestroyVulkanTextureImmediate(tex);
        return false;
    }

    VkSamplerCreateInfo sampInfo = {};
    sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter = VK_FILTER_LINEAR;
    sampInfo.minFilter = VK_FILTER_LINEAR;
    sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.maxAnisotropy = 1.0f;
    if (vkCreateSampler(g_Device, &sampInfo, g_Allocator, &tex.sampler) != VK_SUCCESS) {
        DestroyVulkanTextureImmediate(tex);
        return false;
    }
    if (withNearest) {
        VkSamplerCreateInfo nearInfo = {};
        nearInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        nearInfo.magFilter = VK_FILTER_NEAREST;
        nearInfo.minFilter = VK_FILTER_NEAREST;
        nearInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        nearInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        nearInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        nearInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        nearInfo.maxAnisotropy = 1.0f;
        if (vkCreateSampler(g_Device, &nearInfo, g_Allocator, &tex.samplerNearest) != VK_SUCCESS) {
            DestroyVulkanTextureImmediate(tex);
            return false;
        }
    }

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = uploadSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufInfo, g_Allocator, &stagingBuf) != VK_SUCCESS) {
        DestroyVulkanTextureImmediate(tex);
        return false;
    }

    VkMemoryRequirements bufReq;
    vkGetBufferMemoryRequirements(g_Device, stagingBuf, &bufReq);
    uint32_t bufMemIdx = FindMemoryType(bufReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (bufMemIdx == UINT32_MAX) {
        vkDestroyBuffer(g_Device, stagingBuf, g_Allocator);
        DestroyVulkanTextureImmediate(tex);
        return false;
    }
    allocInfo.allocationSize = bufReq.size;
    allocInfo.memoryTypeIndex = bufMemIdx;
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, stagingBuf, g_Allocator);
        DestroyVulkanTextureImmediate(tex);
        return false;
    }
    vkBindBufferMemory(g_Device, stagingBuf, stagingMem, 0);

    void* mapped = nullptr;
    vkMapMemory(g_Device, stagingMem, 0, uploadSize, 0, &mapped);
    std::memcpy(mapped, rgba.data(), uploadSize);
    vkUnmapMemory(g_Device, stagingMem);

    vkResetFences(g_Device, 1, &g_UploadFence);
    vkResetCommandPool(g_Device, g_UploadPool, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_UploadCmd, &beginInfo);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = tex.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g_UploadCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(g_UploadCmd, stagingBuf, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(g_UploadCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(g_UploadCmd);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_UploadCmd;
    vkQueueSubmit(g_Queue, 1, &submit, g_UploadFence);
    vkWaitForFences(g_Device, 1, &g_UploadFence, VK_TRUE, UINT64_MAX);

    vkDestroyBuffer(g_Device, stagingBuf, g_Allocator);
    vkFreeMemory(g_Device, stagingMem, g_Allocator);

    tex.desc = ImGui_ImplVulkan_AddTexture(tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (withNearest) {
        tex.descNearest = ImGui_ImplVulkan_AddTexture(tex.samplerNearest, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    tex.width = w;
    tex.height = h;
    outTex = tex;
    return true;
}

static bool LoadVulkanTextureFromFile(const std::string& path, VulkanTexture& outTex, int maxDim = 4096, bool withNearest = false) {
    auto loaded = upix::Image::load(path);
    if (!loaded || !loaded->valid()) return false;
    const upix::Image& srcImg = *loaded;
    int w = srcImg.width();
    int h = srcImg.height();
    int ch = srcImg.channels();

    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 255);
    const uint8_t* src = srcImg.data();
    for (int i = 0; i < w * h; ++i) {
        if (ch >= 3) {
            rgba[i * 4 + 0] = src[i * ch + 0];
            rgba[i * 4 + 1] = src[i * ch + 1];
            rgba[i * 4 + 2] = src[i * ch + 2];
            rgba[i * 4 + 3] = (ch == 4) ? src[i * ch + 3] : 255;
        } else if (ch == 1) {
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = src[i];
        }
    }
    DownscaleRgbaIfNeeded(rgba, w, h, maxDim);
    return UploadVulkanTextureFromRgba(rgba, w, h, outTex, withNearest);
}

// Synthetic high-contrast test pattern so Before/After split is unmistakably distinct in --state done.
static void UploadSyntheticStageTexture(bool highRes, VulkanTexture& outTex) {
    constexpr int W = 640;
    constexpr int H = 427;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 255);
    const int block = highRes ? 1 : 6;
    for (int y = 0; y < H; ++y) {
        const int sy = (y / block) * block;
        for (int x = 0; x < W; ++x) {
            const int sx = (x / block) * block;
            float u = static_cast<float>(sx) / static_cast<float>(W);
            float v = static_cast<float>(sy) / static_cast<float>(H);
            // Architectural dark slate + glowing teal/blue wave + crisp grid lines in highRes
            float wave = std::sin(u * 9.0f) * 0.12f + 0.52f;
            float ridge = std::exp(-std::pow((v - wave) * 14.0f, 2.0f));
            float r = 18.0f + 26.0f * u + 40.0f * ridge;
            float g = 24.0f + 46.0f * (1.0f - v) + 125.0f * ridge;
            float b = 34.0f + 85.0f * u + 195.0f * ridge;
            if (highRes) {
                bool grid = (x % 32 == 0) || (y % 32 == 0);
                float dx = static_cast<float>(x) - W * 0.55f;
                float dy = static_cast<float>(y) - H * 0.44f;
                float dist = std::sqrt(dx * dx + dy * dy);
                float ring = std::exp(-std::pow(dist - 92.0f, 2.0f) * 0.18f);
                if (grid) { r += 26.0f; g += 36.0f; b += 48.0f; }
                r += 160.0f * ring;
                g += 190.0f * ring;
                b += 255.0f * ring;
            } else {
                // Deliberately soft / muted before state
                r *= 0.78f;
                g *= 0.78f;
                b *= 0.80f;
            }
            size_t idx = (static_cast<size_t>(y) * W + x) * 4;
            px[idx + 0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
            px[idx + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
            px[idx + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
            px[idx + 3] = 255;
        }
    }
    UploadVulkanTextureFromRgba(px, W, H, outTex, true);
}

static void EnsureStageTextures(UiViewState& ui, const upix::gui::Snapshot& snap) {
    if (snap.inputs.empty() && snap.job.phase == upix::gui::JobPhase::Idle) {
        if (ui.beforeTex.valid()) {
            QueueDestroyVulkanTexture(ui.beforeTex);
            ui.beforeTexPath.clear();
        }
        if (ui.afterTex.valid()) {
            QueueDestroyVulkanTexture(ui.afterTex);
            ui.afterTexPath.clear();
        }
        for (auto& [_, tex] : ui.thumbCache) {
            QueueDestroyVulkanTexture(tex);
        }
        ui.thumbCache.clear();
        ui.thumbLru.clear();
        return;
    }

    std::string targetBefore;
    std::string targetAfter;
    bool allowLastOutputFallback = snap.inputs.empty() || snap.inputs.size() == 1;
    if (!snap.inputs.empty()) {
        int active = std::clamp(snap.active_input, 0, static_cast<int>(snap.inputs.size()) - 1);
        const auto& inp = snap.inputs[active];
        if (inp.is_dir) allowLastOutputFallback = true;
        if (inp.path == "__fixture_before__") {
            targetBefore = "__fixture_before__";
        } else if (!inp.is_dir && fs::exists(inp.path)) {
            targetBefore = inp.path;
        }
        if (!inp.output_path.empty()) {
            if (inp.output_path == "__fixture_after__" || fs::exists(inp.output_path)) {
                targetAfter = inp.output_path;
            }
        }
    }
    if (targetAfter.empty() && allowLastOutputFallback && !snap.job.outputs.empty()) {
        const auto& out = snap.job.outputs.back();
        if (out == "__fixture_after__" || fs::exists(out)) {
            targetAfter = out;
        }
    }
    if (targetBefore.empty()) targetBefore = "__fixture_before__";

    if (targetBefore != ui.beforeTexPath) {
        if (targetBefore == "__fixture_before__") {
            UploadSyntheticStageTexture(false, ui.beforeTex);
            ui.beforeTexPath = targetBefore;
        } else if (LoadVulkanTextureFromFile(targetBefore, ui.beforeTex, 4096, true)) {
            ui.beforeTexPath = targetBefore;
        }
    }
    if (targetAfter.empty()) {
        if (ui.afterTex.valid()) {
            QueueDestroyVulkanTexture(ui.afterTex);
            ui.afterTexPath.clear();
        }
    } else if (targetAfter != ui.afterTexPath) {
        if (targetAfter == "__fixture_after__") {
            UploadSyntheticStageTexture(true, ui.afterTex);
            ui.afterTexPath = targetAfter;
        } else if (LoadVulkanTextureFromFile(targetAfter, ui.afterTex, 4096, true)) {
            ui.afterTexPath = targetAfter;
        }
    }

    // Populate bounded LRU filmstrip cache (up to 24 mini thumbnails)
    for (const auto& inp : snap.inputs) {
        if (inp.is_dir || inp.path.empty() || inp.path.rfind("__", 0) == 0) continue;
        if (ui.thumbCache.find(inp.path) != ui.thumbCache.end()) continue;
        if (!fs::exists(inp.path)) continue;
        VulkanTexture thumb;
        if (LoadVulkanTextureFromFile(inp.path, thumb, 128)) {
            if (ui.thumbLru.size() >= 24) {
                std::string evict = ui.thumbLru.front();
                ui.thumbLru.erase(ui.thumbLru.begin());
                auto it = ui.thumbCache.find(evict);
                if (it != ui.thumbCache.end()) {
                    QueueDestroyVulkanTexture(it->second);
                    ui.thumbCache.erase(it);
                }
            }
            ui.thumbCache[inp.path] = thumb;
            ui.thumbLru.push_back(inp.path);
        }
    }
}

static bool RunAuditMotion(ImGui_ImplVulkanH_Window* wd, OffscreenTarget* ot,
                           upix::gui::AppState& app, UiViewState& ui,
                           std::optional<bool> themeOverride) {
    using namespace upix::ui;
    const char* states[] = {"empty", "default", "running", "done", "error", "models", "host", "pairing", "settings"};
    bool ok = true;

    ImFont* medFont = ui.fontSansMedium ? ui.fontSansMedium : ui.fontSans;
    const char* kTabLabels[4] = {
        upix::i18n::tr(upix::i18n::Key::TabWorkbench),
        upix::i18n::tr(upix::i18n::Key::TabModels),
        upix::i18n::tr(upix::i18n::Key::TabHost),
        upix::i18n::tr(upix::i18n::Key::TabSettings),
    };
    float tabXs[4] = {};
    float tabWs[4] = {};
    bool tabsMeasured = false;
    auto ensureTabsMeasured = [&]() {
        if (tabsMeasured) return;
        tabsMeasured = true;
        constexpr float kWordPx = std::max(18.0f, geist::type::BODY);
        const float wordW = std::ceil(calcText(medFont, kWordPx, "Upix").x);
        const float brandW = 18.0f + geist::space::SM + wordW;
        const float sepX = snapPx(snapPx(geist::space::XL) + brandW + geist::space::XL);
        const float tabsX = snapPx(sepX + hairlinePx() + geist::space::XL);
        float curX = tabsX;
        for (int i = 0; i < 4; ++i) {
            tabWs[i] = std::ceil(calcText(medFont, geist::type::BODY, kTabLabels[i]).x) + 8.0f;
            tabXs[i] = curX;
            curX += tabWs[i] + geist::space::XL;
        }
    };

    for (const char* st : states) {
        motion::resetMotionState();
        app.applyFixture(st);
        if (themeOverride.has_value()) {
            app.setDarkMode(*themeOverride, false);
        }
        if (std::strcmp(st, "models") == 0) ui.activeTab = 1;
        else if (std::strcmp(st, "host") == 0 || std::strcmp(st, "pairing") == 0) ui.activeTab = 2;
        else if (std::strcmp(st, "settings") == 0) ui.activeTab = 3;
        else ui.activeTab = 0;

        float barX0 = 0.0f;
        for (int f = 0; f < 5; ++f) {
            if (ot) {
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2(static_cast<float>(ot->width), static_cast<float>(ot->height));
                io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
                io.DeltaTime = 1.0f / 60.0f;
            } else {
                SDL_PumpEvents();
            }
            ImGui_ImplVulkan_NewFrame();
            if (!ot) ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ensureTabsMeasured();
            ImVec2 winSize = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(winSize);
            ImGui::Begin("##audit_root", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            auto snap = app.snapshot();
            geist::mode() = snap.dark_mode ? geist::Mode::Dark : geist::Mode::Light;
            renderTopBar(dl, winSize, ui, snap);
            if (ui.requestDarkMode >= 0) {
                const bool wantDark = (ui.requestDarkMode != 0);
                app.setDarkMode(wantDark, false);
                geist::mode() = wantDark ? geist::Mode::Dark : geist::Mode::Light;
                ui.requestDarkMode = -1;
            }
            const int tabIdx = std::clamp(ui.activeTab, 0, 3);
            const float targetX = tabXs[tabIdx];
            const float curBarX = ctl::selectionSpring(ImGui::GetID("audit-nav-check"), targetX);
            if (f == 0) barX0 = curBarX;
            else if (std::fabs(curBarX - barX0) > 0.05f) {
                std::fprintf(stderr, "[audit-motion] FAIL state=%s frame=%d tab underline drifted %.2f -> %.2f\n",
                             st, f, barX0, curBarX);
                ok = false;
            }
            ImGui::End();
            ImGui::Render();
            if (ot) {
                OffscreenFrameRender(ot, ImGui::GetDrawData());
            } else {
                FrameRender(wd, ImGui::GetDrawData());
                FramePresent(wd);
            }
            ++g_FrameCounter;
        }
    }

    // Verify topbar tab-strip clicks across 工作台 / 模型 / 设置 inside the 52px header
    {
        motion::resetMotionState();
        app.applyFixture("empty");
        ui.activeTab = 0;
        const int clickTargets[3] = {1, 3, 0};
        for (int targetTab : clickTargets) {
            const float clickX = tabXs[targetTab] + tabWs[targetTab] * 0.5f;
            const float clickY = kTopbarH * 0.5f;
            for (int step = 0; step < 3; ++step) {
                ImGuiIO& io = ImGui::GetIO();
                if (ot) {
                    io.DisplaySize = ImVec2(static_cast<float>(ot->width), static_cast<float>(ot->height));
                    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
                    io.DeltaTime = 1.0f / 60.0f;
                } else {
                    SDL_PumpEvents();
                }
                io.AddMousePosEvent(clickX, clickY);
                if (step == 0) io.AddMouseButtonEvent(0, true);
                else if (step == 1) io.AddMouseButtonEvent(0, false);

                ImGui_ImplVulkan_NewFrame();
                if (!ot) ImGui_ImplSDL3_NewFrame();
                ImGui::NewFrame();
                ImVec2 winSize = ImGui::GetIO().DisplaySize;
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(winSize);
                ImGui::Begin("##audit_root", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                auto snap = app.snapshot();
                geist::mode() = snap.dark_mode ? geist::Mode::Dark : geist::Mode::Light;
                renderTopBar(dl, winSize, ui, snap);
                ImGui::End();
                ImGui::Render();
                if (ot) {
                    OffscreenFrameRender(ot, ImGui::GetDrawData());
                } else {
                    FrameRender(wd, ImGui::GetDrawData());
                    FramePresent(wd);
                }
                ++g_FrameCounter;
            }
            if (ui.activeTab != targetTab) {
                std::fprintf(stderr, "[audit-motion] FAIL topbar click targetTab=%d got activeTab=%d\n",
                             targetTab, ui.activeTab);
                ok = false;
            }
        }
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        motion::resetMotionState();
    }

    // Check reduceMotion instantaneous convergence using deterministic headless ID
    motion::reduceMotion() = true;
    const float r = ctl::selectionSpring(0xA0D17001u, 240.0f);
    if (std::fabs(r - 240.0f) > 0.001f) ok = false;
    motion::reduceMotion() = false;
    std::fprintf(stdout, "[audit-motion] %s (checked %zu fixture states)\n", ok ? "PASS" : "FAIL", std::size(states));
    std::fflush(stdout);
    return ok;
}

static bool ParseSizeArg(const char* s, int& outW, int& outH) {
    if (!s) return false;
    const char* sep = std::strchr(s, 'x');
    if (!sep) sep = std::strchr(s, 'X');
    if (!sep || sep == s || *(sep + 1) == '\0') return false;
    char* endW = nullptr;
    long w = std::strtol(s, &endW, 10);
    if (endW != sep) return false;
    char* endH = nullptr;
    long h = std::strtol(sep + 1, &endH, 10);
    if (!endH || *endH != '\0') return false;
    if (w < 320 || h < 240 || w > 8192 || h > 8192) return false;
    outW = static_cast<int>(w);
    outH = static_cast<int>(h);
    return true;
}

static SDL_Surface* RasterizeUpixIconSurface(int size) {
    SDL_Surface* surf = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return nullptr;

    auto smoothCov = [](float sdf, float aa) -> float {
        return std::clamp(0.5f - sdf / aa, 0.0f, 1.0f);
    };
    auto blendChannel = [](float dst, float src, float a) -> float {
        return dst * (1.0f - a) + src * a;
    };

    const float scale = 108.0f / static_cast<float>(size);
    const float aa = std::max(0.75f * scale, 0.12f);
    constexpr int kSub = 4;
    constexpr float kInvSub = 1.0f / static_cast<float>(kSub);
    constexpr float kInvSub2 = 1.0f / static_cast<float>(kSub * kSub);

    auto sdSeg = [](float px, float py, float ax, float ay, float bx, float by) -> float {
        const float pax = px - ax;
        const float pay = py - ay;
        const float bax = bx - ax;
        const float bay = by - ay;
        const float denom = bax * bax + bay * bay;
        const float h = (denom > 0.0f) ? std::clamp((pax * bax + pay * bay) / denom, 0.0f, 1.0f) : 0.0f;
        return std::hypot(pax - bax * h, pay - bay * h);
    };

    auto sdSquircle = [](float vx, float vy) -> float {
        const float nx = std::fabs(vx - 12.0f) * 0.1f;
        const float ny = std::fabs(vy - 12.0f) * 0.1f;
        if (nx + ny < 1e-4f) return -10.0f;
        constexpr float p = 4.377f;
        const float f = std::pow(nx, p) + std::pow(ny, p);
        const float val = std::pow(f, 1.0f / p);
        const float grad = (val / std::max(f, 1e-12f)) *
                           std::hypot(std::pow(nx, p - 1.0f), std::pow(ny, p - 1.0f));
        return 10.0f * (val - 1.0f) / std::max(grad, 0.25f);
    };

    const float halfBox = 54.0f - 0.5f * scale;
    const float cornerR = std::max(1.0f, 24.0f - 0.5f * scale);

    auto* base = static_cast<uint8_t*>(surf->pixels);
    for (int py = 0; py < size; ++py) {
        uint8_t* row = base + py * surf->pitch;
        for (int px = 0; px < size; ++px) {
            float accR = 0.0f, accG = 0.0f, accB = 0.0f, accA = 0.0f;
            for (int sy = 0; sy < kSub; ++sy) {
                for (int sx = 0; sx < kSub; ++sx) {
                    const float ix = (static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) * kInvSub) * scale;
                    const float iy = (static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) * kInvSub) * scale;

                    // Adaptive-style rounded square (#D4EDEB) in 108x108 icon space
                    const float dx = std::fabs(ix - 54.0f) - (halfBox - cornerR);
                    const float dy = std::fabs(iy - 54.0f) - (halfBox - cornerR);
                    const float dBox = std::hypot(std::max(dx, 0.0f), std::max(dy, 0.0f)) +
                                       std::min(std::max(dx, dy), 0.0f) - cornerR;
                    const float boxCov = smoothCov(dBox, aa);
                    if (boxCov <= 0.0f) continue;

                    // Foreground mark (#1A6B68) in 24x24 space: translate(22.68, 22.68) scale(2.61)
                    const float vx = (ix - 22.68f) / 2.61f;
                    const float vy = (iy - 22.68f) / 2.61f;
                    const float dCenter = std::min({
                        sdSeg(vx, vy, 12.0f, 12.0f, 22.0f, 2.0f),
                        sdSeg(vx, vy, 12.0f, 8.0f, 12.0f, 12.0f),
                        sdSeg(vx, vy, 12.0f, 12.0f, 16.0f, 12.0f),
                        sdSeg(vx, vy, 16.6562f, 2.0f, 22.0f, 2.0f),
                        sdSeg(vx, vy, 22.0f, 2.0f, 22.0f, 7.34375f),
                    });
                    const float dArrow = dCenter - 0.75f;
                    const float dBody = std::max(sdSquircle(vx, vy), 2.25f - dCenter);
                    const float dFg = std::min(dArrow, dBody) * 2.61f;
                    const float fgCov = smoothCov(dFg, aa);

                    const float r = blendChannel(212.0f, 26.0f, fgCov);
                    const float g = blendChannel(237.0f, 107.0f, fgCov);
                    const float b = blendChannel(235.0f, 104.0f, fgCov);

                    accR += r * boxCov;
                    accG += g * boxCov;
                    accB += b * boxCov;
                    accA += boxCov;
                }
            }
            const float a = std::clamp(accA * kInvSub2, 0.0f, 1.0f);
            if (accA > 1e-6f) {
                row[px * 4 + 0] = static_cast<uint8_t>(std::clamp(std::lround(accR / accA), 0L, 255L));
                row[px * 4 + 1] = static_cast<uint8_t>(std::clamp(std::lround(accG / accA), 0L, 255L));
                row[px * 4 + 2] = static_cast<uint8_t>(std::clamp(std::lround(accB / accA), 0L, 255L));
            } else {
                row[px * 4 + 0] = 0;
                row[px * 4 + 1] = 0;
                row[px * 4 + 2] = 0;
            }
            row[px * 4 + 3] = static_cast<uint8_t>(std::clamp(std::lround(a * 255.0f), 0L, 255L));
        }
    }
    return surf;
}

static SDL_Surface* CreateUpixWindowIcon() {
    SDL_Surface* base = RasterizeUpixIconSurface(32);
    if (!base) return nullptr;
    constexpr int kAltSizes[] = {48, 64, 128};
    for (int sz : kAltSizes) {
        if (SDL_Surface* alt = RasterizeUpixIconSurface(sz)) {
            SDL_AddSurfaceAlternateImage(base, alt);
            SDL_DestroySurface(alt);
        }
    }
    return base;
}

static SDL_HitTestResult SDLCALL WindowHitTestCallback(SDL_Window* win, const SDL_Point* area, void* data) {
    auto* ui = static_cast<UiViewState*>(data);
    if (!win || !area || !ui) return SDL_HITTEST_NORMAL;
    if (ui->modalBlockingHitTest) return SDL_HITTEST_NORMAL;

    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    const bool maximized = (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) != 0;

    if (!maximized && w > 0 && h > 0) {
        constexpr int kResizeBorder = 6;
        const bool left = area->x < kResizeBorder;
        const bool right = area->x >= w - kResizeBorder;
        const bool top = area->y < kResizeBorder;
        const bool bottom = area->y >= h - kResizeBorder;
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;
    }

    const float x = static_cast<float>(area->x);
    const float y = static_cast<float>(area->y);
    if (y >= 0.0f && y < kTopbarH &&
        !ui->topbarBrandRect.contains(x, y) &&
        !ui->topbarTabsRect.contains(x, y) &&
        !ui->topbarRightRect.contains(x, y)) {
        return SDL_HITTEST_DRAGGABLE;
    }
    return SDL_HITTEST_NORMAL;
}

} // namespace

int main(int argc, char** argv) {
    if (auto rc = upix::maybe_run_internal_serve_daemon(argc, argv)) {
        return *rc;
    }
    std::string fixtureState;
    std::string shotPath;
    int benchFrames = 0;
    bool auditMotion = false;
    bool forceOffscreen = false;
    std::optional<bool> themeOverride;
    int targetW = 1280;
    int targetH = 840;
    float offscreenScale = 1.0f;
    bool customSize = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--serve-daemon") == 0) {
            upix::ServeDaemonOptions opts;
            opts.models_dir = upix::default_models_dir();
            for (int j = i + 1; j < argc; ++j) {
                std::string_view arg = argv[j];
                if (arg.starts_with("--idle-seconds=")) {
                    opts.idle_timeout_s = std::max(1, std::atoi(std::string(arg.substr(15)).c_str()));
                }
            }
            return upix::run_serve_daemon(opts);
        } else if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            fixtureState = argv[++i];
        } else if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            shotPath = argv[++i];
        } else if (std::strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
            benchFrames = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--audit-motion") == 0) {
            auditMotion = true;
        } else if (std::strcmp(argv[i], "--offscreen") == 0) {
            forceOffscreen = true;
        } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            offscreenScale = std::clamp(std::strtof(argv[++i], nullptr), 0.5f, 4.0f);
        } else if (std::strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            const char* tv = argv[++i];
            if (std::strcmp(tv, "dark") == 0) themeOverride = true;
            else if (std::strcmp(tv, "light") == 0) themeOverride = false;
            else {
                std::fprintf(stderr, "Unknown --theme value '%s' (expected 'dark' or 'light')\n", tv);
                return 1;
            }
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            const char* sv = argv[++i];
            if (!ParseSizeArg(sv, targetW, targetH)) {
                std::fprintf(stderr, "Invalid --size value '%s' (expected WxH, e.g. 1280x840)\n", sv);
                return 1;
            }
            customSize = true;
        }
    }

    const bool offscreen = forceOffscreen || !shotPath.empty() || benchFrames > 0 || auditMotion;
    g_OffscreenMode = offscreen;

    // Temporarily scope VK_LOADER_DRIVERS_SELECT ONLY during GUI Vulkan device init so
    // in-process ncnn discrete-GPU compute inference still sees all Vulkan drivers!
#ifndef _WIN32
    const char* prevDriversEnv = std::getenv("VK_LOADER_DRIVERS_SELECT");
    std::optional<std::string> savedDrivers = prevDriversEnv ? std::optional<std::string>(prevDriversEnv) : std::nullopt;
    setenv("VK_LOADER_LAYERS_DISABLE", "~all~", 0);
    bool scopedIntel = false;
    if (!savedDrivers.has_value() &&
        (fs::exists("/usr/share/vulkan/icd.d/intel_icd.x86_64.json") ||
         fs::exists("/usr/share/vulkan/icd.d/intel_icd.json"))) {
        setenv("VK_LOADER_DRIVERS_SELECT", "intel*", 1);
        scopedIntel = true;
    }
#endif

    SDL_Window* window = nullptr;
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    VkImageUsageFlags extra_usage = 0;

    if (offscreen) {
        SetupVulkan({}, false);
        const uint32_t fbW = static_cast<uint32_t>(std::max<long>(1, std::lround(targetW * offscreenScale)));
        const uint32_t fbH = static_cast<uint32_t>(std::max<long>(1, std::lround(targetH * offscreenScale)));
        CreateOffscreenTarget(&g_Offscreen, fbW, fbH);
    } else {
        SDL_SetAppMetadata("Upix", UPIX_VERSION, "io.upix");
#if defined(__linux__) && !defined(__ANDROID__)
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
#endif
        SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "0");

        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }

        if (!customSize) {
            SDL_Rect ub{};
            const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
            if (primary != 0 && SDL_GetDisplayUsableBounds(primary, &ub) && ub.w > 0 && ub.h > 0) {
                targetW = std::min(targetW, std::max(1040, ub.w - 48));
                targetH = std::min(targetH, std::max(680, ub.h - 48));
            }
        }

        SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_BORDERLESS;
        window = SDL_CreateWindow("Upix", targetW, targetH, window_flags);
        if (!window) {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            return 1;
        }
        SDL_SetWindowMinimumSize(window, 1040, 680);
        if (SDL_Surface* icon = CreateUpixWindowIcon()) {
            if (!SDL_SetWindowIcon(window, icon)) {
                std::fprintf(stderr, "[upix] SDL_SetWindowIcon: %s\n", SDL_GetError());
            }
            SDL_DestroySurface(icon);
        }

        uint32_t sdl_ext_count = 0;
        const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
        std::vector<const char*> extensions(sdl_exts, sdl_exts + sdl_ext_count);
        SetupVulkan(extensions, true);

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window, g_Instance, g_Allocator, &surface)) {
            std::fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
            return 1;
        }

        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        SetupVulkanWindow(wd, surface, w, h, extra_usage);
    }

#ifndef _WIN32
    if (scopedIntel) {
        if (savedDrivers.has_value()) setenv("VK_LOADER_DRIVERS_SELECT", savedDrivers->c_str(), 1);
        else unsetenv("VK_LOADER_DRIVERS_SELECT");
    }
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.Colors[ImGuiCol_NavCursor] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    UiViewState ui;
    ui.showWindowControls = !offscreen || !shotPath.empty();
    if (!offscreen && window) {
        if (!SDL_SetWindowHitTest(window, WindowHitTestCallback, &ui)) {
            std::fprintf(stderr, "[upix] SDL_SetWindowHitTest: %s\n", SDL_GetError());
        }
    }
    auto loadFontZeroCopy = [&](const char* path, float size, bool merge = false, int fontNo = 0) -> ImFont* {
        ImFontConfig cfg;
        cfg.MergeMode = merge;
        cfg.FontNo = static_cast<ImU32>(fontNo);
        cfg.PixelSnapH = false;
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
#ifndef _WIN32
        int fd = ::open(path, O_RDONLY);
        if (fd >= 0) {
            struct stat st{};
            if (::fstat(fd, &st) == 0 && st.st_size > 0) {
                size_t mapSz = static_cast<size_t>(st.st_size);
                void* mapped = ::mmap(nullptr, mapSz, PROT_READ, MAP_PRIVATE, fd, 0);
                ::close(fd);
                if (mapped != MAP_FAILED) {
                    ui.mappedFonts.push_back({mapped, mapSz});
                    cfg.FontDataOwnedByAtlas = false;
                    return io.Fonts->AddFontFromMemoryTTF(mapped, static_cast<int>(mapSz), size, &cfg);
                }
            } else {
                ::close(fd);
            }
        }
#endif
        return io.Fonts->AddFontFromFileTTF(path, size, &cfg);
    };

    struct CjkCandidate {
        const char* path;
        int fontNo;
    };
    const char* latinSansPaths[] = {
        "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
        "/usr/share/fonts/adobe-source-sans/SourceSans3-Regular.otf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/HelveticaNeue.ttc",
        "C:\\Windows\\Fonts\\segoeui.ttf",
    };
    const CjkCandidate cjkFontPaths[] = {
        {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 2},
        {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2},
        {"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", 2},
        {"/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc", 2},
        {"/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc", 2},
        {"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", 0},
        {"/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc", 0},
        {"/System/Library/Fonts/PingFang.ttc", 0},
        {"/System/Library/Fonts/STHeiti Light.ttc", 0},
        {"/System/Library/Fonts/Hiragino Sans GB.ttc", 0},
        {"C:\\Windows\\Fonts\\msyh.ttc", 0},
        {"C:\\Windows\\Fonts\\simsun.ttc", 0},
    };
    CjkCandidate foundCjk = {nullptr, 0};
    for (const auto& cand : cjkFontPaths) {
        if (fs::exists(cand.path)) {
            foundCjk = cand;
            break;
        }
    }
    for (const char* path : latinSansPaths) {
        if (fs::exists(path)) {
            ui.fontSans = loadFontZeroCopy(path, 18.0f, false, 0);
            if (ui.fontSans) {
                if (foundCjk.path) {
                    loadFontZeroCopy(foundCjk.path, 18.0f, true, foundCjk.fontNo);
                }
                break;
            }
        }
    }
    if (!ui.fontSans && foundCjk.path) {
        ui.fontSans = loadFontZeroCopy(foundCjk.path, 18.0f, false, foundCjk.fontNo);
    }
    if (!ui.fontSans) ui.fontSans = io.Fonts->AddFontDefault();

    const CjkCandidate latinSansMediumPaths[] = {
        {"/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf", (5 << 16)},
        {"/usr/share/fonts/adobe-source-sans/SourceSans3-Medium.otf", 0},
        {"/usr/share/fonts/adobe-source-sans/SourceSans3-Semibold.otf", 0},
        {"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", 0},
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 0},
        {"/System/Library/Fonts/SFNS.ttf", 0},
        {"/System/Library/Fonts/HelveticaNeue.ttc", 0},
        {"C:\\Windows\\Fonts\\seguisb.ttf", 0},
    };
    const CjkCandidate cjkMediumPaths[] = {
        {"/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc", 2},
        {"/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc", 2},
        {"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Medium.ttc", 2},
        {"/System/Library/Fonts/PingFang.ttc", 1},
        {"/System/Library/Fonts/STHeiti Medium.ttc", 0},
        {"C:\\Windows\\Fonts\\msyhbd.ttc", 0},
    };
    CjkCandidate foundCjkMedium = foundCjk;
    for (const auto& cand : cjkMediumPaths) {
        if (fs::exists(cand.path)) {
            foundCjkMedium = cand;
            break;
        }
    }
    for (const auto& cand : latinSansMediumPaths) {
        if (fs::exists(cand.path)) {
            ui.fontSansMedium = loadFontZeroCopy(cand.path, 18.0f, false, cand.fontNo);
            if (ui.fontSansMedium) {
                if (foundCjkMedium.path) {
                    loadFontZeroCopy(foundCjkMedium.path, 18.0f, true, foundCjkMedium.fontNo);
                }
                break;
            }
        }
    }
    if (!ui.fontSansMedium) ui.fontSansMedium = ui.fontSans;

    const char* monoFontPaths[] = {
        "/usr/share/fonts/Adwaita/AdwaitaMono-Regular.ttf",
        "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
        "/usr/share/fonts/TTF/JetBrainsMono-Medium.ttf",
        "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "C:\\Windows\\Fonts\\consola.ttf"
    };
    for (const char* path : monoFontPaths) {
        if (fs::exists(path)) {
            ui.fontMono = loadFontZeroCopy(path, 15.0f, false, 0);
            if (ui.fontMono) {
                if (foundCjk.path) {
                    loadFontZeroCopy(foundCjk.path, 15.0f, true, foundCjk.fontNo);
                }
                break;
            }
        }
    }
    if (!ui.fontMono) ui.fontMono = ui.fontSans;
    io.FontDefault = ui.fontSans;

    if (!offscreen) {
        ImGui_ImplSDL3_InitForVulkan(window);
    }
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.PipelineCache = g_PipelineCache;
    init_info.DescriptorPoolSize = 192;
    init_info.MinImageCount = offscreen ? 2u : g_MinImageCount;
    init_info.ImageCount = offscreen ? 2u : wd->ImageCount;
    init_info.Allocator = g_Allocator;
    init_info.PipelineInfoMain.RenderPass = offscreen ? g_Offscreen.renderPass : wd->RenderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    if ((!fixtureState.empty() || auditMotion) && !std::getenv("UPIX_FAKE")) {
#ifdef _WIN32
        _putenv_s("UPIX_FAKE", "1");
#else
        setenv("UPIX_FAKE", "1", 1);
#endif
    }
    upix::gui::AppState app;

    if (!fixtureState.empty()) {
        std::string resolvedState = fixtureState;
        if (resolvedState.rfind("light_", 0) == 0) {
            resolvedState = resolvedState.substr(6);
            if (!themeOverride.has_value()) themeOverride = false;
        }
        app.applyFixture(resolvedState);
        if (resolvedState == "models") ui.activeTab = 1;
        else if (resolvedState == "host" || resolvedState == "host-idle" || resolvedState == "host_idle" || resolvedState == "pairing") ui.activeTab = 2;
        else if (resolvedState == "settings") ui.activeTab = 3;
        else if (resolvedState == "picker") ui.showModelPicker = true;
    }
    if (themeOverride.has_value()) {
        app.setDarkMode(*themeOverride, false);
    }
    {
        auto initSnap = app.snapshot();
        upix::i18n::set_preference(initSnap.language);
        upix::ui::geist::mode() = initSnap.dark_mode ? upix::ui::geist::Mode::Dark : upix::ui::geist::Mode::Light;
        upix::ui::motion::reduceMotion() = initSnap.reduce_motion;
    }

    if (auditMotion) {
        bool pass = RunAuditMotion(wd, offscreen ? &g_Offscreen : nullptr, app, ui, themeOverride);
        vkDeviceWaitIdle(g_Device);
        FlushDestroyQueue(true);
        ImGui_ImplVulkan_Shutdown();
        if (!offscreen) ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        if (offscreen) DestroyOffscreenTarget(&g_Offscreen);
        else CleanupVulkanWindow();
        CleanupVulkan();
        if (!offscreen) {
            SDL_DestroyWindow(window);
            SDL_Quit();
        }
#ifndef _WIN32
        for (const auto& mf : ui.mappedFonts) ::munmap(mf.ptr, mf.size);
#endif
        return pass ? 0 : 1;
    }

    auto dlgAlive = std::make_shared<std::atomic<bool>>(true);
    DialogBridge dlgFiles{&app, 0, dlgAlive, false};
    DialogBridge dlgFolder{&app, 1, dlgAlive, false};
    DialogBridge dlgOutDir{&app, 2, dlgAlive, false};
    DialogBridge dlgImport{&app, 3, dlgAlive, false};

    if (offscreen) {
        std::fprintf(stdout, "[upix] Offscreen Vulkan initialized (%dx%d)\n", targetW, targetH);
    } else {
        std::fprintf(stdout, "[upix] SDL3 driver: %s | Vulkan device initialized\n", SDL_GetCurrentVideoDriver());
    }
    std::fflush(stdout);

    bool done = false;
    bool shotOk = true;
    int redrawFrames = 4;
    int renderedCount = 0;
    const int benchWarmup = 10;
    std::vector<double> benchFrameMs;
    std::chrono::steady_clock::time_point benchT0;
#ifndef _WIN32
    double benchCpuMs0 = 0.0;
#endif
    double benchPrevMs = 0.0;
    if (benchFrames > 0) redrawFrames = 1 << 30;

    while (!done) {
        bool stateChanged = app.pollEvents();
        if (stateChanged) redrawFrames = std::max(redrawFrames, 2);

        if (!offscreen) {
            SDL_Event event;
            bool gotEvent = false;
            if (redrawFrames > 0 || !shotPath.empty()) {
                gotEvent = SDL_PollEvent(&event);
            } else {
                gotEvent = SDL_WaitEventTimeout(&event, 100);
            }

            while (gotEvent) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                redrawFrames = std::max(redrawFrames, 2);

                if (event.type == SDL_EVENT_QUIT)
                    done = true;
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))
                    done = true;
                if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                    app.addPaths({event.drop.data});
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && window &&
                    event.button.windowID == SDL_GetWindowID(window) && !ui.modalBlockingHitTest) {
                    const float mx = event.button.x;
                    const float my = event.button.y;
                    const bool inDragBar = (my >= 0.0f && my < kTopbarH &&
                                            !ui.topbarBrandRect.contains(mx, my) &&
                                            !ui.topbarTabsRect.contains(mx, my) &&
                                            !ui.topbarRightRect.contains(mx, my));
                    if (inDragBar) {
                        if (event.button.button == SDL_BUTTON_RIGHT) {
                            SDL_ShowWindowSystemMenu(window, static_cast<int>(mx), static_cast<int>(my));
                        } else if (event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2) {
                            ui.requestWindowAction = 2;
                        }
                    }
                }
                if (event.type == SDL_EVENT_KEY_DOWN) {
#ifdef __APPLE__
                    const bool ctrl = (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
#else
                    const bool ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
#endif
                    if (ctrl && event.key.key == SDLK_PERIOD) {
                        app.cancelJob();
                    } else if (ctrl && event.key.key == SDLK_O && !io.WantTextInput && ui.activeTab == 0) {
                        ui.requestOpenFiles = true;
                    } else if (ctrl && event.key.key == SDLK_V && !io.WantTextInput) {
                        bool pasted = false;
                        if (SDL_HasClipboardText()) {
                            char* clip = SDL_GetClipboardText();
                            if (clip) {
                                pasted = app.addFromClipboardText(clip);
                                SDL_free(clip);
                            }
                        }
                        if (!pasted && SDL_HasClipboardData("image/png")) {
                            size_t sz = 0;
                            void* raw = SDL_GetClipboardData("image/png", &sz);
                            if (raw && sz > 0) {
                                std::vector<uint8_t> bytes(static_cast<const uint8_t*>(raw),
                                                           static_cast<const uint8_t*>(raw) + sz);
                                SDL_free(raw);
                                if (auto img = upix::Image::load_from_memory("png", bytes); img && img->valid()) {
                                    int wImg = img->width(), hImg = img->height(), ch = img->channels();
                                    std::vector<uint8_t> rgba(static_cast<size_t>(wImg) * hImg * 4, 255);
                                    const uint8_t* src = img->data();
                                    for (int i = 0; i < wImg * hImg; ++i) {
                                        if (ch >= 3) {
                                            rgba[i * 4 + 0] = src[i * ch + 0];
                                            rgba[i * 4 + 1] = src[i * ch + 1];
                                            rgba[i * 4 + 2] = src[i * ch + 2];
                                            rgba[i * 4 + 3] = (ch == 4) ? src[i * ch + 3] : 255;
                                        } else {
                                            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = src[i];
                                        }
                                    }
                                    pasted = app.addFromClipboardRgba(wImg, hImg, rgba.data(), rgba.size());
                                }
                            }
                        }
                        if (!pasted) {
                            app.setNotice(upix::i18n::tr(upix::i18n::Key::NoticeClipboardEmpty));
                        }
                    } else if (event.key.key == SDLK_ESCAPE) {
                        auto cur = app.snapshot();
                        if (!cur.host.pending_device.empty()) {
                            app.hostDecide(false);
                        } else if (ui.showModelPicker) {
                            ui.showModelPicker = false;
                        } else if (ui.showIpPicker) {
                            ui.showIpPicker = false;
                        } else if (!cur.notice.empty()) {
                            app.clearNotice();
                        } else if (cur.job.phase == upix::gui::JobPhase::Running) {
                            app.cancelJob();
                        }
                    } else if (ctrl && event.key.key == SDLK_1) {
                        ui.activeTab = 0;
                    } else if (ctrl && event.key.key == SDLK_2) {
                        ui.activeTab = 1;
                    } else if (ctrl && event.key.key == SDLK_3) {
                        ui.activeTab = 2;
                    } else if (ctrl && event.key.key == SDLK_4) {
                        ui.activeTab = 3;
                    }
                }
                gotEvent = SDL_PollEvent(&event);
            }

            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(16);
                continue;
            }

            int fb_width = 0, fb_height = 0;
            SDL_GetWindowSizeInPixels(window, &fb_width, &fb_height);
            if (fb_width > 0 && fb_height > 0 && (g_SwapChainRebuild || wd->Width != fb_width || wd->Height != fb_height)) {
                ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
                ImGui_ImplVulkanH_CreateOrResizeWindow(
                    g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily,
                    g_Allocator, fb_width, fb_height, g_MinImageCount, extra_usage);
                wd->FrameIndex = 0;
                g_SwapChainRebuild = false;
            }
        } else {
            io.DisplaySize = ImVec2(static_cast<float>(targetW), static_cast<float>(targetH));
            io.DisplayFramebufferScale = ImVec2(offscreenScale, offscreenScale);
            io.DeltaTime = 1.0f / 60.0f;
        }

        FlushDestroyQueue(false);

        ImGui_ImplVulkan_NewFrame();
        if (!offscreen) {
            ImGui_ImplSDL3_NewFrame();
        }

        if (benchFrames > 0 && renderedCount == benchWarmup) {
            benchT0 = std::chrono::steady_clock::now();
#ifndef _WIN32
            struct rusage ru{};
            getrusage(RUSAGE_SELF, &ru);
            benchCpuMs0 = (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 +
                          (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
#endif
        }

        const float fbScale = (io.DisplayFramebufferScale.x > 0.0f) ? io.DisplayFramebufferScale.x : 1.0f;
        float rasterScale = fbScale;
        if (!offscreen && window) {
            const float pixelDensity = SDL_GetWindowPixelDensity(window);
            if (pixelDensity > 0.0f && std::fabs(fbScale - pixelDensity) < 0.02f) {
                rasterScale = pixelDensity;
            }
        }
        upix::ui::currentDpiScale() = fbScale;
        if (ui.fontSans) ui.fontSans->CurrentRasterizerDensity = rasterScale;
        if (ui.fontSansMedium) ui.fontSansMedium->CurrentRasterizerDensity = rasterScale;
        if (ui.fontMono) ui.fontMono->CurrentRasterizerDensity = rasterScale;

        auto snap = app.snapshot();
        upix::ui::geist::mode() = snap.dark_mode ? upix::ui::geist::Mode::Dark : upix::ui::geist::Mode::Light;
        upix::ui::motion::reduceMotion() = snap.reduce_motion;
        upix::ui::motion::beginMotionFrame();
        dlgFolder.recursive.store(snap.recursive, std::memory_order_release);

        ImGui::NewFrame();
        // We already snap coordinates to physical pixels via snapPx()/snapPt();
        // ImGui's default IM_TRUNC() in RenderText() destroys sub-logical physical
        // alignment at fractional DPI (e.g. 1.25x/1.5x) and blurs glyphs.
        ImGui::GetDrawListSharedData()->InitialFlags |= ImDrawListFlags_TextNoPixelSnap;

        ImVec2 winSize = io.DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(winSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##upix_root", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoBackground);

        if (io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) {
            ImGui::SetFontRasterizerDensity(rasterScale);
        }
        if (ui.fontSans) ui.fontSans->CurrentRasterizerDensity = rasterScale;
        if (ui.fontSansMedium) ui.fontSansMedium->CurrentRasterizerDensity = rasterScale;
        if (ui.fontMono) ui.fontMono->CurrentRasterizerDensity = rasterScale;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->Flags |= ImDrawListFlags_TextNoPixelSnap;
        ImGui::GetForegroundDrawList()->Flags |= ImDrawListFlags_TextNoPixelSnap;
        ImGui::GetBackgroundDrawList()->Flags |= ImDrawListFlags_TextNoPixelSnap;
        const auto& gt = upix::ui::geist::tokens();
        dl->AddRectFilled(ImVec2(0, 0), winSize, gt.bg);

        if (snap.job.phase == upix::gui::JobPhase::Running || snap.job.phase == upix::gui::JobPhase::Cancelling) {
            redrawFrames = std::max(redrawFrames, 2);
        }

        EnsureStageTextures(ui, snap);

        ui.windowMaximized = !offscreen && window &&
                             ((SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0);
        renderTopBar(dl, winSize, ui, snap);
        if (!offscreen && window && ui.requestWindowAction != 0) {
            if (ui.requestWindowAction == 1) {
                SDL_MinimizeWindow(window);
            } else if (ui.requestWindowAction == 2) {
                if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) {
                    SDL_RestoreWindow(window);
                } else {
                    SDL_MaximizeWindow(window);
                }
            } else if (ui.requestWindowAction == 3) {
                done = true;
            }
            ui.requestWindowAction = 0;
            redrawFrames = std::max(redrawFrames, 2);
        }
        if (ui.requestDarkMode >= 0) {
            const bool wantDark = (ui.requestDarkMode != 0);
            app.setDarkMode(wantDark);
            upix::ui::geist::mode() = wantDark ? upix::ui::geist::Mode::Dark : upix::ui::geist::Mode::Light;
            snap.dark_mode = wantDark;
            redrawFrames = std::max(redrawFrames, 2);
            ui.requestDarkMode = -1;
        }
        if (ui.activeTab == 0) {
            renderWorkbench(dl, winSize, ui, app, snap, window, &dlgFiles, &dlgFolder, &dlgOutDir);
        } else if (ui.activeTab == 1) {
            renderModelsPage(dl, winSize, ui, app, snap, window, &dlgImport);
        } else if (ui.activeTab == 2) {
            renderRemotePage(dl, winSize, ui, app, snap);
        } else if (ui.activeTab == 3) {
            renderSettingsPage(dl, winSize, ui, app, snap);
        }
        renderPairingModal(dl, winSize, ui, app, snap);

        if (ui.showWindowControls && !ui.windowMaximized) {
            ImGui::GetForegroundDrawList()->AddRect(
                ImVec2(0.0f, 0.0f), winSize, gt.outlineStrong, 0.0f, 0, upix::ui::hairlinePx());
        }

        ImGui::End();
        ImGui::PopStyleVar(2);

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const ImVec4 clearVec = ImGui::ColorConvertU32ToFloat4(gt.bg);
        if (offscreen) {
            g_Offscreen.clearValue.color.float32[0] = clearVec.x;
            g_Offscreen.clearValue.color.float32[1] = clearVec.y;
            g_Offscreen.clearValue.color.float32[2] = clearVec.z;
            g_Offscreen.clearValue.color.float32[3] = 1.0f;
            OffscreenFrameRender(&g_Offscreen, draw_data);
        } else {
            wd->ClearValue.color.float32[0] = clearVec.x;
            wd->ClearValue.color.float32[1] = clearVec.y;
            wd->ClearValue.color.float32[2] = clearVec.z;
            wd->ClearValue.color.float32[3] = 1.0f;
            FrameRender(wd, draw_data);
        }
        ++g_FrameCounter;

#ifndef _WIN32
        if (renderedCount == 2) {
            for (const auto& mf : ui.mappedFonts) {
                ::madvise(mf.ptr, mf.size, MADV_DONTNEED);
            }
        }
#endif

        ++renderedCount;
        if (!shotPath.empty() && renderedCount >= 3) {
            if (offscreen) {
                shotOk = CaptureOffscreenToPng(&g_Offscreen, shotPath);
            } else {
                shotOk = CaptureSwapchainToPng(wd, shotPath);
                FramePresent(wd);
            }
            if (!shotOk) {
                std::fprintf(stderr, "[upix] Failed to save screenshot to '%s'\n", shotPath.c_str());
            }
            break;
        }

        if (!offscreen) {
            FramePresent(wd);
        }
        if (benchFrames > 0 && renderedCount >= benchWarmup) {
            double totalMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - benchT0).count();
            benchFrameMs.push_back(totalMs - benchPrevMs);
            benchPrevMs = totalMs;
            if ((int)benchFrameMs.size() >= benchFrames) {
                std::vector<double> kept;
                kept.reserve(benchFrameMs.size());
                size_t dropped = 0;
                for (double v : benchFrameMs) {
                    if (v < 0.0 || v > 1000.0) ++dropped;
                    else kept.push_back(v);
                }
                std::sort(kept.begin(), kept.end());
                double sum = 0.0;
                for (double v : kept) sum += v;
                const size_t n = std::max<size_t>(1, kept.size());
                auto pct = [&](double q) { return kept.empty() ? 0.0 : kept[std::min((size_t)(q * (n - 1)), n - 1)]; };
                double cpuMs = 0.0;
#ifndef _WIN32
                struct rusage ru{};
                getrusage(RUSAGE_SELF, &ru);
                double cpuMs1 = (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 +
                                (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
                cpuMs = (cpuMs1 - benchCpuMs0) / static_cast<double>(n);
#endif
                std::fprintf(stdout,
                             "[bench] frames=%zu (dropped %zu) wall ms min/avg/p95/max = %.2f/%.2f/%.2f/%.2f | total %.1f s | cpu %.2f ms/frame | fps %.1f\n",
                             benchFrameMs.size(), dropped, kept.empty() ? 0.0 : kept.front(),
                             sum / static_cast<double>(n), pct(0.95), kept.empty() ? 0.0 : kept.back(),
                             totalMs / 1000.0, cpuMs, 1000.0 / std::max(0.001, sum / static_cast<double>(n)));
                std::fflush(stdout);
                done = true;
            }
        }
        if (redrawFrames > 0) --redrawFrames;
        if (upix::ui::motion::hasActiveMotion()) {
            redrawFrames = std::max(redrawFrames, 2);
        }
    }

    dlgAlive->store(false, std::memory_order_release);
    vkDeviceWaitIdle(g_Device);
    DestroyVulkanTextureImmediate(ui.beforeTex);
    DestroyVulkanTextureImmediate(ui.afterTex);
    for (auto& [_, t] : ui.thumbCache) {
        DestroyVulkanTextureImmediate(t);
    }
    ui.thumbCache.clear();
    FlushDestroyQueue(true);

    ImGui_ImplVulkan_Shutdown();
    if (!offscreen) {
        ImGui_ImplSDL3_Shutdown();
    }
    ImGui::DestroyContext();

    if (offscreen) {
        DestroyOffscreenTarget(&g_Offscreen);
    } else {
        CleanupVulkanWindow();
    }
    CleanupVulkan();
    if (!offscreen) {
        SDL_DestroyWindow(window);
        SDL_Quit();
    }

#ifndef _WIN32
    for (const auto& mf : ui.mappedFonts) {
        ::munmap(mf.ptr, mf.size);
    }
    ui.mappedFonts.clear();
#endif
    return shotOk ? 0 : 1;
}
