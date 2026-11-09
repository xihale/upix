#include "app_state.hpp"
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
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
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
struct DialogBridge {
    upix::gui::AppState* app = nullptr;
    int mode = 0; // 0 = input files, 1 = input folder, 2 = output folder, 3 = import models
};

static void SDLCALL OnFileDialog(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* bridge = static_cast<DialogBridge*>(userdata);
    if (!bridge || !bridge->app || !filelist) return;
    std::vector<std::string> paths;
    for (int i = 0; filelist[i] != nullptr; ++i) {
        if (filelist[i][0] != '\0') paths.emplace_back(filelist[i]);
    }
    if (paths.empty()) return;
    if (bridge->mode == 0 || bridge->mode == 1) {
        bridge->app->addPaths(paths);
    } else if (bridge->mode == 2) {
        bridge->app->setCustomOutDir(paths[0]);
    } else if (bridge->mode == 3) {
        bridge->app->importModels(paths);
    }
}

// ---------------------------------------------------------------------------
// Vulkan Image Texture Loader (for Stage preview & thumbnails)
// ---------------------------------------------------------------------------
struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet desc = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;

    bool valid() const { return desc != VK_NULL_HANDLE; }
};

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
// UI Controller State
// ---------------------------------------------------------------------------
struct MmapFontRegion {
    void* ptr = nullptr;
    size_t size = 0;
};

struct UiViewState {
    int activeTab = 0; // 0 = 放大 (Stage), 1 = 模型 (Models), 2 = 远程 (Host)
    float splitRatio = 0.545f; // Figma 71:1129 uses 480 / 880 = 0.545f
    bool showModelPicker = false;
    std::string modelsSceneFilter = "all";
    upix::ui::Palette pal{true};
    ImFont* fontSans = nullptr;
    ImFont* fontMono = nullptr;
    VulkanTexture stageTex;
    std::string stageTexPath;
    std::vector<MmapFontRegion> mappedFonts;
};

static void drawStageArtwork(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool highRes, const UiViewState& ui) {
    using namespace upix::ui;
    if (ui.stageTex.valid()) {
        ImU32 tint = highRes ? rgba(255, 255, 255, 255) : rgba(228, 228, 232, 242);
        dl->AddImage((ImTextureID)(intptr_t)ui.stageTex.desc, p0, p1, ImVec2(0, 0), ImVec2(1, 1), tint);
        return;
    }
    dl->AddRectFilledMultiColor(p0, p1, hex(0x0E1B1E), hex(0x13262A), hex(0x161E24), hex(0x0D1217));
}

static void drawMiniThumb(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const UiViewState& ui, float radius = 4.0f, size_t cropIdx = 0) {
    using namespace upix::ui;
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    if (ui.stageTex.valid()) {
        // Slightly vary crop window per index so filmstrip slots look like distinct shots when using a single preview texture
        float ox = (cropIdx % 3) * 0.08f;
        float oy = ((cropIdx / 2) % 2) * 0.08f;
        ImVec2 uv0(0.10f + ox, 0.05f + oy);
        ImVec2 uv1(0.74f + ox, 0.85f + oy);
        dl->AddImageRounded((ImTextureID)(intptr_t)ui.stageTex.desc, p0, p1, uv0, uv1, rgba(255, 255, 255, 255), radius);
    } else {
        dl->AddRectFilled(p0, p1, hex(0x121216), radius);
    }
}

// ---------------------------------------------------------------------------
// 1. Navigation Rail (76px fixed width) — Figma "Desktop / Navigation rail"
// ---------------------------------------------------------------------------
static void renderNavRail(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& pal = ui.pal;
    float railW = 76.0f;

    dl->AddRectFilled(ImVec2(0, 0), ImVec2(railW, winSize.y), pal.bg());

    struct NavItem {
        const char* label;
        const char* icon;
        int idx;
    };
    const NavItem items[3] = {
        {"放大", "zoom-in", 0},
        {"模型", "layers", 1},
        {"远程", "smartphone", 2},
    };

    // Figma: padding-top 12px, item 60x60, gap 2px, active fill #26262c (NO border), radius 10px, icon 20x20, label 11px
    float startY = 12.0f;
    for (int i = 0; i < 3; ++i) {
        ImVec2 pos = snapPt(8.0f, startY + i * (60.0f + 2.0f));
        ImVec2 sz(60.0f, 60.0f);
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##nav", sz);
        bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();

        if (clicked) ui.activeTab = items[i].idx;
        bool active = (ui.activeTab == items[i].idx);

        if (active) {
            drawBox(dl, pos, ImVec2(pos.x + sz.x, pos.y + sz.y), pal.high(), 0, 10.0f);
        } else if (hovered) {
            drawBox(dl, pos, ImVec2(pos.x + sz.x, pos.y + sz.y), pal.containerLow(), 0, 10.0f);
        }

        ImU32 iconCol = active ? pal.primary() : (hovered ? pal.onSurface() : pal.muted());
        ImU32 textCol = active ? pal.onSurface() : (hovered ? pal.onSurface() : pal.muted());
        drawIcon(dl, items[i].icon, ImVec2(pos.x + sz.x * 0.5f, pos.y + 21.0f), 20.0f, iconCol);
        drawTextCentered(dl, ui.fontSans, 11.0f, ImVec2(pos.x, pos.y + 34.0f), ImVec2(pos.x + sz.x, pos.y + 52.0f), textCol, items[i].label);
    }

    // Bottom version label ("0.2.0" — Figma: 10px #a2a2ac, bottom padding 16px)
    drawTextCentered(dl, ui.fontSans, 10.0f, ImVec2(0, winSize.y - 30.0f), ImVec2(railW, winSize.y - 16.0f), pal.muted(), snap.version.c_str());
}

// ---------------------------------------------------------------------------
// 2. Workbench Stage + Inspector (Tab 0: 放大)
// ---------------------------------------------------------------------------
static void renderWorkbench(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                            upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                            SDL_Window* window, DialogBridge* dlgFiles, DialogBridge* dlgFolder, DialogBridge* dlgOutDir) {
    using namespace upix::ui;
    const auto& pal = ui.pal;

    float railW = 76.0f;
    float inspW = 308.0f;
    float inspX = winSize.x - inspW;
    float stageAreaX = railW;
    float stageAreaW = inspX - railW;

    bool hasInputs = !snap.inputs.empty() || snap.job.phase != upix::gui::JobPhase::Idle;
    bool hasDock = snap.inputs.size() > 1 || snap.job.phase == upix::gui::JobPhase::Running || snap.job.phase == upix::gui::JobPhase::Error;

    float padX = 8.0f;
    float padY = 16.0f;
    float dockBarH = 54.0f;
    float dockH = hasDock ? (dockBarH + 10.0f) : 0.0f;
    ImVec2 stageMin = snapPt(stageAreaX + padX, padY);
    ImVec2 stageMax = snapPt(stageAreaX + stageAreaW - padX, winSize.y - padY - dockH);

    // Stage Well container (Figma 4:18 & 71:1133: background #18181c, border 1px solid #282830, radius 12px)
    ImU32 stageBg = (snap.job.phase == upix::gui::JobPhase::Running) ? pal.recessed() : pal.surface();
    drawBox(dl, stageMin, stageMax, stageBg, pal.outline(), 12.0f);

    if (!hasInputs) {
        // ---- Empty State (Figma Frame 4:2) ----
        // Registration marks: inset 28px, 16x2px, #343440
        drawRegistrationMarks(dl, stageMin, stageMax, 28.0f, 16.0f, 2.0f, pal.regMark());

        ImVec2 center = snapPt((stageMin.x + stageMax.x) * 0.5f, (stageMin.y + stageMax.y) * 0.5f);

        // Figma 7:6: top 352px (center - 52px), 32x32 zoom-in icon, gap 14px, 15px weight 500 text #a2a2ac
        drawIcon(dl, "zoom-in", ImVec2(center.x, center.y - 36.0f), 32.0f, pal.muted(), 1.8f);
        drawTextCentered(dl, ui.fontSans, 15.0f,
                         ImVec2(stageMin.x, center.y - 6.0f), ImVec2(stageMax.x, center.y + 20.0f),
                         pal.muted(), "拖入图片，或按 Ctrl+V 粘贴");

        // Figma 12:366: 162x32px button at top:732px (44px above stage bottom), #1c1c21 fill, #2d2d34 border, radius 8px, 12px #a2a2ac
        ImVec2 folderBtnSz(162.0f, 32.0f);
        ImVec2 btnPos = snapPt(center.x - folderBtnSz.x * 0.5f, stageMax.y - 44.0f - folderBtnSz.y);
        if (pillButton(dl, "##pick_folder", btnPos, folderBtnSz, "选择文件夹批量处理", "folder-open", pal, ui.fontSans, false, 8.0f, 12.0f, true)) {
            SDL_ShowOpenFolderDialog(OnFileDialog, dlgFolder, window, nullptr, false);
        }

        // Click on the rest of the stage opens file picker
        ImGui::SetCursorScreenPos(stageMin);
        if (ImGui::InvisibleButton("##stage_empty_click", ImVec2(stageMax.x - stageMin.x, btnPos.y - stageMin.y - 8.0f))) {
            SDL_ShowOpenFileDialog(OnFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
        }
    } else {
        // ---- Active Stage (Compare / Running / Error) ----
        float telemH = 40.0f;
        ImVec2 viewMin = snapPt(stageMin.x + 1.0f, stageMin.y + 1.0f);
        ImVec2 viewMax = snapPt(stageMax.x - 1.0f, stageMax.y - telemH);

        dl->PushClipRect(viewMin, viewMax, true);

        bool isCompare = (snap.job.phase == upix::gui::JobPhase::Done);
        if (isCompare) {
            float splitX = snapPx(viewMin.x + (viewMax.x - viewMin.x) * ui.splitRatio);

            // Left: Before (1x)
            dl->PushClipRect(viewMin, ImVec2(splitX, viewMax.y), true);
            drawStageArtwork(dl, viewMin, viewMax, false, ui);
            dl->PopClipRect();

            // Right: After (4x)
            dl->PushClipRect(ImVec2(splitX, viewMin.y), viewMax, true);
            drawStageArtwork(dl, viewMin, viewMax, true, ui);
            dl->PopClipRect();

            // 1px vertical split line (#ececeee5) + 28x28 rounded square handle (Figma Frame 71:1129)
            dl->AddRectFilled(ImVec2(splitX, viewMin.y), ImVec2(splitX + hairlinePx(), viewMax.y), pal.withAlpha(pal.onSurface(), 0.90f));
            ImVec2 hCenter = snapPt(splitX, (viewMin.y + viewMax.y) * 0.5f);
            ImVec2 h0(hCenter.x - 14.0f, hCenter.y - 14.0f);
            ImVec2 h1(hCenter.x + 14.0f, hCenter.y + 14.0f);
            drawBox(dl, h0, h1, rgba(20, 20, 24, 215), hex(0x2C2C35), 7.0f);
            ImVec2 cL[3] = {ImVec2(hCenter.x - 3.0f, hCenter.y - 3.5f), ImVec2(hCenter.x - 6.5f, hCenter.y), ImVec2(hCenter.x - 3.0f, hCenter.y + 3.5f)};
            ImVec2 cR[3] = {ImVec2(hCenter.x + 3.0f, hCenter.y - 3.5f), ImVec2(hCenter.x + 6.5f, hCenter.y), ImVec2(hCenter.x + 3.0f, hCenter.y + 3.5f)};
            dl->AddPolyline(cL, 3, pal.muted(), ImDrawFlags_None, 1.4f);
            dl->AddPolyline(cR, 3, pal.muted(), ImDrawFlags_None, 1.4f);

            ImGui::SetCursorScreenPos(ImVec2(splitX - 16.0f, viewMin.y));
            ImGui::InvisibleButton("##split_drag", ImVec2(32.0f, viewMax.y - viewMin.y));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsItemActive()) {
                ui.splitRatio = std::clamp((ImGui::GetIO().MousePos.x - viewMin.x) / (viewMax.x - viewMin.x), 0.05f, 0.95f);
            }

            // Optical HUD chips (Figma 75:2993 & 75:2995: 77x26px, #141418c7, border #2c2c35, radius 6px, 11px JetBrains Mono)
            ImVec2 chipL0 = snapPt(viewMin.x + 14.0f, viewMin.y + 14.0f);
            drawBox(dl, chipL0, ImVec2(chipL0.x + 77.0f, chipL0.y + 26.0f), rgba(20, 20, 24, 199), hex(0x2C2C35), 6.0f);
            drawTextCentered(dl, ui.fontMono, 11.0f, chipL0, ImVec2(chipL0.x + 77.0f, chipL0.y + 26.0f), pal.muted(), "原图 · 1×");

            ImVec2 close0 = snapPt(viewMax.x - 14.0f - 26.0f, viewMin.y + 14.0f);
            ImVec2 chipR0 = snapPt(close0.x - 10.0f - 77.0f, viewMin.y + 14.0f);
            drawBox(dl, chipR0, ImVec2(chipR0.x + 77.0f, chipR0.y + 26.0f), rgba(20, 20, 24, 199), hex(0x2C2C35), 6.0f);
            drawTextCentered(dl, ui.fontMono, 11.0f, chipR0, ImVec2(chipR0.x + 77.0f, chipR0.y + 26.0f), pal.onSurface(), "超分 · 4×");

            ImGui::SetCursorScreenPos(close0);
            if (ImGui::InvisibleButton("##close_stage_done", ImVec2(26.0f, 26.0f))) {
                app.clearInputs();
            }
            drawBox(dl, close0, ImVec2(close0.x + 26.0f, close0.y + 26.0f), rgba(20, 20, 24, 199), hex(0x2C2C35), 6.0f);
            drawIcon(dl, "x", ImVec2(close0.x + 13.0f, close0.y + 13.0f), 12.0f, pal.muted());
        } else {
            // Running / Idle / Error plate
            drawStageArtwork(dl, viewMin, viewMax, false, ui);

            if (snap.job.phase == upix::gui::JobPhase::Running) {
                float scanY = snapPx(viewMin.y + (viewMax.y - viewMin.y) * (snap.job.percent / 100.0f));
                dl->AddRectFilled(ImVec2(viewMin.x, scanY), ImVec2(viewMax.x, scanY + hairlinePx()), pal.withAlpha(pal.primary(), 0.55f));
            }

            ImVec2 close0 = snapPt(viewMax.x - 14.0f - 28.0f, viewMin.y + 14.0f);
            ImGui::SetCursorScreenPos(close0);
            if (ImGui::InvisibleButton("##close_stage_run", ImVec2(28.0f, 28.0f))) {
                app.clearInputs();
            }
            drawBox(dl, close0, ImVec2(close0.x + 28.0f, close0.y + 28.0f), rgba(20, 20, 24, 199), hex(0x2C2C35), 6.0f);
            drawIcon(dl, "x", ImVec2(close0.x + 14.0f, close0.y + 14.0f), 13.0f, pal.muted());
        }
        dl->PopClipRect();

        // ---- Bottom 40px Stage Telemetry Bar (Figma 83:3540 & 83:3528: #18181c, border #26262e) ----
        ImVec2 bar0 = snapPt(stageMin.x + 1.0f, stageMax.y - telemH);
        ImVec2 bar1 = snapPt(stageMax.x - 1.0f, stageMax.y - 1.0f);
        dl->AddRectFilled(bar0, bar1, pal.surface(), 11.0f, ImDrawFlags_RoundCornersBottom);
        dl->AddRectFilled(ImVec2(stageMin.x + 1.0f, bar0.y), ImVec2(stageMax.x - 1.0f, bar0.y + hairlinePx()), hex(0x26262E));

        float cy = snapPx((bar0.y + bar1.y) * 0.5f);

        if (snap.job.phase == upix::gui::JobPhase::Running) {
            dl->AddCircleFilled(ImVec2(bar0.x + 18.0f, cy), 3.0f, pal.primary());

            drawText(dl, ui.fontMono, 12.0f, ImVec2(bar0.x + 30.0f, cy - 7.5f), pal.onSurface(), snap.job.current_file.c_str());
            ImVec2 csz = calcText(ui.fontMono, 12.0f, snap.job.current_file.c_str());

            std::string tileMeta = std::format("分块 {}/{} · 剩余 ~0.3s",
                                               snap.job.tiles_done > 0 ? snap.job.tiles_done : 18,
                                               snap.job.tiles_total > 0 ? snap.job.tiles_total : 24);
            drawText(dl, ui.fontMono, 11.0f, ImVec2(bar0.x + 42.0f + csz.x, cy - 7.0f), pal.dim(), tileMeta.c_str());

            // Right: 148x4px inline progress track (#121216) + "75%" (11px #7fdac9)
            std::string pctStr = std::format("{}%", snap.job.percent);
            ImVec2 psz = calcText(ui.fontMono, 11.0f, pctStr.c_str());
            drawText(dl, ui.fontMono, 11.0f, ImVec2(bar1.x - 14.0f - psz.x, cy - 7.0f), pal.primary(), pctStr.c_str());

            float trkW = 148.0f;
            float trkX = snapPx(bar1.x - 24.0f - psz.x - trkW);
            dl->AddRectFilled(ImVec2(trkX, cy - 2.0f), ImVec2(trkX + trkW, cy + 2.0f), hex(0x121216), 2.0f);
            dl->AddRectFilled(ImVec2(trkX, cy - 2.0f), ImVec2(trkX + trkW * (snap.job.percent / 100.0f), cy + 2.0f), pal.primary(), 2.0f);
        } else if (snap.job.phase == upix::gui::JobPhase::Done) {
            dl->AddCircleFilled(ImVec2(bar0.x + 19.0f, cy), 3.0f, pal.primary());

            ImVec2 t0 = snapPt(bar0.x + 32.0f, cy - 10.0f);
            ImVec2 t1 = snapPt(t0.x + 58.0f, cy + 10.0f);
            drawBox(dl, t0, t1, pal.primaryDimBg(), pal.primaryDimBorder(), 4.0f);
            drawTextCentered(dl, ui.fontMono, 10.0f, t0, t1, pal.primary(), "4× · PNG");

            std::string resStr = std::format("{}×{} → {}×{}", snap.job.in_w, snap.job.in_h, snap.job.out_w, snap.job.out_h);
            drawText(dl, ui.fontMono, 12.0f, ImVec2(t1.x + 10.0f, cy - 7.5f), pal.onSurface(), resStr.c_str());
            ImVec2 rsz = calcText(ui.fontMono, 12.0f, resStr.c_str());

            std::string metaStr = std::format("{:.2f}s · {} · {}", snap.job.elapsed_sec, snap.job.out_size_str, snap.model);
            drawText(dl, ui.fontMono, 11.0f, ImVec2(t1.x + 20.0f + rsz.x, cy - 7.0f), pal.dim(), metaStr.c_str());

            drawText(dl, ui.fontSans, 11.0f, ImVec2(bar1.x - 134.0f, cy - 7.5f), hex(0x8E8E9A), "定位文件");
            ImVec2 open0 = snapPt(bar1.x - 76.0f, cy - 12.0f);
            drawBox(dl, open0, ImVec2(open0.x + 64.0f, open0.y + 24.0f), hex(0x24242C), hex(0x343440), 4.0f);
            drawTextCentered(dl, ui.fontSans, 11.0f, open0, ImVec2(open0.x + 64.0f, open0.y + 24.0f), pal.onSurface(), "打开图像");
        } else if (snap.job.phase == upix::gui::JobPhase::Error) {
            dl->AddCircleFilled(ImVec2(bar0.x + 19.0f, cy), 3.0f, pal.error());

            ImVec2 t0 = snapPt(bar0.x + 32.0f, cy - 10.0f);
            ImVec2 t1 = snapPt(t0.x + 42.0f, cy + 10.0f);
            drawBox(dl, t0, t1, pal.errorDimBg(), pal.errorDimBorder(), 4.0f);
            drawTextCentered(dl, ui.fontMono, 10.0f, t0, t1, pal.error(), "VRAM");

            drawText(dl, ui.fontSans, 12.0f, ImVec2(t1.x + 10.0f, cy - 8.0f), pal.onSurface(), snap.job.message.c_str());
            ImVec2 msz = calcText(ui.fontSans, 12.0f, snap.job.message.c_str());

            drawText(dl, ui.fontMono, 11.0f, ImVec2(t1.x + 18.0f + msz.x, cy - 7.0f), pal.dim(), snap.job.diag_detail.c_str());

            drawText(dl, ui.fontSans, 11.0f, ImVec2(bar1.x - 94.0f, cy - 7.5f), hex(0x8E8E9A), "跳过");
            if (pillButton(dl, "##retry_job", ImVec2(bar1.x - 56.0f, cy - 12.0f), ImVec2(44.0f, 24.0f), "重试", "", pal, ui.fontSans, false, 4.0f, 11.0f)) {
                app.startJob();
            }
        } else {
            std::string readyStr = std::format("已就绪 · {} 项输入待处理", snap.inputs.size());
            drawText(dl, ui.fontSans, 12.0f, ImVec2(bar0.x + 16.0f, cy - 8.0f), pal.muted(), readyStr.c_str());
        }
    }

    // ---- Minimal Filmstrip Dock (880x54: pure 40x40 thumbnails + bottom status hairline, no text clutter) ----
    if (hasDock) {
        ImVec2 dock0 = snapPt(stageMin.x, stageMax.y + 10.0f);
        ImVec2 dock1 = snapPt(stageMax.x, stageMax.y + 10.0f + dockBarH);
        drawBox(dl, dock0, dock1, pal.surface(), hex(0x26262E), 10.0f);

        float cx = dock0.x + 8.0f;
        float cy = dock0.y + 7.0f;
        size_t doneCnt = 0;
        for (const auto& it : snap.inputs) {
            if (it.status_kind == 2) ++doneCnt;
        }
        for (size_t i = 0; i < snap.inputs.size() && i < 16; ++i) {
            const auto& item = snap.inputs[i];
            ImVec2 th0 = snapPt(cx, cy);
            ImVec2 th1 = snapPt(cx + 40.0f, cy + 40.0f);

            drawMiniThumb(dl, th0, th1, ui, 6.0f, i);

            if (item.status_kind == 0) { // queued
                dl->AddRectFilled(th0, th1, rgba(16, 16, 20, 145), 6.0f);
                dl->AddRect(th0, th1, hex(0x25252D), 6.0f, 0, hairlinePx());
            } else if (item.status_kind == 1) { // running (active glow border + bottom progress bar)
                dl->AddRect(th0, th1, pal.primary(), 6.0f, 0, hairlinePx() * 1.5f);
                float progW = std::clamp(snap.job.percent / 100.0f, 0.08f, 1.0f) * 32.0f;
                dl->AddRectFilled(snapPt(th0.x + 4.0f, th1.y - 3.5f), snapPt(th0.x + 4.0f + 32.0f, th1.y - 1.5f), rgba(14, 14, 18, 200), 1.0f);
                dl->AddRectFilled(snapPt(th0.x + 4.0f, th1.y - 3.5f), snapPt(th0.x + 4.0f + progW, th1.y - 1.5f), pal.primary(), 1.0f);
            } else if (item.status_kind == 2) { // done (subtle frame + bottom teal accent line)
                dl->AddRect(th0, th1, hex(0x2E3836), 6.0f, 0, hairlinePx());
                dl->AddRectFilled(snapPt(th0.x + 10.0f, th1.y - 2.5f), snapPt(th1.x - 10.0f, th1.y - 1.0f), pal.primary(), 1.0f);
            } else if (item.status_kind == 3) { // error (coral border + bottom error bar)
                dl->AddRectFilled(th0, th1, rgba(35, 16, 18, 120), 6.0f);
                dl->AddRect(th0, th1, pal.error(), 6.0f, 0, hairlinePx() * 1.5f);
                dl->AddRectFilled(snapPt(th0.x + 8.0f, th1.y - 3.0f), snapPt(th1.x - 8.0f, th1.y - 1.0f), pal.error(), 1.0f);
            }

            cx += 40.0f + 6.0f;
        }

        // Right counter ("2 / 5") + 1x18px divider + 28x28 "+" button
        std::string cntText = std::format("{} / {}", doneCnt, snap.inputs.size());
        ImVec2 cntSz = calcText(ui.fontMono, 11.0f, cntText.c_str());
        drawText(dl, ui.fontMono, 11.0f, ImVec2(dock1.x - 58.0f - cntSz.x, dock0.y + 19.5f), pal.dim(), cntText.c_str());
        dl->AddRectFilled(snapPt(dock1.x - 46.0f, dock0.y + 18.0f), snapPt(dock1.x - 46.0f + hairlinePx(), dock0.y + 36.0f), hex(0x26262E));
        ImVec2 add0 = snapPt(dock1.x - 36.0f, dock0.y + 13.0f);
        ImGui::SetCursorScreenPos(add0);
        if (ImGui::InvisibleButton("##dock_add", ImVec2(28.0f, 28.0f))) {
            SDL_ShowOpenFileDialog(OnFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
        }
        drawBox(dl, add0, ImVec2(add0.x + 28.0f, add0.y + 28.0f), hex(0x202027), hex(0x2E2E38), 6.0f);
        drawIcon(dl, "plus", ImVec2(add0.x + 14.0f, add0.y + 14.0f), 12.0f, pal.muted());
    }

    // -----------------------------------------------------------------------
    // 3. Right Inspector (308px fixed width) — Figma "Desktop / Inspector"
    // Note: In Figma 4:2 & 73:1931, Inspector background is #131316 (var(--bg)) with NO left border!
    // -----------------------------------------------------------------------
    dl->AddRectFilled(ImVec2(inspX, 0), ImVec2(winSize.x, winSize.y), pal.bg());

    float ix = snapPx(inspX + 16.0f);
    float iw = 276.0f;
    float iy = 16.0f;

    // --- Section: 模型 (Figma: 12px #a2a2ac, gap 8px/12px) ---
    drawText(dl, ui.fontSans, 12.0f, ImVec2(ix, iy), pal.muted(), "模型");
    iy += 24.0f;

    // Model Picker Field (Figma: height 36px, #18181d, border #2d2d36, radius 8px)
    ImVec2 mp0 = snapPt(ix, iy);
    ImVec2 mp1 = snapPt(ix + iw, iy + 36.0f);
    ImGui::SetCursorScreenPos(mp0);
    if (ImGui::InvisibleButton("##model_picker_btn", ImVec2(iw, 36.0f))) {
        ui.showModelPicker = !ui.showModelPicker;
    }
    bool mpHover = ImGui::IsItemHovered();
    if (mpHover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    drawBox(dl, mp0, mp1, mpHover ? pal.container() : hex(0x18181D), ui.showModelPicker ? pal.primaryDimBorder() : hex(0x2D2D36), 8.0f);

    std::string shortMod = snap.model;
    int modScale = 4;
    bool modRec = true;
    for (const auto& m : snap.installed) {
        if (m.name == snap.model) {
            shortMod = m.short_name;
            modScale = m.scale;
            modRec = m.recommended;
            break;
        }
    }
    drawText(dl, ui.fontMono, 13.0f, ImVec2(mp0.x + 12.0f, mp0.y + 9.0f), pal.onSurface(), shortMod.c_str());
    if (modRec) {
        ImVec2 mtsz = calcText(ui.fontMono, 13.0f, shortMod.c_str());
        drawText(dl, ui.fontSans, 11.0f, ImVec2(mp0.x + 18.0f + mtsz.x, mp0.y + 10.5f), pal.onSurface(), "★");
    }

    // [4×] badge (Figma: height 20px, #1c1c22, border #2e2e37, radius 4px, 11px #a2a2ac) + 14x14 chevron
    std::string scaleBadge = std::format("{}×", modScale);
    ImVec2 b0 = snapPt(mp1.x - 58.0f, mp0.y + 8.0f);
    ImVec2 b1 = snapPt(mp1.x - 32.0f, mp0.y + 28.0f);
    drawBox(dl, b0, b1, hex(0x1C1C22), hex(0x2E2E37), 4.0f);
    drawTextCentered(dl, ui.fontMono, 11.0f, b0, b1, pal.muted(), scaleBadge.c_str());
    drawIcon(dl, "chevron-down", ImVec2(mp1.x - 18.0f, mp0.y + 18.0f), 14.0f, pal.muted());
    iy += 48.0f;

    // --- Section: 输出 (Figma: 12px #a2a2ac) ---
    drawText(dl, ui.fontSans, 12.0f, ImVec2(ix, iy), pal.muted(), "输出");
    iy += 24.0f;
    const SegmentOption fmtOpts[5] = {
        {"跟随", ""},
        {"PNG", "png"},
        {"JPG", "jpg"},
        {"WebP", "webp"},
        {"AVIF", "avif"},
    };
    std::string curFmt = snap.format;
    if (segmentedControl(dl, "##fmt_seg", ImVec2(ix, iy), ImVec2(iw, 32.0f), fmtOpts, 5, curFmt, pal, ui.fontSans, 12.0f)) {
        app.setFormat(curFmt);
    }
    iy += 44.0f;

    // Quality Slider ("质量" — Figma: label 14px #ececee, value 12.5px/14px JetBrains Mono #ececee)
    if (snap.format != "png") {
        drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy), pal.onSurface(), "质量");
        std::string qValStr = std::format("{}", snap.quality);
        ImVec2 qsz = calcText(ui.fontMono, 13.0f, qValStr.c_str());
        drawText(dl, ui.fontMono, 13.0f, ImVec2(ix + iw - qsz.x, iy + 1.0f), pal.onSurface(), qValStr.c_str());
        iy += 26.0f;

        int q = snap.quality;
        if (qualitySlider(dl, "##q_slider", ImVec2(ix, iy), iw, q, 1, 100, pal)) {
            app.setQuality(q);
        }
        iy += 30.0f;
    }

    // Format-linked encoder parameters (AVIF / JPG chroma & speed — Figma Frame 73:1931: 14px label, 164x28 segmented)
    if (snap.format == "avif" || snap.format == "jpg") {
        drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy + 4.0f), pal.onSurface(), "色度抽样");
        const SegmentOption chromaOpts[2] = {{"4:2:0", "420"}, {"4:4:4", "444"}};
        std::string cVal = std::to_string(snap.chroma);
        if (segmentedControl(dl, "##chroma_seg", ImVec2(ix + iw - 164.0f, iy), ImVec2(164.0f, 28.0f), chromaOpts, 2, cVal, pal, ui.fontMono, 12.0f)) {
            app.setChroma(std::atoi(cVal.c_str()));
        }
        iy += 38.0f;
    }
    if (snap.format == "avif") {
        drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy + 4.0f), pal.onSurface(), "编码速度");
        const SegmentOption spdOpts[3] = {{"精细", "4"}, {"均衡", "6"}, {"快速", "8"}};
        std::string sVal = std::to_string(snap.avif_speed);
        if (segmentedControl(dl, "##spd_seg", ImVec2(ix + iw - 164.0f, iy), ImVec2(164.0f, 28.0f), spdOpts, 3, sVal, pal, ui.fontSans, 12.0f)) {
            app.setAvifSpeed(std::atoi(sVal.c_str()));
        }
        iy += 38.0f;
    }

    // Export Directory ("导出位置" — Figma: 13px #ececee, gap 8px, box 34px #16161a border #282830 radius 8px)
    drawText(dl, ui.fontSans, 13.0f, ImVec2(ix, iy), pal.onSurface(), "导出位置");
    iy += 24.0f;
    ImVec2 dir0 = snapPt(ix, iy);
    ImVec2 dir1 = snapPt(ix + iw, iy + 34.0f);
    drawBox(dl, dir0, dir1, hex(0x16161A), pal.outline(), 8.0f);
    drawIcon(dl, "folder", ImVec2(dir0.x + 16.5f, dir0.y + 17.0f), 13.0f, hex(0x7E7E8A));

    bool hasCustomDir = !snap.custom_out_dir.empty();
    std::string outLabel = hasCustomDir ? snap.custom_out_dir : "跟随原图目录";
    drawText(dl, hasCustomDir ? ui.fontMono : ui.fontSans, 12.0f, ImVec2(dir0.x + 31.0f, dir0.y + 9.0f),
             hex(0xD0D0D8), outLabel.c_str());

    // Pill "指定…" / "更改" (Figma: height 24px, #202026, border #2e2e38, radius 6px, 11px #a2a2ac)
    const char* dirBtnLbl = hasCustomDir ? "更改" : "指定…";
    float dirBtnW = hasCustomDir ? 40.0f : 48.0f;
    ImVec2 db0 = snapPt(dir1.x - dirBtnW - 4.0f, dir0.y + 5.0f);
    ImGui::SetCursorScreenPos(db0);
    if (ImGui::InvisibleButton("##pick_outdir", ImVec2(dirBtnW, 24.0f))) {
        SDL_ShowOpenFolderDialog(OnFileDialog, dlgOutDir, window, nullptr, false);
    }
    drawBox(dl, db0, ImVec2(db0.x + dirBtnW, db0.y + 24.0f), pal.container(), hex(0x2E2E38), 6.0f);
    drawTextCentered(dl, ui.fontSans, 11.0f, db0, ImVec2(db0.x + dirBtnW, db0.y + 24.0f), pal.muted(), dirBtnLbl);
    iy += 46.0f;

    // --- Section: 引擎与处理 (Figma: 12px #a2a2ac, gap 12px) ---
    drawText(dl, ui.fontSans, 12.0f, ImVec2(ix, iy), pal.muted(), "引擎与处理");
    iy += 26.0f;

    // 计算设备 (Figma: 14px #ececee, segmented 164x28px)
    drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy + 4.0f), pal.onSurface(), "计算设备");
    const SegmentOption devOpts[2] = {{"GPU", "gpu"}, {"CPU", "cpu"}};
    std::string curDev = snap.gpu ? "gpu" : "cpu";
    if (segmentedControl(dl, "##dev_seg", ImVec2(ix + iw - 164.0f, iy), ImVec2(164.0f, 28.0f), devOpts, 2, curDev, pal, ui.fontSans, 12.0f)) {
        app.setGpu(curDev == "gpu");
    }
    iy += 40.0f;

    // 分块尺寸 (Figma: 14px #ececee, segmented 164x28px)
    drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy + 4.0f), pal.onSurface(), "分块尺寸");
    const SegmentOption tileOpts[4] = {{"自动", "0"}, {"512", "512"}, {"256", "256"}, {"128", "128"}};
    std::string curTile = std::to_string(snap.tile_size);
    if (segmentedControl(dl, "##tile_seg", ImVec2(ix + iw - 164.0f, iy), ImVec2(164.0f, 28.0f), tileOpts, 4, curTile, pal, ui.fontSans, 12.0f)) {
        app.setTileSize(std::atoi(curTile.c_str()));
    }
    iy += 40.0f;

    // 常驻热引擎 (Figma: 14px #ececee, toggle 44x26px)
    drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy + 3.0f), pal.onSurface(), "常驻热引擎");
    bool daemonOn = snap.daemon.running;
    if (toggleSwitch(dl, "##daemon_toggle", ImVec2(ix + iw - 44.0f, iy), daemonOn, pal)) {
        app.toggleDaemon();
    }
    iy += 38.0f;

    // 覆盖已有结果 (Figma: 14px/15px #ececee, toggle 44x26px)
    drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy + 3.0f), pal.onSurface(), "覆盖已有结果");
    bool forceOn = snap.force;
    if (toggleSwitch(dl, "##force_toggle", ImVec2(ix + iw - 44.0f, iy), forceOn, pal)) {
        app.setForce(forceOn);
    }

    // --- Bottom Sticky Action Area (Figma: 276x44px, bottom 16px, font 14px weight 500) ---
    ImVec2 cta0 = snapPt(ix, winSize.y - 60.0f);
    ImVec2 ctaSz(iw, 44.0f);
    if (snap.job.phase == upix::gui::JobPhase::Running) {
        // Cancel CTA (Figma Frame 73:1931: #1e1416, border #58282c, stop 9x9px #ff7361, "取消超分" 14px, pill "Ctrl+." #2a181a border #4a2528)
        ImGui::SetCursorScreenPos(cta0);
        if (ImGui::InvisibleButton("##cancel_job", ctaSz)) {
            app.cancelJob();
        }
        bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        drawBox(dl, cta0, ImVec2(cta0.x + ctaSz.x, cta0.y + ctaSz.y), pal.errorDimBg(), pal.errorDimBorder(), 8.0f);

        float midY = snapPx(cta0.y + ctaSz.y * 0.5f);
        dl->AddRectFilled(snapPt(cta0.x + 74.0f, midY - 4.5f), snapPt(cta0.x + 83.0f, midY + 4.5f), pal.error(), 2.0f);
        drawText(dl, ui.fontSans, 14.0f, ImVec2(cta0.x + 91.0f, midY - 9.0f), pal.error(), "取消超分");
        ImVec2 kbd0 = snapPt(cta0.x + 155.0f, midY - 9.0f);
        ImVec2 kbd1 = snapPt(kbd0.x + 44.0f, midY + 9.0f);
        drawBox(dl, kbd0, kbd1, hex(0x2A181A), hex(0x4A2528), 4.0f);
        drawTextCentered(dl, ui.fontMono, 10.0f, kbd0, kbd1, hex(0xFF897A), "Ctrl+.");
    } else if (snap.job.phase == upix::gui::JobPhase::Error) {
        if (pillButton(dl, "##resume_job", cta0, ctaSz, "继续超分", "play", pal, ui.fontSans, false, 8.0f, 14.0f)) {
            app.startJob();
        }
    } else if (snap.job.phase == upix::gui::JobPhase::Done) {
        if (pillButton(dl, "##rerun_job", cta0, ctaSz, "重新超分", "play", pal, ui.fontSans, true, 8.0f, 14.0f)) {
            app.startJob();
        }
    } else {
        if (pillButton(dl, "##start_job", cta0, ctaSz, "开始超分", "play", pal, ui.fontSans, true, 8.0f, 14.0f)) {
            if (snap.inputs.empty()) {
                SDL_ShowOpenFileDialog(OnFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
            } else {
                app.startJob();
            }
        }
    }

    // --- Floating Model Picker Popup (Figma "Desktop / Input / Model picker", 276px wide) ---
    if (ui.showModelPicker) {
        ImVec2 pop0 = snapPt(ix, mp1.y + 6.0f);
        ImVec2 pop1 = snapPt(ix + iw, mp1.y + 274.0f);
        drawBox(dl, pop0, pop1, pal.surface(), pal.outlineStrong(), 8.0f);

        ImVec2 sb0 = snapPt(pop0.x + 6.0f, pop0.y + 6.0f);
        ImVec2 sb1 = snapPt(pop1.x - 6.0f, pop0.y + 40.0f);
        dl->AddRectFilled(sb0, sb1, pal.recessed(), 6.0f);
        drawIcon(dl, "search", ImVec2(sb0.x + 16.0f, sb0.y + 17.0f), 14.0f, pal.muted());
        dl->AddRectFilled(ImVec2(sb0.x + 30.0f, sb0.y + 10.0f), ImVec2(sb0.x + 31.5f, sb0.y + 24.0f), pal.primary());
        dl->AddRectFilled(ImVec2(sb0.x, sb1.y), ImVec2(sb1.x, sb1.y + 1.5f), pal.primary());

        float ry = sb1.y + 6.0f;
        for (size_t i = 0; i < snap.installed.size() && ry + 42.0f < pop1.y - 4.0f; ++i) {
            const auto& m = snap.installed[i];
            ImVec2 r0 = snapPt(pop0.x + 6.0f, ry);
            ImVec2 r1 = snapPt(pop1.x - 6.0f, ry + 42.0f);
            ImGui::SetCursorScreenPos(r0);
            ImGui::PushID((int)i);
            bool clicked = ImGui::InvisibleButton("##mp_row", ImVec2(r1.x - r0.x, 42.0f));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();

            bool sel = (m.name == snap.model);
            if (sel || hov) {
                drawBox(dl, r0, r1, sel ? pal.high() : pal.containerLow(), sel ? pal.outlineStrong() : 0, 6.0f);
            }
            drawText(dl, ui.fontMono, 12.5f, ImVec2(r0.x + 12.0f, r0.y + 6.0f), pal.onSurface(), m.name.c_str());
            std::string subMeta = std::format("{} · x{}", m.size, m.scale);
            drawText(dl, ui.fontMono, 11.0f, ImVec2(r0.x + 12.0f, r0.y + 23.0f), pal.dim(), subMeta.c_str());

            if (sel) {
                dl->AddCircleFilled(ImVec2(r1.x - 14.0f, (r0.y + r1.y) * 0.5f), 3.5f, pal.primary());
            }

            if (clicked) {
                app.setModel(m.name);
                ui.showModelPicker = false;
            }
            ry += 44.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// 4. Models Page (Tab 1: 模型 — Figma Frame 4:62, 760px centered column)
// ---------------------------------------------------------------------------
static void renderModelsPage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                             upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                             SDL_Window* window, DialogBridge* dlgImport) {
    using namespace upix::ui;
    const auto& pal = ui.pal;

    float railW = 76.0f;
    float availW = winSize.x - railW;
    float colW = std::min(760.0f, availW - 48.0f);
    float colX = snapPx(railW + (availW - colW) * 0.5f);
    float y = 28.0f;

    // Top bar (34px): Search (220x34, #151519, border #282830, radius 8px) + Scene Filter (210x34) + Right 3 square icon buttons (32x32)
    ImVec2 s0 = snapPt(colX, y);
    ImVec2 s1 = snapPt(colX + 220.0f, y + 34.0f);
    drawBox(dl, s0, s1, pal.recessed(), pal.outline(), 8.0f);
    drawIcon(dl, "search", ImVec2(s0.x + 17.0f, s0.y + 17.0f), 14.0f, pal.muted());
    drawText(dl, ui.fontSans, 12.0f, ImVec2(s0.x + 32.0f, s0.y + 9.0f), pal.muted(), "搜索…");

    const SegmentOption sceneOpts[5] = {
        {"全部", "all"},
        {"动漫", "动漫"},
        {"照片", "照片"},
        {"综合", "综合"},
        {"修复", "修复"},
    };
    segmentedControl(dl, "##scene_filter", ImVec2(s1.x + 12.0f, y), ImVec2(210.0f, 34.0f),
                     sceneOpts, 5, ui.modelsSceneFilter, pal, ui.fontSans, 12.0f);

    if (iconButton(dl, "##open_mdir", ImVec2(colX + colW - 108.0f, y + 1.0f), ImVec2(32.0f, 32.0f), "folder", pal)) {
        SDL_OpenURL(("file://" + snap.models_dir).c_str());
    }
    if (iconButton(dl, "##import_mod", ImVec2(colX + colW - 70.0f, y + 1.0f), ImVec2(32.0f, 32.0f), "download", pal)) {
        SDL_ShowOpenFileDialog(OnFileDialog, dlgImport, window, nullptr, 0, nullptr, true);
    }
    if (iconButton(dl, "##refresh_mod", ImVec2(colX + colW - 32.0f, y + 1.0f), ImVec2(32.0f, 32.0f), "refresh", pal)) {
        app.refreshModels();
    }
    y += 48.0f;

    auto sceneDotColor = [&](const std::string& sc) -> ImU32 {
        if (sc.find("动漫") != std::string::npos) return pal.primary();
        if (sc.find("照片") != std::string::npos) return hex(0xE5A952);
        if (sc.find("修复") != std::string::npos) return hex(0xE07A9A);
        return hex(0x9BA3E8);
    };

    auto renderFamilySection = [&](const char* title, const std::vector<upix::gui::ModelFamily>& groups, bool isRemote) {
        size_t totalCount = 0;
        for (const auto& g : groups) totalCount += g.items.size();

        // Figma 82:2938: "已安装" 12px #a2a2ac + "6" 12px #6e6e7a + 1px rule #25252c
        drawText(dl, ui.fontSans, 12.0f, ImVec2(colX, y), pal.muted(), title);
        std::string cntStr = std::format("{}", totalCount);
        drawText(dl, ui.fontMono, 12.0f, ImVec2(colX + 46.0f, y), pal.dim(), cntStr.c_str());
        dl->AddRectFilled(snapPt(colX + 64.0f, y + 9.0f), snapPt(colX + colW, y + 9.0f + hairlinePx()), hex(0x25252C));
        y += 32.0f;

        for (const auto& grp : groups) {
            std::vector<upix::gui::ModelItem> filtered;
            for (const auto& it : grp.items) {
                if (ui.modelsSceneFilter == "all" || it.scene.find(ui.modelsSceneFilter) != std::string::npos) {
                    filtered.push_back(it);
                }
            }
            if (filtered.empty()) continue;

            // Figma 82:2942: group card #1c1c21, border #282830, radius 10px; header 34px #202026; rows 38px
            float cardH = 34.0f + filtered.size() * 38.0f;
            ImVec2 c0 = snapPt(colX, y);
            ImVec2 c1 = snapPt(colX + colW, y + cardH);
            drawBox(dl, c0, c1, pal.containerLow(), pal.outline(), 10.0f);

            dl->AddRectFilled(snapPt(c0.x + 1.0f, c0.y + 1.0f), snapPt(c1.x - 1.0f, c0.y + 34.0f), pal.container(), 9.0f, ImDrawFlags_RoundCornersTop);
            drawText(dl, ui.fontSans, 12.0f, ImVec2(c0.x + 16.0f, c0.y + 9.0f), hex(0xC7C7D1), grp.label.c_str());
            std::string gCnt = std::format("{} 款", filtered.size());
            ImVec2 gsz = calcText(ui.fontSans, 11.0f, gCnt.c_str());
            drawText(dl, ui.fontSans, 11.0f, ImVec2(c1.x - gsz.x - 16.0f, c0.y + 9.5f), pal.dim(), gCnt.c_str());

            float ry = c0.y + 34.0f;
            for (size_t i = 0; i < filtered.size(); ++i) {
                const auto& m = filtered[i];
                dl->AddRectFilled(snapPt(c0.x + 1.0f, ry), snapPt(c1.x - 1.0f, ry + hairlinePx()), hex(0x23232A));

                bool isCurrent = (!isRemote && m.name == snap.model);
                drawText(dl, ui.fontMono, 13.0f, ImVec2(c0.x + 16.0f, ry + 10.5f), pal.onSurface(), m.short_name.c_str());
                ImVec2 nsz = calcText(ui.fontMono, 13.0f, m.short_name.c_str());
                float iconX = c0.x + 16.0f + nsz.x + 8.0f;
                if (m.recommended) {
                    drawText(dl, ui.fontSans, 11.0f, ImVec2(iconX, ry + 11.5f), pal.muted(), "★");
                    iconX += 16.0f;
                }
                if (isCurrent && snap.daemon.running) {
                    drawIcon(dl, "zap", ImVec2(iconX + 4.0f, ry + 19.0f), 11.5f, pal.primary());
                }

                // Figma 82:2761: Name column width 244px -> badges start at c0.x + 16 + 244 = c0.x + 260
                float bColX = c0.x + 260.0f;
                ImVec2 sc0 = snapPt(bColX, ry + 9.0f);
                ImVec2 sc1 = snapPt(bColX + 46.0f, ry + 29.0f);
                drawBox(dl, sc0, sc1, hex(0x23232A), hex(0x32323C), 4.0f);
                dl->AddCircleFilled(ImVec2(sc0.x + 9.5f, ry + 19.0f), 2.5f, sceneDotColor(m.scene));
                drawText(dl, ui.fontSans, 11.0f, ImVec2(sc0.x + 16.0f, ry + 11.0f), hex(0xC7C7D1), m.scene.c_str());

                std::string scaleStr = std::format("{}×", m.scale);
                ImVec2 sb0 = snapPt(sc1.x + 8.0f, ry + 9.0f);
                ImVec2 sb1 = snapPt(sb0.x + 26.0f, ry + 29.0f);
                drawBox(dl, sb0, sb1, hex(0x1C1C22), hex(0x2E2E37), 4.0f);
                drawTextCentered(dl, ui.fontMono, 11.0f, sb0, sb1, pal.muted(), scaleStr.c_str());

                ImGui::PushID(m.name.c_str());
                if (!isRemote) {
                    ImVec2 ssz = calcText(ui.fontMono, 12.0f, m.size.c_str());
                    drawText(dl, ui.fontMono, 12.0f, ImVec2(c1.x - 52.0f - ssz.x, ry + 11.0f), hex(0x8F8F9E), m.size.c_str());
                    ImGui::SetCursorScreenPos(ImVec2(c1.x - 40.0f, ry + 6.0f));
                    if (ImGui::InvisibleButton("##del_m", ImVec2(26.0f, 26.0f))) {
                        app.removeModel(m.name);
                    }
                    drawIcon(dl, "trash", ImVec2(c1.x - 27.0f, ry + 19.0f), 13.5f, hex(0x5E5E6A));
                } else {
                    if (snap.downloading == m.name) {
                        ImVec2 ssz = calcText(ui.fontMono, 12.0f, m.size.c_str());
                        drawText(dl, ui.fontMono, 12.0f, ImVec2(c1.x - 140.0f - ssz.x, ry + 11.0f), hex(0x8F8F9E), m.size.c_str());
                        float barX = snapPx(c1.x - 126.0f);
                        float barW = 72.0f;
                        dl->AddRectFilled(ImVec2(barX, ry + 17.5f), ImVec2(barX + barW, ry + 20.5f), hex(0x2A2A33), 1.5f);
                        dl->AddRectFilled(ImVec2(barX, ry + 17.5f), ImVec2(barX + barW * (std::max(0, snap.download_pct) / 100.0f), ry + 20.5f), pal.primary(), 1.5f);
                        std::string pct = std::format("{}%", std::max(0, snap.download_pct));
                        drawText(dl, ui.fontMono, 11.0f, ImVec2(c1.x - 44.0f, ry + 11.5f), pal.primary(), pct.c_str());
                    } else {
                        ImVec2 ssz = calcText(ui.fontMono, 12.0f, m.size.c_str());
                        drawText(dl, ui.fontMono, 12.0f, ImVec2(c1.x - 52.0f - ssz.x, ry + 11.0f), hex(0x8F8F9E), m.size.c_str());
                        if (iconButton(dl, "##dl_m", ImVec2(c1.x - 40.0f, ry + 6.0f), ImVec2(26.0f, 26.0f), "download", pal, 13.0f, 6.0f)) {
                            app.downloadModel(m.name);
                        }
                    }
                }
                ImGui::PopID();

                ry += 38.0f;
            }
            y += cardH + 14.0f;
        }
    };

    renderFamilySection("已安装", snap.installed_groups, false);
    renderFamilySection("可下载", snap.remote_groups, true);
}

// ---------------------------------------------------------------------------
// 5. Remote Host Page (Tab 2: 远程 — Figma Frame 4:137 & 73:2535, 712px column)
// ---------------------------------------------------------------------------
static void renderRemotePage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                             upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& pal = ui.pal;

    float railW = 76.0f;
    float availW = winSize.x - railW;
    float colW = std::min(712.0f, availW - 48.0f);
    float colX = snapPx(railW + (availW - colW) * 0.5f);

    bool isIdle = !snap.host.running && snap.host.trusted_devices.empty() && snap.host.logs.empty();
    float totalActiveH = 288.0f + 14.0f + 110.0f + 14.0f + 122.0f; // 548px
    float y = isIdle ? snapPx((winSize.y - 288.0f) * 0.5f) : snapPx((winSize.y - totalActiveH) * 0.5f);

    // --- Card 1: Pairing Panel (712x288, Figma 73:2672: #1c1c21, border #2d2d34, radius 12px, padding 24px, gap 28px) ---
    ImVec2 p0 = snapPt(colX, y);
    ImVec2 p1 = snapPt(colX + colW, y + 288.0f);
    drawBox(dl, p0, p1, pal.containerLow(), pal.outlineInput(), 12.0f);

    // Left 240x240 QR Well
    ImVec2 qr0 = snapPt(p0.x + 24.0f, p0.y + 24.0f);
    ImVec2 qr1 = snapPt(qr0.x + 240.0f, qr0.y + 240.0f);
    drawBox(dl, qr0, qr1, pal.recessed(), pal.outline(), 10.0f);

    ImU32 regCol = snap.host.running ? pal.primaryDimBorder() : pal.regMark();
    drawRegistrationMarks(dl, qr0, qr1, 14.0f, 12.0f, 2.0f, regCol);

    if (snap.host.running && !snap.host.qr_payload.empty()) {
        try {
            qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(snap.host.qr_payload.c_str(), qrcodegen::QrCode::Ecc::LOW);
            int qsz = qr.getSize();
            float boxSz = 192.0f;
            float cell = std::floor(boxSz / (qsz + 4));
            float realSz = cell * (qsz + 4);
            ImVec2 qStart = snapPt(qr0.x + (240.0f - realSz) * 0.5f, qr0.y + (240.0f - realSz) * 0.5f);
            dl->AddRectFilled(qStart, ImVec2(qStart.x + realSz, qStart.y + realSz), hex(0xF4F3EF), 8.0f);
            for (int qy = 0; qy < qsz; ++qy) {
                for (int qx = 0; qx < qsz; ++qx) {
                    if (qr.getModule(qx, qy)) {
                        ImVec2 m0(qStart.x + (qx + 2) * cell, qStart.y + (qy + 2) * cell);
                        dl->AddRectFilled(m0, ImVec2(m0.x + cell, m0.y + cell), hex(0x131316));
                    }
                }
            }
        } catch (...) {}
    } else {
        ImVec2 qCenter = snapPt((qr0.x + qr1.x) * 0.5f, (qr0.y + qr1.y) * 0.5f);
        auto drawFinder = [&](ImVec2 tl) {
            dl->AddRect(tl, ImVec2(tl.x + 28.0f, tl.y + 28.0f), pal.outlineStrong(), 3.0f, 0, 2.0f);
            dl->AddRectFilled(ImVec2(tl.x + 8.0f, tl.y + 8.0f), ImVec2(tl.x + 20.0f, tl.y + 20.0f), pal.outlineStrong(), 1.5f);
        };
        drawFinder(ImVec2(qr0.x + 48.0f, qr0.y + 48.0f));
        drawFinder(ImVec2(qr1.x - 76.0f, qr0.y + 48.0f));
        drawFinder(ImVec2(qr0.x + 48.0f, qr1.y - 76.0f));

        for (int gy = 0; gy < 7; ++gy) {
            for (int gx = 0; gx < 7; ++gx) {
                if ((gx < 2 && gy < 2) || (gx > 4 && gy < 2) || (gx < 2 && gy > 4) || (gx >= 2 && gx <= 4 && gy >= 2 && gy <= 4))
                    continue;
                if ((gx * 3 + gy * 5) % 2 == 0) {
                    dl->AddCircleFilled(ImVec2(qr0.x + 62.0f + gx * 19.0f, qr0.y + 62.0f + gy * 19.0f), 2.2f, pal.outlineStrong());
                }
            }
        }

        ImVec2 badge0(qCenter.x - 24.0f, qCenter.y - 24.0f);
        ImVec2 badge1(qCenter.x + 24.0f, qCenter.y + 24.0f);
        drawBox(dl, badge0, badge1, pal.surface(), pal.outlineStrong(), 10.0f);
        drawIcon(dl, "qr-code", qCenter, 18.0f, pal.muted());
    }

    // Right Instrument Column (396px wide, gap 28px from QR)
    float rx = snapPx(qr1.x + 28.0f);
    float rw = 396.0f;
    float ry = p0.y + 24.0f;

    // Row 1: 指纹 (44px row, label 12px #7e7e8a, 44x24 pills #16161b border #26262e)
    drawText(dl, ui.fontSans, 12.0f, ImVec2(rx, ry + 13.0f), hex(0x7E7E8A), "指纹");
    float fx = rx + 48.0f;
    std::string fp = snap.host.fingerprint.empty() ? "EC61F2B66D4C25C4" : snap.host.fingerprint;
    size_t pos = 0;
    int groupIdx = 0;
    while (pos < fp.size() && groupIdx < 4) {
        while (pos < fp.size() && (fp[pos] == ' ' || fp[pos] == ':')) ++pos;
        std::string token = fp.substr(pos, std::min<size_t>(4, fp.size() - pos));
        pos += token.size();
        ImVec2 c0 = snapPt(fx, ry + 10.0f);
        ImVec2 c1 = snapPt(fx + 44.0f, ry + 34.0f);
        drawBox(dl, c0, c1, hex(0x16161B), hex(0x26262E), 4.0f);
        drawTextCentered(dl, ui.fontMono, 11.5f, c0, c1, hex(0xD8D8E0), token.c_str());
        fx += 50.0f;
        ++groupIdx;
    }
    ImGui::SetCursorScreenPos(ImVec2(fx + 2.0f, ry + 10.0f));
    if (ImGui::InvisibleButton("##copy_fp", ImVec2(24.0f, 24.0f))) {
        SDL_SetClipboardText(fp.c_str());
    }
    drawIcon(dl, "copy", ImVec2(fx + 14.0f, ry + 22.0f), 13.0f, hex(0x9A9AA6));
    ry += 44.0f;

    dl->AddRectFilled(snapPt(rx, ry), snapPt(rx + rw, ry + hairlinePx()), hex(0x26262E));
    ry += 1.0f;

    // Row 2: 地址 (44px row, 216x30 input #22222a border #282830)
    drawText(dl, ui.fontSans, 12.0f, ImVec2(rx, ry + 13.0f), hex(0x7E7E8A), "地址");
    std::string curIp = snap.host.ips.empty() ? "192.168.0.126" : snap.host.ips[snap.host.selected_ip_idx % snap.host.ips.size()];
    ImVec2 ip0 = snapPt(rx + 48.0f, ry + 7.0f);
    ImVec2 ip1 = snapPt(ip0.x + 216.0f, ry + 37.0f);
    drawBox(dl, ip0, ip1, hex(0x22222A), pal.outline(), 6.0f);
    dl->AddCircleFilled(ImVec2(ip0.x + 12.5f, ry + 22.0f), 2.5f, pal.primary());
    drawText(dl, ui.fontMono, 12.0f, ImVec2(ip0.x + 23.0f, ry + 14.0f), pal.onSurface(), curIp.c_str());
    ImVec2 lan0 = snapPt(ip1.x - 50.0f, ry + 14.0f);
    drawBox(dl, lan0, ImVec2(lan0.x + 28.0f, lan0.y + 16.0f), hex(0x23232B), 0, 4.0f);
    drawTextCentered(dl, ui.fontMono, 9.5f, lan0, ImVec2(lan0.x + 28.0f, lan0.y + 16.0f), hex(0x8E8E9A), "LAN");
    drawIcon(dl, "chevron-down", ImVec2(ip1.x - 13.0f, ry + 22.0f), 10.0f, hex(0x7E7E8A));
    ry += 44.0f;

    dl->AddRectFilled(snapPt(rx, ry), snapPt(rx + rw, ry + hairlinePx()), hex(0x26262E));
    ry += 1.0f;

    // Row 3: 端口 (44px row, 84x30 input #16161b border #2a2a32)
    drawText(dl, ui.fontSans, 12.0f, ImVec2(rx, ry + 13.0f), hex(0x7E7E8A), "端口");
    ImVec2 pt0 = snapPt(rx + 48.0f, ry + 7.0f);
    ImVec2 pt1 = snapPt(pt0.x + 84.0f, ry + 37.0f);
    drawBox(dl, pt0, pt1, hex(0x16161B), hex(0x2A2A32), 6.0f);
    drawText(dl, ui.fontMono, 12.0f, ImVec2(pt0.x + 10.0f, ry + 14.0f), pal.dim(), ":");
    std::string portNum = std::format("{}", snap.host.port);
    drawText(dl, ui.fontMono, 12.0f, ImVec2(pt0.x + 20.0f, ry + 14.0f), pal.onSurface(), portNum.c_str());

    // Bottom of card: Left [● LIVE] (64x24) + Right [停止服务] (98x36, #26262e border #383844)
    float botY = p1.y - 60.0f;
    ImVec2 st0 = snapPt(rx, botY + 6.0f);
    ImVec2 st1 = snapPt(rx + 64.0f, botY + 30.0f);
    if (snap.host.running) {
        drawBox(dl, st0, st1, hex(0x162624), hex(0x24423E), 12.0f);
        dl->AddCircleFilled(ImVec2(st0.x + 13.0f, st0.y + 12.0f), 3.0f, pal.primary());
        drawText(dl, ui.fontMono, 10.5f, ImVec2(st0.x + 22.0f, st0.y + 5.0f), pal.primary(), "LIVE");

        ImVec2 stop0 = snapPt(rx + rw - 98.0f, botY);
        ImGui::SetCursorScreenPos(stop0);
        if (ImGui::InvisibleButton("##host_stop", ImVec2(98.0f, 36.0f))) {
            app.hostStop();
        }
        drawBox(dl, stop0, ImVec2(stop0.x + 98.0f, stop0.y + 36.0f), hex(0x26262E), hex(0x383844), 6.0f);
        dl->AddRectFilled(snapPt(stop0.x + 15.0f, stop0.y + 14.0f), snapPt(stop0.x + 23.0f, stop0.y + 22.0f), pal.onSurface(), 2.0f);
        drawText(dl, ui.fontSans, 13.0f, ImVec2(stop0.x + 30.0f, stop0.y + 9.0f), pal.onSurface(), "停止服务");
    } else {
        drawBox(dl, st0, st1, pal.recessed(), pal.outline(), 12.0f);
        dl->AddCircleFilled(ImVec2(st0.x + 13.0f, st0.y + 12.0f), 3.0f, pal.dim());
        drawText(dl, ui.fontMono, 10.5f, ImVec2(st0.x + 22.0f, st0.y + 5.0f), pal.dim(), "IDLE");

        if (pillButton(dl, "##host_start", ImVec2(rx + rw - 98.0f, botY), ImVec2(98.0f, 36.0f), "启动服务", "play-outline", pal, ui.fontSans, true, 6.0f, 13.0f)) {
            app.hostStart();
        }
    }

    if (isIdle) return;
    y += 288.0f + 14.0f;

    // --- Card 2: 已信任设备 (712x110, Figma 83:3669: #18181c, border #282830, header 34px #202026e5, rows 38px) ---
    float tdH = 34.0f + std::max<size_t>(1, snap.host.trusted_devices.size()) * 38.0f;
    ImVec2 td0 = snapPt(colX, y);
    ImVec2 td1 = snapPt(colX + colW, y + tdH);
    drawBox(dl, td0, td1, pal.surface(), pal.outline(), 10.0f);
    dl->AddRectFilled(snapPt(td0.x + 1.0f, td0.y + 1.0f), snapPt(td1.x - 1.0f, td0.y + 34.0f), pal.container(), 9.0f, ImDrawFlags_RoundCornersTop);
    drawText(dl, ui.fontSans, 12.5f, ImVec2(td0.x + 16.0f, td0.y + 8.5f), hex(0xE2E2E8), "已信任设备");
    std::string tdCnt = std::format("{}", snap.host.trusted_devices.size());
    drawText(dl, ui.fontMono, 11.0f, ImVec2(td1.x - 24.0f, td0.y + 9.5f), hex(0x7E7E8A), tdCnt.c_str());

    float dy = td0.y + 34.0f;
    for (size_t i = 0; i < snap.host.trusted_devices.size(); ++i) {
        const auto& d = snap.host.trusted_devices[i];
        dl->AddCircleFilled(ImVec2(td0.x + 19.0f, dy + 19.0f), 3.0f, d.online ? pal.primary() : hex(0x4A4A56));
        drawText(dl, ui.fontMono, 12.5f, ImVec2(td0.x + 32.0f, dy + 10.5f), d.online ? pal.onSurface() : hex(0x9E9EA8), d.name.c_str());
        ImVec2 dsz = calcText(ui.fontMono, 12.5f, d.name.c_str());

        ImVec2 b0 = snapPt(td0.x + 42.0f + dsz.x, dy + 9.5f);
        ImVec2 b1 = snapPt(b0.x + 40.0f, dy + 28.5f);
        drawBox(dl, b0, b1, d.online ? pal.primaryDimBg() : pal.container(), d.online ? hex(0x264A43) : hex(0x2C2C35), 4.0f);
        drawTextCentered(dl, ui.fontSans, 11.0f, b0, b1, d.online ? pal.primary() : hex(0x7E7E8A), d.online ? "在线" : "昨天");

        ImVec2 fpsz = calcText(ui.fontMono, 11.5f, d.fingerprint.c_str());
        drawText(dl, ui.fontMono, 11.5f, ImVec2(td1.x - 56.0f - fpsz.x, dy + 11.0f), pal.dim(), d.fingerprint.c_str());
        drawIcon(dl, "trash", ImVec2(td1.x - 27.0f, dy + 19.0f), 13.5f, hex(0x5E5E6A));
        dy += 38.0f;
    }
    y += tdH + 14.0f;

    // --- Card 3: Active Telemetry Console (712x122, Figma 83:3730: #151519, border #282830, header 38px #1d1d23) ---
    ImVec2 lg0 = snapPt(colX, y);
    ImVec2 lg1 = snapPt(colX + colW, y + 122.0f);
    drawBox(dl, lg0, lg1, pal.recessed(), pal.outline(), 10.0f);

    dl->AddRectFilled(snapPt(lg0.x + 1.0f, lg0.y + 1.0f), snapPt(lg1.x - 1.0f, lg0.y + 38.0f), hex(0x1D1D23), 9.0f, ImDrawFlags_RoundCornersTop);
    dl->AddCircleFilled(ImVec2(lg0.x + 19.0f, lg0.y + 19.0f), 3.0f, pal.primary());
    std::string actDev = snap.host.active_device.empty() ? "Pixel 9 Pro" : snap.host.active_device;
    drawText(dl, ui.fontMono, 12.0f, ImVec2(lg0.x + 30.0f, lg0.y + 11.0f), pal.onSurface(), actDev.c_str());

    ImVec2 mb0 = snapPt(lg0.x + 124.0f, lg0.y + 10.0f);
    ImVec2 mb1 = snapPt(mb0.x + 118.0f, lg0.y + 28.0f);
    drawBox(dl, mb0, mb1, hex(0x16161B), hex(0x2A2A34), 4.0f);
    drawTextCentered(dl, ui.fontMono, 10.0f, mb0, mb1, hex(0x8E8E9A), "AnimeSharp-x4 · 4×");

    int tPct = snap.host.task_percent >= 0 ? snap.host.task_percent : 78;
    std::string tPctStr = std::format("{}%", tPct);
    drawText(dl, ui.fontMono, 11.0f, ImVec2(lg1.x - 40.0f, lg0.y + 11.5f), pal.primary(), tPctStr.c_str());
    float pBarW = 136.0f;
    float pBarX = snapPx(lg1.x - 52.0f - pBarW);
    dl->AddRectFilled(ImVec2(pBarX, lg0.y + 17.0f), ImVec2(pBarX + pBarW, lg0.y + 21.0f), hex(0x121216), 2.0f);
    dl->AddRectFilled(ImVec2(pBarX, lg0.y + 17.0f), ImVec2(pBarX + pBarW * (tPct / 100.0f), lg0.y + 21.0f), pal.primary(), 2.0f);

    float ly = lg0.y + 46.0f;
    for (size_t i = 0; i < snap.host.logs.size() && i < 3; ++i) {
        const std::string& raw = snap.host.logs[i];
        auto p1Pos = raw.find('|');
        if (p1Pos != std::string::npos) {
            auto p2Pos = raw.find('|', p1Pos + 1);
            auto p3Pos = raw.find('|', p2Pos + 1);
            std::string tm = raw.substr(0, p1Pos);
            std::string tag = raw.substr(p1Pos + 1, p2Pos - p1Pos - 1);
            std::string msg = raw.substr(p2Pos + 1, p3Pos - p2Pos - 1);
            std::string rNote = (p3Pos != std::string::npos) ? raw.substr(p3Pos + 1) : "";

            drawText(dl, ui.fontMono, 11.0f, ImVec2(lg0.x + 16.0f, ly), hex(0x565662), tm.c_str());

            bool isTask = (tag == "TASK");
            ImVec2 tg0 = snapPt(lg0.x + 76.0f, ly - 1.0f);
            ImVec2 tg1 = snapPt(tg0.x + 42.0f, ly + 15.0f);
            drawBox(dl, tg0, tg1, isTask ? pal.primaryDimBg() : hex(0x1D1E24), isTask ? pal.primaryDimBorder() : hex(0x2D2E38), 4.0f);
            drawTextCentered(dl, ui.fontMono, 10.0f, tg0, tg1, isTask ? pal.primary() : hex(0x8E8E9A), tag.c_str());

            drawText(dl, ui.fontSans, 11.0f, ImVec2(tg1.x + 8.0f, ly - 0.5f), isTask ? hex(0xC8C8D2) : hex(0x8E8E9A), msg.c_str());

            if (!rNote.empty()) {
                ImVec2 nsz = calcText(ui.fontMono, 11.0f, rNote.c_str());
                drawText(dl, ui.fontMono, 11.0f, ImVec2(lg1.x - 16.0f - nsz.x, ly), isTask ? pal.primary() : hex(0x565662), rNote.c_str());
            }
        } else {
            drawText(dl, ui.fontMono, 11.0f, ImVec2(lg0.x + 16.0f, ly), pal.muted(), raw.c_str());
        }
        ly += 24.0f;
    }
}

// ---------------------------------------------------------------------------
// 6. Pairing Sheet Modal (Figma Frame 83:3762, 380x240 centered over scrim)
// ---------------------------------------------------------------------------
static void renderPairingModal(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                               upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    if (snap.host.pending_device.empty()) return;
    using namespace upix::ui;
    const auto& pal = ui.pal;

    dl->AddRectFilled(ImVec2(0, 0), winSize, rgba(8, 8, 11, 185));

    ImVec2 sz(380.0f, 240.0f);
    ImVec2 m0 = snapPt((winSize.x - sz.x) * 0.5f, (winSize.y - sz.y) * 0.5f);
    ImVec2 m1 = snapPt(m0.x + sz.x, m0.y + sz.y);
    drawBox(dl, m0, m1, pal.surface(), pal.outlineStrong(), 14.0f);

    float headY = m0.y + 24.0f;
    ImVec2 ic0 = snapPt(m0.x + 100.0f, headY);
    ImVec2 ic1 = snapPt(ic0.x + 28.0f, headY + 28.0f);
    drawBox(dl, ic0, ic1, pal.primaryDimBg(), pal.primaryDimBorder(), 7.0f);
    drawIcon(dl, "smartphone", ImVec2(ic0.x + 14.0f, ic0.y + 14.0f), 15.0f, pal.primary());

    drawText(dl, ui.fontMono, 15.0f, ImVec2(ic1.x + 10.0f, headY + 4.5f), pal.onSurface(), snap.host.pending_device.c_str());
    ImVec2 dsz = calcText(ui.fontMono, 15.0f, snap.host.pending_device.c_str());

    ImVec2 pb0 = snapPt(ic1.x + 20.0f + dsz.x, headY + 4.0f);
    ImVec2 pb1 = snapPt(pb0.x + 38.0f, headY + 24.0f);
    drawBox(dl, pb0, pb1, pal.high(), pal.outlineStrong(), 4.0f);
    drawTextCentered(dl, ui.fontMono, 10.5f, pb0, pb1, pal.muted(), "PAIR");

    ImVec2 fw0 = snapPt(m0.x + 22.0f, m0.y + 72.0f);
    ImVec2 fw1 = snapPt(m1.x - 22.0f, m0.y + 156.0f);
    drawBox(dl, fw0, fw1, pal.recessed(), pal.outline(), 10.0f);
    drawTextCentered(dl, ui.fontSans, 11.5f, ImVec2(fw0.x, fw0.y + 12.0f), ImVec2(fw1.x, fw0.y + 28.0f), pal.muted(), "核对设备指纹一致");

    std::string fp = snap.host.pending_fp.empty() ? "EC61F2B66D4C25C4" : snap.host.pending_fp;
    float pillW = 60.0f;
    float pillGap = 8.0f;
    float startX = snapPx(fw0.x + (336.0f - (pillW * 4 + pillGap * 3)) * 0.5f);
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        while (pos < fp.size() && (fp[pos] == ' ' || fp[pos] == ':')) ++pos;
        std::string tok = fp.substr(pos, std::min<size_t>(4, fp.size() - pos));
        pos += tok.size();
        ImVec2 c0 = snapPt(startX + i * (pillW + pillGap), fw0.y + 38.0f);
        ImVec2 c1 = snapPt(c0.x + pillW, c0.y + 32.0f);
        drawBox(dl, c0, c1, pal.surface(), pal.outlineStrong(), 6.0f);
        drawTextCentered(dl, ui.fontMono, 13.0f, c0, c1, pal.onSurface(), tok.c_str());
    }

    ImVec2 deny0 = snapPt(m0.x + 22.0f, m1.y - 56.0f);
    ImVec2 denySz(116.0f, 38.0f);
    ImGui::SetCursorScreenPos(deny0);
    if (ImGui::InvisibleButton("##pair_deny", denySz)) {
        app.hostDecide(false);
    }
    drawBox(dl, deny0, ImVec2(deny0.x + denySz.x, deny0.y + denySz.y), pal.container(), pal.outlineStrong(), 8.0f);
    drawText(dl, ui.fontSans, 13.0f, ImVec2(deny0.x + 30.0f, deny0.y + 10.0f), pal.onSurface(), "拒绝");
    drawText(dl, ui.fontMono, 11.5f, ImVec2(deny0.x + 64.0f, deny0.y + 11.0f), pal.dim(), "Esc");

    if (pillButton(dl, "##pair_allow", ImVec2(deny0.x + denySz.x + 10.0f, m1.y - 56.0f), ImVec2(210.0f, 38.0f), "允许连接", "", pal, ui.fontSans, true, 8.0f, 13.0f)) {
        app.hostDecide(true);
    }
}

} // namespace

int main(int argc, char** argv) {
    // 1. Subprocess mode: warm daemon (`upix --serve-daemon`) runs headless without GUI init
    std::string fixtureState;
    std::string shotPath;
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
