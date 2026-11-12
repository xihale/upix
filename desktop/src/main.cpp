#include "app_state.hpp"
#include "views/common.hpp"

#include "imgui.h"
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
using upix::views::renderGuidePage;
using upix::views::renderModelsPage;
using upix::views::renderNavRail;
using upix::views::renderPairingModal;
using upix::views::renderRemotePage;
using upix::views::renderSettingsPage;
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

static void SetupVulkan(std::vector<const char*> instance_extensions) {
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
    create_info.ppEnabledExtensionNames = instance_extensions.data();
    err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
    check_vk_result(err);

    g_PhysicalDevice = SelectGuiPhysicalDevice(g_Instance);
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);

    std::vector<const char*> device_extensions;
    device_extensions.push_back("VK_KHR_swapchain");

    uint32_t dev_props_count = 0;
    vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &dev_props_count, nullptr);
    std::vector<VkExtensionProperties> dev_props(dev_props_count);
    vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &dev_props_count, dev_props.data());
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
    if (IsExtensionAvailable(dev_props, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
        device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

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
    dev_info.ppEnabledExtensionNames = device_extensions.data();
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
    if (tex.sampler) vkDestroySampler(g_Device, tex.sampler, g_Allocator);
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
    const uint64_t lag = static_cast<uint64_t>(std::max<uint32_t>(2u, g_MainWindowData.ImageCount)) + 1;
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

static bool CaptureSwapchainToPng(ImGui_ImplVulkanH_Window* wd, const std::string& outPath) {
    vkDeviceWaitIdle(g_Device);
    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    uint32_t w = (uint32_t)wd->Width;
    uint32_t h = (uint32_t)wd->Height;
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
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.image = fd->Backbuffer;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g_UploadCmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(g_UploadCmd, fd->Backbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
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
    bool isBgra = (wd->SurfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM || wd->SurfaceFormat.format == VK_FORMAT_B8G8R8A8_SRGB);
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

    auto saved = img.save(outPath);
    return saved.has_value();
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

static bool UploadVulkanTextureFromRgba(const std::vector<uint8_t>& rgbaIn, int w, int h, VulkanTexture& outTex) {
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
    tex.width = w;
    tex.height = h;
    outTex = tex;
    return true;
}

static bool LoadVulkanTextureFromFile(const std::string& path, VulkanTexture& outTex, int maxDim = 4096) {
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
    return UploadVulkanTextureFromRgba(rgba, w, h, outTex);
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
    UploadVulkanTextureFromRgba(px, W, H, outTex);
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
        return;
    }

    std::string targetBefore;
    std::string targetAfter;
    if (!snap.inputs.empty()) {
        int active = std::clamp(snap.active_input, 0, static_cast<int>(snap.inputs.size()) - 1);
        const auto& inp = snap.inputs[active];
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
    if (targetAfter.empty() && !snap.job.outputs.empty()) {
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
        } else if (LoadVulkanTextureFromFile(targetBefore, ui.beforeTex)) {
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
        } else if (LoadVulkanTextureFromFile(targetAfter, ui.afterTex)) {
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

static bool RunAuditMotion(ImGui_ImplVulkanH_Window* wd, upix::gui::AppState& app, UiViewState& ui) {
    using namespace upix::ui;
    const char* states[] = {"empty", "default", "running", "done", "error", "models", "host", "pairing", "settings", "guide"};
    bool ok = true;
    for (const char* st : states) {
        motion::resetMotionState();
        app.applyFixture(st);
        if (std::strcmp(st, "models") == 0) ui.activeTab = 1;
        else if (std::strcmp(st, "host") == 0 || std::strcmp(st, "pairing") == 0) ui.activeTab = 2;
        else if (std::strcmp(st, "guide") == 0) ui.activeTab = 3;
        else if (std::strcmp(st, "settings") == 0) ui.activeTab = 4;
        else ui.activeTab = 0;

        float pillY0 = 0.0f;
        for (int f = 0; f < 5; ++f) {
            SDL_PumpEvents();
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ImVec2 winSize = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(winSize);
            ImGui::Begin("##audit_root", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            auto snap = app.snapshot();
            renderNavRail(dl, winSize, ui, snap);
            const float targetY = (ui.activeTab < 3)
                ? (16.0f + ui.activeTab * 62.0f)
                : (winSize.y - 16.0f - (snap.version.empty() ? 0.0f : (calcText(ui.fontSans, 10.0f, snap.version.c_str()).y + 10.0f)) - 122.0f + (ui.activeTab - 3) * 62.0f);
            const float curPillY = ctl::selectionSpring(ImGui::GetID("audit-nav-check"), targetY);
            if (f == 0) pillY0 = curPillY;
            else if (std::fabs(curPillY - pillY0) > 0.05f) {
                std::fprintf(stderr, "[audit-motion] FAIL state=%s frame=%d pill drifted %.2f -> %.2f\n",
                             st, f, pillY0, curPillY);
                ok = false;
            }
            ImGui::End();
            ImGui::Render();
            FrameRender(wd, ImGui::GetDrawData());
            FramePresent(wd);
            ++g_FrameCounter;
        }
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

} // namespace

int main(int argc, char** argv) {
    std::string fixtureState;
    std::string shotPath;
    int benchFrames = 0;
    bool auditMotion = false;
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
        }
    }

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

    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "0");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window* window = SDL_CreateWindow("Upix", 1280, 840, window_flags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 1040, 680);

    uint32_t sdl_ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    std::vector<const char*> extensions(sdl_exts, sdl_exts + sdl_ext_count);
    SetupVulkan(extensions);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, g_Instance, g_Allocator, &surface)) {
        std::fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return 1;
    }

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    VkImageUsageFlags extra_usage = (shotPath.empty() && !auditMotion) ? 0 : VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    SetupVulkanWindow(wd, surface, w, h, extra_usage);

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

    UiViewState ui;
    auto loadFontZeroCopy = [&](const char* path, float size, bool merge = false, int fontNo = 0) -> ImFont* {
        ImFontConfig cfg;
        cfg.MergeMode = merge;
        cfg.FontNo = fontNo;
        cfg.PixelSnapH = true;
        cfg.OversampleH = 2;
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
    const CjkCandidate cjkFontPaths[] = {
        {"/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc", 2},
        {"/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc", 2},
        {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 2},
        {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2},
        {"/usr/share/fonts/TTF/DejaVuSans.ttf", 0},
        {"C:\\Windows\\Fonts\\msyh.ttc", 0},
    };
    CjkCandidate foundCjk = {nullptr, 0};
    for (const auto& cand : cjkFontPaths) {
        if (fs::exists(cand.path)) {
            ui.fontSans = loadFontZeroCopy(cand.path, 14.0f, false, cand.fontNo);
            if (ui.fontSans) {
                foundCjk = cand;
                break;
            }
        }
    }
    if (!ui.fontSans) ui.fontSans = io.Fonts->AddFontDefault();

    const char* monoFontPaths[] = {
        "/usr/share/fonts/TTF/JetBrainsMono-Medium.ttf",
        "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
        "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Medium.ttf",
        "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "C:\\Windows\\Fonts\\consola.ttf"
    };
    for (const char* path : monoFontPaths) {
        if (fs::exists(path)) {
            ui.fontMono = loadFontZeroCopy(path, 13.0f, false, 0);
            if (ui.fontMono) {
                if (foundCjk.path) {
                    loadFontZeroCopy(foundCjk.path, 13.0f, true, foundCjk.fontNo);
                }
                break;
            }
        }
    }
    if (!ui.fontMono) ui.fontMono = ui.fontSans;

    ImGui_ImplSDL3_InitForVulkan(window);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.PipelineCache = g_PipelineCache;
    init_info.DescriptorPoolSize = 96;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.Allocator = g_Allocator;
    init_info.PipelineInfoMain.RenderPass = wd->RenderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    upix::gui::AppState app;
    {
        auto initSnap = app.snapshot();
        upix::ui::geist::mode() = initSnap.dark_mode ? upix::ui::geist::Mode::Dark : upix::ui::geist::Mode::Light;
        upix::ui::motion::reduceMotion() = initSnap.reduce_motion;
    }

    if (!fixtureState.empty()) {
        app.applyFixture(fixtureState);
        if (fixtureState == "models") ui.activeTab = 1;
        else if (fixtureState == "host" || fixtureState == "host-idle" || fixtureState == "pairing") ui.activeTab = 2;
        else if (fixtureState == "guide") ui.activeTab = 3;
        else if (fixtureState == "settings") ui.activeTab = 4;
        else if (fixtureState == "picker") ui.showModelPicker = true;
    }

    if (auditMotion) {
        bool pass = RunAuditMotion(wd, app, ui);
        vkDeviceWaitIdle(g_Device);
        FlushDestroyQueue(true);
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        CleanupVulkanWindow();
        CleanupVulkan();
        SDL_DestroyWindow(window);
        SDL_Quit();
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

    std::fprintf(stdout, "[upix] SDL3 driver: %s | Vulkan device initialized\n", SDL_GetCurrentVideoDriver());
    std::fflush(stdout);

    bool done = false;
    int redrawFrames = 4;
    int renderedCount = 0;
    const int benchWarmup = 10;
    std::vector<double> benchFrameMs;
    std::chrono::steady_clock::time_point benchT0;
    double benchCpuMs0 = 0.0;
    double benchPrevMs = 0.0;
    if (benchFrames > 0) redrawFrames = 1 << 30;

    while (!done) {
        bool stateChanged = app.pollEvents();
        if (stateChanged) redrawFrames = std::max(redrawFrames, 2);

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
            if (event.type == SDL_EVENT_KEY_DOWN) {
                const bool ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
                if (ctrl && event.key.key == SDLK_PERIOD) {
                    app.cancelJob();
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
                        app.setNotice("剪贴板里没有图片或有效文件路径");
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
                } else if (ctrl && event.key.key == SDLK_5) {
                    ui.activeTab = 4;
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

        FlushDestroyQueue(false);

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();

        if (benchFrames > 0 && renderedCount == benchWarmup) {
            benchT0 = std::chrono::steady_clock::now();
#ifndef _WIN32
            struct rusage ru{};
            getrusage(RUSAGE_SELF, &ru);
            benchCpuMs0 = (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 +
                          (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
#endif
        }

        float dpiScale = (io.DisplayFramebufferScale.x > 0.0f) ? io.DisplayFramebufferScale.x : 1.0f;
        upix::ui::currentDpiScale() = dpiScale;
        if (ui.fontSans) ui.fontSans->CurrentRasterizerDensity = dpiScale;
        if (ui.fontMono) ui.fontMono->CurrentRasterizerDensity = dpiScale;

        auto snap = app.snapshot();
        upix::ui::geist::mode() = snap.dark_mode ? upix::ui::geist::Mode::Dark : upix::ui::geist::Mode::Light;
        upix::ui::motion::reduceMotion() = snap.reduce_motion;
        dlgFolder.recursive = snap.recursive;

        ImGui::NewFrame();

        ImVec2 winSize = io.DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(winSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##upix_root", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoBackground);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const auto& gt = upix::ui::geist::tokens();
        dl->AddRectFilled(ImVec2(0, 0), winSize, gt.bg);

        if (snap.job.phase == upix::gui::JobPhase::Running || snap.job.phase == upix::gui::JobPhase::Cancelling) {
            redrawFrames = std::max(redrawFrames, 2);
        }

        EnsureStageTextures(ui, snap);

        renderNavRail(dl, winSize, ui, snap);
        if (ui.activeTab == 0) {
            renderWorkbench(dl, winSize, ui, app, snap, window, &dlgFiles, &dlgFolder, &dlgOutDir);
        } else if (ui.activeTab == 1) {
            renderModelsPage(dl, winSize, ui, app, snap, window, &dlgImport);
        } else if (ui.activeTab == 2) {
            renderRemotePage(dl, winSize, ui, app, snap);
        } else if (ui.activeTab == 3) {
            renderGuidePage(dl, winSize, ui, snap);
        } else if (ui.activeTab == 4) {
            renderSettingsPage(dl, winSize, ui, app, snap);
        }
        renderPairingModal(dl, winSize, ui, app, snap);

        ImGui::End();
        ImGui::PopStyleVar(2);

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const ImVec4 clearVec = ImGui::ColorConvertU32ToFloat4(gt.bg);
        wd->ClearValue.color.float32[0] = clearVec.x;
        wd->ClearValue.color.float32[1] = clearVec.y;
        wd->ClearValue.color.float32[2] = clearVec.z;
        wd->ClearValue.color.float32[3] = 1.0f;

        FrameRender(wd, draw_data);
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
            CaptureSwapchainToPng(wd, shotPath);
            FramePresent(wd);
            break;
        }

        FramePresent(wd);
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
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    CleanupVulkanWindow();
    CleanupVulkan();
    SDL_DestroyWindow(window);
    SDL_Quit();

#ifndef _WIN32
    for (const auto& mf : ui.mappedFonts) {
        ::munmap(mf.ptr, mf.size);
    }
    ui.mappedFonts.clear();
#endif
    return 0;
}
