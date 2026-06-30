/**********************************************************************************************
*
*   rvk_internal.hpp - Shared internal definitions for the Vulkan backend (C++20)
*
*   Not part of the public API. Included by all src/rvk_*.cpp translation units.
*   Holds the global device context, opaque-handle slot tables, the per-frame deletion
*   queue, the tracked render state (PSO hash key) and cross-module helper declarations.
*
**********************************************************************************************/
#ifndef RVK_INTERNAL_HPP
#define RVK_INTERNAL_HPP

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <vector>
#include <deque>
#include <array>
#include <functional>
#include <unordered_map>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" {
    #include "rvk.h"
}

// raylib provides TraceLog(); declare it weakly so rvk can log through it.
extern "C" void TraceLog(int logLevel, const char *text, ...);

//----------------------------------------------------------------------------------
// Error handling - every VkResult-returning call is wrapped (directive requirement)
//----------------------------------------------------------------------------------
const char *rvkResultString(VkResult r);

#define VK_CHECK(expr)                                                             \
    do {                                                                          \
        VkResult _vk_res = (expr);                                                \
        if (_vk_res != VK_SUCCESS) {                                              \
            TraceLog(RL_LOG_FATAL, "RVK: %s:%d %s -> %s", __FILE__, __LINE__,     \
                     #expr, rvkResultString(_vk_res));                            \
            abort();                                                              \
        }                                                                        \
    } while (0)

//----------------------------------------------------------------------------------
// Configuration
//----------------------------------------------------------------------------------
#ifndef RVK_FRAMES_IN_FLIGHT
    #define RVK_FRAMES_IN_FLIGHT 2          // double-buffered command recording
#endif
#ifndef RVK_MAX_SWAPCHAIN_IMAGES
    #define RVK_MAX_SWAPCHAIN_IMAGES 4
#endif

//----------------------------------------------------------------------------------
// Resource records (indexed by the public unsigned-int handles)
//----------------------------------------------------------------------------------
struct RvkBuffer {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VmaAllocation  alloc  = VK_NULL_HANDLE;
    VkDeviceSize   size   = 0;
    void          *mapped = nullptr;        // non-null when persistently mapped (host-visible)
    bool           inUse  = false;
};

struct RvkTexture {
    VkImage        image  = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
    VmaAllocation  alloc  = VK_NULL_HANDLE;
    VkSampler      sampler = VK_NULL_HANDLE;
    VkFormat       format = VK_FORMAT_UNDEFINED;
    VkImageLayout  layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t       width = 0, height = 0, mipLevels = 1, layers = 1;
    bool           isCubemap = false;
    bool           isDepth = false;
    bool           inUse = false;
};

struct RvkShader {
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
    std::vector<uint32_t> spirv;            // kept for reflection / pipeline assembly
    bool inUse = false;
};

// A linked shader "program" id (mirrors a GL program object)
struct RvkProgram {
    uint32_t vsId = 0, fsId = 0, csId = 0;  // component rvk shader handles
    // name -> reflected binding/offset, filled from SPIR-V reflection
    std::unordered_map<std::string, int> uniformLocs;
    std::unordered_map<std::string, int> attribLocs;
    bool isCompute = false;
    bool inUse = false;
};

struct RvkFramebuffer {                     // dynamic-rendering target set (no VkFramebuffer object)
    std::array<uint32_t, 8> colorTex{};     // texture handles for color attachments
    uint32_t depthTex = 0;
    uint32_t colorCount = 0;
    uint32_t width = 0, height = 0;
    bool inUse = false;
};

//----------------------------------------------------------------------------------
// Tracked render state -> hashed into the PSO cache key (rvk_pipeline.cpp)
//----------------------------------------------------------------------------------
struct RvkRenderState {
    uint32_t shaderProgram = 0;
    int      topology = RL_TRIANGLES;       // RL_LINES / RL_TRIANGLES / RL_QUADS
    bool     blendEnabled = true;
    int      blendMode = RL_BLEND_ALPHA;
    int      customSrcRGB = RL_SRC_ALPHA, customDstRGB = RL_ONE_MINUS_SRC_ALPHA;
    int      customSrcA = RL_SRC_ALPHA, customDstA = RL_ONE_MINUS_SRC_ALPHA;
    int      customEqRGB = RL_FUNC_ADD, customEqA = RL_FUNC_ADD;
    bool     depthTest = false;
    bool     depthMask = true;
    bool     cullEnabled = false;
    int      cullFace = RL_CULL_FACE_BACK;
    bool     colorMask[4] = {true, true, true, true};
    bool     wireMode = false;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;   // current render target formats
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    bool operator==(const RvkRenderState &o) const;
    uint64_t hash() const;
};

//----------------------------------------------------------------------------------
// Per-frame data (frames-in-flight)
//----------------------------------------------------------------------------------
struct RvkFrame {
    VkCommandPool   cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd     = VK_NULL_HANDLE;
    VkSemaphore     imageAvailable = VK_NULL_HANDLE;  // signalled by acquire
    VkSemaphore     renderFinished = VK_NULL_HANDLE;  // signalled by submit, waited by present
    VkFence         inFlight = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE; // reset each frame
    // Deferred destruction: resources retired this frame, freed once `inFlight` signals
    std::deque<std::function<void()>> deletionQueue;
};

//----------------------------------------------------------------------------------
// Global device context
//----------------------------------------------------------------------------------
struct RvkContext {
    // Core (vk-bootstrap owned)
    VkInstance       instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR     surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice         device = VK_NULL_HANDLE;
    VkQueue          graphicsQueue = VK_NULL_HANDLE;
    uint32_t         graphicsQueueFamily = 0;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // Swapchain
    VkSwapchainKHR   swapchain = VK_NULL_HANDLE;
    VkFormat         swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D       swapchainExtent{};
    std::vector<VkImage>     swapchainImages;
    std::vector<VkImageView> swapchainViews;
    uint32_t         currentImageIndex = 0;

    // Default depth target for the main pass
    uint32_t         mainDepthTex = 0;

    // Frames in flight
    std::array<RvkFrame, RVK_FRAMES_IN_FLIGHT> frames;
    uint32_t         frameIndex = 0;
    bool             frameActive = false;        // between rvkBeginFrame/rvkEndFrame (pass open)
    bool             frameAcquired = false;       // a swapchain image is acquired & awaiting present

    // Pipeline cache (rvk_pipeline.cpp)
    VkPipelineCache  pipelineCache = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkPipeline> psoCache;

    // Handle tables (index 0 reserved as "none")
    std::vector<RvkBuffer>      buffers{1};
    std::vector<RvkTexture>     textures{1};
    std::vector<RvkShader>      shaders{1};
    std::vector<RvkProgram>     programs{1};
    std::vector<RvkFramebuffer> framebuffers{1};

    // Defaults
    uint32_t defaultTextureId = 0;               // 1x1 white
    uint32_t defaultShaderId = 0;                // default sprite program
    int defaultShaderLocs[RL_MAX_SHADER_LOCATIONS]{};

    // Live render state + currently bound framebuffer (0 = swapchain)
    RvkRenderState state;
    uint32_t activeFramebuffer = 0;
    int fbWidth = 0, fbHeight = 0;

    // GLFW window (opaque) for surface recreation
    void *window = nullptr;

    bool initialized = false;
};

extern RvkContext g_rvk;                          // single global context

//----------------------------------------------------------------------------------
// Cross-module helpers
//----------------------------------------------------------------------------------
// rvk_context.cpp
VkCommandBuffer rvkBeginSingleTimeCommands();     // one-shot transfer/transition cmd buffer
void rvkEndSingleTimeCommands(VkCommandBuffer cmd);
RvkFrame &rvkCurrentFrame();
void rvkDeferDestroy(std::function<void()> fn);    // push to current frame deletion queue
void rvkImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldL, VkImageLayout newL,
                     VkImageAspectFlags aspect, uint32_t mipLevels = 1, uint32_t layers = 1);

// rvk_memory.cpp
uint32_t rvkAllocBufferSlot();
uint32_t rvkAllocTextureSlot();
VkFormat rvkPixelFormatToVk(int rlFormat);
uint32_t rvkPixelFormatBpp(int rlFormat);

// rvk_pipeline.cpp
VkPipeline rvkGetGraphicsPipeline(const RvkRenderState &state, const rvkVertexBuffer *layout);
void rvkInitDefaultShader();
void rvkDestroyPipelineCache();

#endif // RVK_INTERNAL_HPP
