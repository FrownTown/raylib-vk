/**********************************************************************************************
*
*   rvk_context.cpp - Instance, Device, Swapchain, Command Pools, frame lifecycle
*
*   Implements: device/swapchain init (vk-bootstrap), VMA allocator, sync2 frame loop with
*   dynamic rendering, present, swapchain recreation, the per-frame deletion queue, one-shot
*   command helpers, and renderer info getters.
*
*   STATUS: P1 scaffold. Device/swapchain/frame structure is in place. Sections marked TODO(Px)
*   are completed by later phases. Requires the Vulkan SDK + fetched deps to compile/build
*   (see cmake/VulkanBackend.cmake); the headless CI container here has the loader but no headers.
*
**********************************************************************************************/
// Emit the VMA implementation in this single translation unit. Must be defined BEFORE
// rvk_internal.hpp (which includes vk_mem_alloc.h) or the include guard would suppress it.
#define VMA_IMPLEMENTATION
#include "rvk_internal.hpp"

#include <VkBootstrap.h>

// vulkan.h is already included via rvk_internal.hpp, so GLFW only needs to expose its
// Vulkan surface helpers (declared when VK_VERSION_1_0 is visible). Avoid GLFW pulling in
// any GL headers.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

RvkContext g_rvk;                       // the single global context

static VkClearValue s_clearColor = { {{ 0.0f, 0.0f, 0.0f, 1.0f }} };

//----------------------------------------------------------------------------------
// VkResult -> string (for VK_CHECK diagnostics)
//----------------------------------------------------------------------------------
const char *rvkResultString(VkResult r)
{
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        default: return "VK_ERROR_<other>";
    }
}

//----------------------------------------------------------------------------------
// Frame / deletion-queue helpers
//----------------------------------------------------------------------------------
RvkFrame &rvkCurrentFrame() { return g_rvk.frames[g_rvk.frameIndex]; }

void rvkDeferDestroy(std::function<void()> fn) { rvkCurrentFrame().deletionQueue.push_back(std::move(fn)); }

static void rvkFlushDeletionQueue(RvkFrame &frame)
{
    while (!frame.deletionQueue.empty()) {
        frame.deletionQueue.front()();
        frame.deletionQueue.pop_front();
    }
}

//----------------------------------------------------------------------------------
// sync2 image layout transition helper
//----------------------------------------------------------------------------------
void rvkImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldL, VkImageLayout newL,
                     VkImageAspectFlags aspect, uint32_t mipLevels, uint32_t layers)
{
    VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = oldL;
    barrier.newLayout = newL;
    barrier.image = image;
    barrier.subresourceRange = { aspect, 0, mipLevels, 0, layers };

    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

//----------------------------------------------------------------------------------
// One-shot command buffer (transfers, layout transitions, mipmap blits)
//----------------------------------------------------------------------------------
VkCommandBuffer rvkBeginSingleTimeCommands()
{
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = rvkCurrentFrame().cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(g_rvk.device, &ai, &cmd));
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

void rvkEndSingleTimeCommands(VkCommandBuffer cmd)
{
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cmdInfo.commandBuffer = cmd;
    VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    VK_CHECK(vkQueueSubmit2(g_rvk.graphicsQueue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(g_rvk.graphicsQueue));     // simple sync for one-shots
    vkFreeCommandBuffers(g_rvk.device, rvkCurrentFrame().cmdPool, 1, &cmd);
}

//----------------------------------------------------------------------------------
// Swapchain (re)creation
//----------------------------------------------------------------------------------
static void rvkCreateSwapchain(int width, int height)
{
    VkSwapchainKHR oldSwapchain = g_rvk.swapchain;
    std::vector<VkImageView> oldViews = g_rvk.swapchainViews;

    vkb::SwapchainBuilder builder{ g_rvk.physicalDevice, g_rvk.device, g_rvk.surface };
    auto ret = builder
        .set_desired_format({ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)         // vsync; matches raylib default
        .set_desired_extent((uint32_t)width, (uint32_t)height)
        .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)     // for blits/clears
        .set_old_swapchain(oldSwapchain)
        .build();
    if (!ret) { TraceLog(RL_LOG_FATAL, "RVK: swapchain build failed: %s", ret.error().message().c_str()); abort(); }

    // Destroy the previous swapchain + its views now that the new one is built
    if (oldSwapchain != VK_NULL_HANDLE) {
        for (auto v : oldViews) vkDestroyImageView(g_rvk.device, v, nullptr);
        vkDestroySwapchainKHR(g_rvk.device, oldSwapchain, nullptr);
    }

    vkb::Swapchain vkbSwap = ret.value();
    g_rvk.swapchain = vkbSwap.swapchain;
    g_rvk.swapchainFormat = vkbSwap.image_format;
    g_rvk.swapchainExtent = vkbSwap.extent;
    g_rvk.swapchainImages = vkbSwap.get_images().value();
    g_rvk.swapchainViews = vkbSwap.get_image_views().value();
    g_rvk.fbWidth = (int)vkbSwap.extent.width;
    g_rvk.fbHeight = (int)vkbSwap.extent.height;
    g_rvk.state.colorFormat = g_rvk.swapchainFormat;

    // TODO(P1): (re)create the main depth target (rvk_memory) at swapchain extent.
}

//----------------------------------------------------------------------------------
// Initialization
//----------------------------------------------------------------------------------
extern "C" void rvkInit(void *glfwWindow, int width, int height)
{
    g_rvk.window = glfwWindow;

    // --- Instance (vk-bootstrap), Vulkan 1.3 + validation in debug ---
    vkb::InstanceBuilder ib;
    auto instRet = ib.set_app_name("raylib-vulkan")
                     .require_api_version(1, 3, 0)
#if RVK_ENABLE_VALIDATION
                     .request_validation_layers(true)
                     .use_default_debug_messenger()
#endif
                     .build();
    if (!instRet) { TraceLog(RL_LOG_FATAL, "RVK: instance build failed: %s", instRet.error().message().c_str()); abort(); }
    vkb::Instance vkbInst = instRet.value();
    g_rvk.instance = vkbInst.instance;
    g_rvk.debugMessenger = vkbInst.debug_messenger;

    // --- Surface (GLFW) ---
    VK_CHECK(glfwCreateWindowSurface(g_rvk.instance, (GLFWwindow*)glfwWindow, nullptr, &g_rvk.surface));

    // --- Physical device + device, requiring 1.3 dynamic rendering + sync2 ---
    VkPhysicalDeviceVulkan13Features feat13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    feat13.dynamicRendering = VK_TRUE;
    feat13.synchronization2 = VK_TRUE;

    vkb::PhysicalDeviceSelector selector{ vkbInst };
    auto physRet = selector.set_minimum_version(1, 3)
                           .set_required_features_13(feat13)
                           .set_surface(g_rvk.surface)
                           .select();
    if (!physRet) { TraceLog(RL_LOG_FATAL, "RVK: GPU select failed: %s", physRet.error().message().c_str()); abort(); }

    vkb::DeviceBuilder devBuilder{ physRet.value() };
    auto devRet = devBuilder.build();
    if (!devRet) { TraceLog(RL_LOG_FATAL, "RVK: device build failed: %s", devRet.error().message().c_str()); abort(); }
    vkb::Device vkbDev = devRet.value();

    g_rvk.physicalDevice = physRet.value().physical_device;
    g_rvk.device = vkbDev.device;
    g_rvk.graphicsQueue = vkbDev.get_queue(vkb::QueueType::graphics).value();
    g_rvk.graphicsQueueFamily = vkbDev.get_queue_index(vkb::QueueType::graphics).value();

    // --- VMA allocator (all allocations route through here; direct vkAllocateMemory banned) ---
    VmaAllocatorCreateInfo allocInfo{};
    allocInfo.physicalDevice = g_rvk.physicalDevice;
    allocInfo.device = g_rvk.device;
    allocInfo.instance = g_rvk.instance;
    allocInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    VK_CHECK(vmaCreateAllocator(&allocInfo, &g_rvk.allocator));

    // --- Per-frame command pools, sync objects, descriptor pools ---
    for (auto &frame : g_rvk.frames) {
        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = g_rvk.graphicsQueueFamily;
        VK_CHECK(vkCreateCommandPool(g_rvk.device, &pci, nullptr, &frame.cmdPool));

        VkCommandBufferAllocateInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbi.commandPool = frame.cmdPool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(g_rvk.device, &cbi, &frame.cmd));

        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(g_rvk.device, &si, nullptr, &frame.imageAvailable));
        VK_CHECK(vkCreateSemaphore(g_rvk.device, &si, nullptr, &frame.renderFinished));
        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(g_rvk.device, &fi, nullptr, &frame.inFlight));

        // TODO(P2): create per-frame descriptor pool sized for batch draws.
    }

    rvkCreateSwapchain(width, height);

    // Pipeline cache
    VkPipelineCacheCreateInfo pcci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    VK_CHECK(vkCreatePipelineCache(g_rvk.device, &pcci, nullptr, &g_rvk.pipelineCache));

    // TODO(P2/P4): create default 1x1 white texture + default sprite shader program.
    //   rvkInitDefaultShader();

    g_rvk.initialized = true;
    TraceLog(RL_LOG_INFO, "RVK: Vulkan 1.3 backend initialized (%dx%d)", g_rvk.fbWidth, g_rvk.fbHeight);
}

// rlgl-compatible entry point: raylib calls rlglInit(w,h) after the window/surface exists.
// The platform layer must call rvkInit() (with the GLFW window) first; this finalizes
// renderer-side defaults (matrix stack, batch, states).
extern "C" void rvkglInit(int width, int height)
{
    if (!g_rvk.initialized) {
        TraceLog(RL_LOG_WARNING, "RVK: rvkglInit called before rvkInit (platform must create device first)");
        return;
    }
    g_rvk.fbWidth = width; g_rvk.fbHeight = height;
    // TODO(P3): initialize internal render batch + matrix stack defaults (rvk_batcher).
}

//----------------------------------------------------------------------------------
// Frame lifecycle
//----------------------------------------------------------------------------------
extern "C" {

void rvkBeginFrame(void)
{
    RvkFrame &frame = g_rvk.frames[g_rvk.frameIndex];
    VK_CHECK(vkWaitForFences(g_rvk.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
    rvkFlushDeletionQueue(frame);                        // safe: this frame's GPU work is done

    VkResult acq = vkAcquireNextImageKHR(g_rvk.device, g_rvk.swapchain, UINT64_MAX,
                                         frame.imageAvailable, VK_NULL_HANDLE, &g_rvk.currentImageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { rvkResizeSwapchain(g_rvk.fbWidth, g_rvk.fbHeight); return; }
    else if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) VK_CHECK(acq);

    VK_CHECK(vkResetFences(g_rvk.device, 1, &frame.inFlight));
    VK_CHECK(vkResetCommandBuffer(frame.cmd, 0));

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(frame.cmd, &bi));

    VkImage img = g_rvk.swapchainImages[g_rvk.currentImageIndex];
    rvkImageBarrier(frame.cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    // Dynamic rendering: no VkRenderPass / VkFramebuffer objects
    VkRenderingAttachmentInfo colorAtt{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    colorAtt.imageView = g_rvk.swapchainViews[g_rvk.currentImageIndex];
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.clearValue = s_clearColor;

    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { {0,0}, g_rvk.swapchainExtent };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colorAtt;
    // TODO(P5/P6): attach the main depth target for 3D passes.
    vkCmdBeginRendering(frame.cmd, &ri);

    g_rvk.frameActive = true;
    g_rvk.activeFramebuffer = 0;
}

void rvkEndFrame(void)
{
    RvkFrame &frame = g_rvk.frames[g_rvk.frameIndex];
    if (!g_rvk.frameActive) return;

    vkCmdEndRendering(frame.cmd);
    VkImage img = g_rvk.swapchainImages[g_rvk.currentImageIndex];
    rvkImageBarrier(frame.cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
    VK_CHECK(vkEndCommandBuffer(frame.cmd));

    // sync2 submit: wait imageAvailable, signal renderFinished
    VkSemaphoreSubmitInfo waitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    waitInfo.semaphore = frame.imageAvailable;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signalInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signalInfo.semaphore = frame.renderFinished;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cmdInfo.commandBuffer = frame.cmd;

    VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submit.waitSemaphoreInfoCount = 1;   submit.pWaitSemaphoreInfos = &waitInfo;
    submit.signalSemaphoreInfoCount = 1; submit.pSignalSemaphoreInfos = &signalInfo;
    submit.commandBufferInfoCount = 1;   submit.pCommandBufferInfos = &cmdInfo;
    VK_CHECK(vkQueueSubmit2(g_rvk.graphicsQueue, 1, &submit, frame.inFlight));

    g_rvk.frameActive = false;
}

void rvkPresent(void)
{
    RvkFrame &frame = g_rvk.frames[g_rvk.frameIndex];
    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &frame.renderFinished;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_rvk.swapchain;
    pi.pImageIndices = &g_rvk.currentImageIndex;
    VkResult res = vkQueuePresentKHR(g_rvk.graphicsQueue, &pi);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) rvkResizeSwapchain(g_rvk.fbWidth, g_rvk.fbHeight);
    else if (res != VK_SUCCESS) VK_CHECK(res);

    g_rvk.frameIndex = (g_rvk.frameIndex + 1) % RVK_FRAMES_IN_FLIGHT;
}

void rvkResizeSwapchain(int width, int height)
{
    if (width == 0 || height == 0) return;          // minimized
    VK_CHECK(vkDeviceWaitIdle(g_rvk.device));
    rvkCreateSwapchain(width, height);
}

//----------------------------------------------------------------------------------
// Clear color / screen buffers
//----------------------------------------------------------------------------------
void rvkClearColor(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    s_clearColor.color.float32[0] = r/255.0f;
    s_clearColor.color.float32[1] = g/255.0f;
    s_clearColor.color.float32[2] = b/255.0f;
    s_clearColor.color.float32[3] = a/255.0f;
}

// The actual clear happens via LOAD_OP_CLEAR at rvkBeginFrame; mid-frame clears are a TODO(P7).
void rvkClearScreenBuffers(void) { /* handled by dynamic-rendering loadOp clear */ }

void rvkCheckErrors(void) { /* validation layers report asynchronously; no-op */ }

//----------------------------------------------------------------------------------
// Info getters
//----------------------------------------------------------------------------------
int  rvkGetVersion(void) { return RL_VULKAN; }
void rvkSetFramebufferWidth(int width) { g_rvk.fbWidth = width; }
int  rvkGetFramebufferWidth(void) { return g_rvk.fbWidth; }
void rvkSetFramebufferHeight(int height) { g_rvk.fbHeight = height; }
int  rvkGetFramebufferHeight(void) { return g_rvk.fbHeight; }
unsigned int rvkGetTextureIdDefault(void) { return g_rvk.defaultTextureId; }
unsigned int rvkGetShaderIdDefault(void) { return g_rvk.defaultShaderId; }
int *rvkGetShaderLocsDefault(void) { return g_rvk.defaultShaderLocs; }

void *rvkGetProcAddress(const char *procName) { (void)procName; return nullptr; }   // GL-only concept
void  rvkLoadExtensions(void *loader) { (void)loader; }                             // GL-only concept

} // extern "C" (frame lifecycle + clear + info)

//----------------------------------------------------------------------------------
// Shutdown
//----------------------------------------------------------------------------------
extern "C" void rvkglClose(void)
{
    if (!g_rvk.initialized) return;
    vkDeviceWaitIdle(g_rvk.device);

    for (auto &frame : g_rvk.frames) {
        rvkFlushDeletionQueue(frame);
        vkDestroyCommandPool(g_rvk.device, frame.cmdPool, nullptr);
        vkDestroySemaphore(g_rvk.device, frame.imageAvailable, nullptr);
        vkDestroySemaphore(g_rvk.device, frame.renderFinished, nullptr);
        vkDestroyFence(g_rvk.device, frame.inFlight, nullptr);
        if (frame.descriptorPool) vkDestroyDescriptorPool(g_rvk.device, frame.descriptorPool, nullptr);
    }
    rvkDestroyPipelineCache();
    for (auto v : g_rvk.swapchainViews) vkDestroyImageView(g_rvk.device, v, nullptr);
    vkDestroySwapchainKHR(g_rvk.device, g_rvk.swapchain, nullptr);
    vmaDestroyAllocator(g_rvk.allocator);
    vkDestroyDevice(g_rvk.device, nullptr);
    vkDestroySurfaceKHR(g_rvk.instance, g_rvk.surface, nullptr);
#if RVK_ENABLE_VALIDATION
    vkb::destroy_debug_utils_messenger(g_rvk.instance, g_rvk.debugMessenger);
#endif
    vkDestroyInstance(g_rvk.instance, nullptr);
    g_rvk.initialized = false;
}
