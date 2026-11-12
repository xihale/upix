#include "app_state.hpp"
#include "views/common.hpp"

using upix::views::DialogBridge;
using upix::views::MmapFontRegion;
using upix::views::UiViewState;
using upix::views::VulkanTexture;
using upix::views::drawMiniThumb;
using upix::views::drawStageArtwork;
using upix::views::renderNavRail;
using upix::views::renderWorkbench;
using upix::views::renderModelsPage;
using upix::views::renderRemotePage;
using upix::views::renderPairingModal;
#include "ui_theme.hpp"
#include "ui_widgets.hpp"

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
#include <format>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Minimal SDL3 + Vulkan boilerplate for Dear ImGui 1.92
// ---------------------------------------------------------------------------
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
    return 0;
}

// Select integrated GPU first for the lightweight 2D GUI window so hybrid laptops
// don't spin up a 70MB+ discrete GPU driver context just for 2D UI rendering.
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

static void CleanupVulkan() {
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

// Capture current swapchain backbuffer to PNG via upix::Image
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
    check_vk_result(vkCreateBuffer(g_Device, &bufInfo, g_Allocator, &stagingBuf));

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(g_Device, stagingBuf, &memReq);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check_vk_result(vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &stagingMem));
    check_vk_result(vkBindBufferMemory(g_Device, stagingBuf, stagingMem, 0));

    vkResetCommandPool(g_Device, fd->CommandPool, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(fd->CommandBuffer, &beginInfo);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.image = fd->Backbuffer;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(fd->CommandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(fd->CommandBuffer, fd->Backbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(fd->CommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(fd->CommandBuffer);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &fd->CommandBuffer;
    vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_Queue);

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

// ---------------------------------------------------------------------------
// Native File Dialog Callbacks (SDL3)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Vulkan Image Texture Loader (for Stage preview & thumbnails)
static void DestroyVulkanTexture(VulkanTexture& tex) {
    if (!tex.valid()) return;
    vkDeviceWaitIdle(g_Device);
    ImGui_ImplVulkan_RemoveTexture(tex.desc);
    if (tex.sampler) vkDestroySampler(g_Device, tex.sampler, g_Allocator);
    if (tex.view) vkDestroyImageView(g_Device, tex.view, g_Allocator);
    if (tex.image) vkDestroyImage(g_Device, tex.image, g_Allocator);
    if (tex.memory) vkFreeMemory(g_Device, tex.memory, g_Allocator);
    tex = VulkanTexture{};
}

static bool LoadVulkanTextureFromFile(ImGui_ImplVulkanH_Window* wd, const std::string& path, VulkanTexture& outTex) {
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

    DestroyVulkanTexture(outTex);
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
    check_vk_result(vkCreateImage(g_Device, &imgInfo, g_Allocator, &outTex.image));

    VkMemoryRequirements imgReq;
    vkGetImageMemoryRequirements(g_Device, outTex.image, &imgReq);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = imgReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(imgReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check_vk_result(vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &outTex.memory));
    check_vk_result(vkBindImageMemory(g_Device, outTex.image, outTex.memory, 0));

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outTex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check_vk_result(vkCreateImageView(g_Device, &viewInfo, g_Allocator, &outTex.view));

    VkSamplerCreateInfo sampInfo = {};
    sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter = VK_FILTER_LINEAR;
    sampInfo.minFilter = VK_FILTER_LINEAR;
    sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.maxAnisotropy = 1.0f;
    check_vk_result(vkCreateSampler(g_Device, &sampInfo, g_Allocator, &outTex.sampler));

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = uploadSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check_vk_result(vkCreateBuffer(g_Device, &bufInfo, g_Allocator, &stagingBuf));

    VkMemoryRequirements bufReq;
    vkGetBufferMemoryRequirements(g_Device, stagingBuf, &bufReq);
    allocInfo.allocationSize = bufReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(bufReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check_vk_result(vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &stagingMem));
    check_vk_result(vkBindBufferMemory(g_Device, stagingBuf, stagingMem, 0));

    void* mapped = nullptr;
    vkMapMemory(g_Device, stagingMem, 0, uploadSize, 0, &mapped);
    std::memcpy(mapped, rgba.data(), uploadSize);
    vkUnmapMemory(g_Device, stagingMem);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[0];
    vkResetCommandPool(g_Device, fd->CommandPool, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(fd->CommandBuffer, &beginInfo);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = outTex.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(fd->CommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(fd->CommandBuffer, stagingBuf, outTex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(fd->CommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(fd->CommandBuffer);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &fd->CommandBuffer;
    vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_Queue);

    vkDestroyBuffer(g_Device, stagingBuf, g_Allocator);
    vkFreeMemory(g_Device, stagingMem, g_Allocator);

    outTex.desc = ImGui_ImplVulkan_AddTexture(outTex.sampler, outTex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    outTex.width = w;
    outTex.height = h;
    return true;
}

// ---------------------------------------------------------------------------
} // namespace

int main(int argc, char** argv) {
    // 1. Subprocess mode: warm daemon (`upix --serve-daemon`) runs headless without GUI init
    std::string fixtureState;
    std::string shotPath;
    int benchFrames = 0;
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
        }
    }

    // 2. Keep Vulkan loader ultra-lean for the 2D GUI:
    //    - Disable implicit 3rd-party Vulkan layers (e.g. MangoHud/LSFG) unless user set it.
    //    - On hybrid Linux laptops, avoid loading unused Mesa LLVM / dGPU shader compiler ICDs (~95MB RSS!)
    //      into the 2D GUI window process when an Intel/AMD iGPU ICD is present.
#ifndef _WIN32
    setenv("VK_LOADER_LAYERS_DISABLE", "~all~", 0);
    if (fs::exists("/usr/share/vulkan/icd.d/intel_icd.x86_64.json") ||
        fs::exists("/usr/share/vulkan/icd.d/intel_icd.json")) {
        setenv("VK_LOADER_DRIVERS_SELECT", "intel*", 0);
    }
#endif

    // 3. Prefer native Wayland on Linux with fallback to X11
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

    // 4. Setup Vulkan
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
    VkImageUsageFlags extra_usage = shotPath.empty() ? 0 : VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    SetupVulkanWindow(wd, surface, w, h, extra_usage);

    // 5. Setup Dear ImGui 1.92 (with zero-copy mmap + LightHinting + HiDPI RasterizerDensity)
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

    // Prefer NotoSansCJK-Medium.ttc (FontNo=2 for Simplified Chinese) matching Figma's font-weight: 500!
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
    init_info.DescriptorPoolSize = 64;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.Allocator = g_Allocator;
    init_info.PipelineInfoMain.RenderPass = wd->RenderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    // 6. Initialize Upix application controller
    upix::gui::AppState app;
    if (!fixtureState.empty()) {
        app.applyFixture(fixtureState);
        if (fixtureState == "models") ui.activeTab = 1;
        else if (fixtureState == "host" || fixtureState == "host-idle" || fixtureState == "pairing") ui.activeTab = 2;
        else if (fixtureState == "picker") ui.showModelPicker = true;
    }

    DialogBridge dlgFiles{&app, 0};
    DialogBridge dlgFolder{&app, 1};
    DialogBridge dlgOutDir{&app, 2};
    DialogBridge dlgImport{&app, 3};

    std::fprintf(stdout, "[upix] SDL3 driver: %s | Vulkan device initialized\n", SDL_GetCurrentVideoDriver());
    std::fflush(stdout);

    // 7. Event-driven main loop
    bool done = false;
    int redrawFrames = 4;
    int renderedCount = 0;
    const int benchWarmup = 10;
    std::vector<double> benchFrameMs;
    std::chrono::steady_clock::time_point benchT0;
    double benchCpuMs0 = 0.0;
    double benchPrevMs = 0.0;
    if (benchFrames > 0) redrawFrames = 1 << 30; // render continuously

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
                bool ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
                if (ctrl && event.key.key == SDLK_PERIOD) {
                    app.cancelJob();
                } else if (event.key.key == SDLK_ESCAPE) {
                    app.hostDecide(false);
                    ui.showModelPicker = false;
                } else if (ctrl && event.key.key == SDLK_1) {
                    ui.activeTab = 0;
                } else if (ctrl && event.key.key == SDLK_2) {
                    ui.activeTab = 1;
                } else if (ctrl && event.key.key == SDLK_3) {
                    ui.activeTab = 2;
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

        // Update Wayland HiDPI rasterizer density & physical pixel snap grid every frame!
        float dpiScale = (io.DisplayFramebufferScale.x > 0.0f) ? io.DisplayFramebufferScale.x : 1.0f;
        upix::ui::currentDpiScale() = dpiScale;
        if (ui.fontSans) ui.fontSans->CurrentRasterizerDensity = dpiScale;
        if (ui.fontMono) ui.fontMono->CurrentRasterizerDensity = dpiScale;

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
        dl->AddRectFilled(ImVec2(0, 0), winSize, ui.pal.bg());

        auto snap = app.snapshot();
        if (snap.job.phase == upix::gui::JobPhase::Running) {
            redrawFrames = std::max(redrawFrames, 2);
        }

        // Lazy-load stage texture only when inputs exist
        if (snap.inputs.empty() && snap.job.phase == upix::gui::JobPhase::Idle) {
            if (ui.stageTex.valid()) {
                DestroyVulkanTexture(ui.stageTex);
                ui.stageTexPath.clear();
            }
        } else {
            std::string targetImg;
            if (!snap.inputs.empty() && !snap.inputs.front().is_dir && fs::exists(snap.inputs.front().path)) {
                targetImg = snap.inputs.front().path;
            }
#ifdef FIXTURE_DIR
            if (targetImg.empty()) {
                std::string demo = std::string(FIXTURE_DIR) + "/demo_stage.png";
                if (fs::exists(demo)) targetImg = demo;
            }
#endif
            if (!targetImg.empty() && targetImg != ui.stageTexPath) {
                if (LoadVulkanTextureFromFile(wd, targetImg, ui.stageTex)) {
                    ui.stageTexPath = targetImg;
                }
            }
        }

        renderNavRail(dl, winSize, ui, snap);
        if (ui.activeTab == 0) {
            renderWorkbench(dl, winSize, ui, app, snap, window, &dlgFiles, &dlgFolder, &dlgOutDir);
        } else if (ui.activeTab == 1) {
            renderModelsPage(dl, winSize, ui, app, snap, window, &dlgImport);
        } else if (ui.activeTab == 2) {
            renderRemotePage(dl, winSize, ui, app, snap);
        }
        renderPairingModal(dl, winSize, ui, app, snap);

        ImGui::End();
        ImGui::PopStyleVar(2);

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        wd->ClearValue.color.float32[0] = 0.075f;
        wd->ClearValue.color.float32[1] = 0.075f;
        wd->ClearValue.color.float32[2] = 0.086f;
        wd->ClearValue.color.float32[3] = 1.0f;

        FrameRender(wd, draw_data);

#ifndef _WIN32
        // Evict file-backed CJK font pages after FreeType rasterizes initial glyphs
        // so the 19MB TTC file doesn't sit in RSS needlessly.
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
                // Drop clock-glitch / compositor-stall outliers (v < 0 or > 1 s)
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
                const size_t n = kept.size();
                auto pct = [&](double q) { return kept[std::min((size_t)(q * (n - 1)), n - 1)]; };
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
                             benchFrameMs.size(), dropped, kept.front(), sum / static_cast<double>(kept.size()), pct(0.95), kept.back(),
                             totalMs / 1000.0, cpuMs, 1000.0 / (sum / static_cast<double>(n)));
                std::fflush(stdout);
                done = true;
            }
        }
        if (redrawFrames > 0) --redrawFrames;
    }

    vkDeviceWaitIdle(g_Device);
    DestroyVulkanTexture(ui.stageTex);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    CleanupVulkanWindow();
    CleanupVulkan();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
