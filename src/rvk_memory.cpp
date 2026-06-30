/**********************************************************************************************
*
*   rvk_memory.cpp - VMA wrapper: buffers, images, staging, textures, framebuffers
*
*   Implements: pixel-format tables, slot allocators, VMA-backed buffer/image creation,
*   staging uploads, texture load/update/unload/mipmaps/read, vertex-buffer management, and
*   framebuffer (dynamic-rendering target) management. All allocations route through VMA;
*   direct vkAllocateMemory is banned (directive).
*
*   STATUS: format tables + slot allocators + buffer/texture creation implemented; mipmap
*   generation, readback, and framebuffers are P4/P7 scaffolds.
*
**********************************************************************************************/
#include "rvk_internal.hpp"

#include <cstring>

//----------------------------------------------------------------------------------
// Pixel format tables
//----------------------------------------------------------------------------------
VkFormat rvkPixelFormatToVk(int rlFormat)
{
    switch (rlFormat) {
        case RL_PIXELFORMAT_UNCOMPRESSED_GRAYSCALE:    return VK_FORMAT_R8_UNORM;
        case RL_PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA:   return VK_FORMAT_R8G8_UNORM;
        case RL_PIXELFORMAT_UNCOMPRESSED_R5G6B5:       return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8:       return VK_FORMAT_R8G8B8_UNORM;
        case RL_PIXELFORMAT_UNCOMPRESSED_R5G5B5A1:     return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
        case RL_PIXELFORMAT_UNCOMPRESSED_R4G4B4A4:     return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
        case RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8:     return VK_FORMAT_R8G8B8A8_UNORM;
        case RL_PIXELFORMAT_UNCOMPRESSED_R32:          return VK_FORMAT_R32_SFLOAT;
        case RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32:    return VK_FORMAT_R32G32B32_SFLOAT;
        case RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32A32: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case RL_PIXELFORMAT_UNCOMPRESSED_R16:          return VK_FORMAT_R16_SFLOAT;
        case RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16:    return VK_FORMAT_R16G16B16_SFLOAT;
        case RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16A16: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case RL_PIXELFORMAT_COMPRESSED_DXT1_RGB:       return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case RL_PIXELFORMAT_COMPRESSED_DXT1_RGBA:      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case RL_PIXELFORMAT_COMPRESSED_DXT3_RGBA:      return VK_FORMAT_BC2_UNORM_BLOCK;
        case RL_PIXELFORMAT_COMPRESSED_DXT5_RGBA:      return VK_FORMAT_BC3_UNORM_BLOCK;
        case RL_PIXELFORMAT_COMPRESSED_ETC2_RGB:       return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        case RL_PIXELFORMAT_COMPRESSED_ETC2_EAC_RGBA:  return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        case RL_PIXELFORMAT_COMPRESSED_ASTC_4x4_RGBA:  return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
        case RL_PIXELFORMAT_COMPRESSED_ASTC_8x8_RGBA:  return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
        default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

// Bytes per pixel for uncompressed formats (compressed return 0 -> handled separately)
uint32_t rvkPixelFormatBpp(int rlFormat)
{
    switch (rlFormat) {
        case RL_PIXELFORMAT_UNCOMPRESSED_GRAYSCALE:    return 1;
        case RL_PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA:   return 2;
        case RL_PIXELFORMAT_UNCOMPRESSED_R5G6B5:       return 2;
        case RL_PIXELFORMAT_UNCOMPRESSED_R5G5B5A1:     return 2;
        case RL_PIXELFORMAT_UNCOMPRESSED_R4G4B4A4:     return 2;
        case RL_PIXELFORMAT_UNCOMPRESSED_R16:          return 2;
        case RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8:       return 3;
        case RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8:     return 4;
        case RL_PIXELFORMAT_UNCOMPRESSED_R32:          return 4;
        case RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16:    return 6;
        case RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16A16: return 8;
        case RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32:    return 12;
        case RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32A32: return 16;
        default: return 0;   // compressed
    }
}

//----------------------------------------------------------------------------------
// Slot allocators (reuse freed slots; 0 is reserved)
//----------------------------------------------------------------------------------
uint32_t rvkAllocBufferSlot()
{
    for (uint32_t i = 1; i < g_rvk.buffers.size(); i++) if (!g_rvk.buffers[i].inUse) return i;
    g_rvk.buffers.push_back({}); return (uint32_t)g_rvk.buffers.size() - 1;
}
uint32_t rvkAllocTextureSlot()
{
    for (uint32_t i = 1; i < g_rvk.textures.size(); i++) if (!g_rvk.textures[i].inUse) return i;
    g_rvk.textures.push_back({}); return (uint32_t)g_rvk.textures.size() - 1;
}

//----------------------------------------------------------------------------------
// Internal: create a VMA buffer
//----------------------------------------------------------------------------------
static uint32_t rvkCreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memUsage,
                                VmaAllocationCreateFlags flags = 0)
{
    uint32_t id = rvkAllocBufferSlot();
    RvkBuffer &b = g_rvk.buffers[id];
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = memUsage; ai.flags = flags;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(g_rvk.allocator, &bi, &ai, &b.buffer, &b.alloc, &info));
    b.size = size; b.mapped = info.pMappedData; b.inUse = true;
    return id;
}

static void rvkUploadToBuffer(uint32_t bufId, const void *data, VkDeviceSize size, VkDeviceSize offset)
{
    if (!data) return;
    RvkBuffer &b = g_rvk.buffers[bufId];
    if (b.mapped) { memcpy((char*)b.mapped + offset, data, size); return; }
    // staged upload through a host-visible scratch buffer
    uint32_t staging = rvkCreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VMA_MEMORY_USAGE_AUTO,
                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    memcpy(g_rvk.buffers[staging].mapped, data, size);
    VkCommandBuffer cmd = rvkBeginSingleTimeCommands();
    VkBufferCopy region{ 0, offset, size };
    vkCmdCopyBuffer(cmd, g_rvk.buffers[staging].buffer, b.buffer, 1, &region);
    rvkEndSingleTimeCommands(cmd);
    vmaDestroyBuffer(g_rvk.allocator, g_rvk.buffers[staging].buffer, g_rvk.buffers[staging].alloc);
    g_rvk.buffers[staging] = {};
}

extern "C" {

//----------------------------------------------------------------------------------
// Textures
//----------------------------------------------------------------------------------
unsigned int rvkLoadTexture(const void *data, int width, int height, int format, int mipmapCount)
{
    uint32_t id = rvkAllocTextureSlot();
    RvkTexture &t = g_rvk.textures[id];
    t.format = rvkPixelFormatToVk(format);
    t.width = (uint32_t)width; t.height = (uint32_t)height;
    t.mipLevels = (uint32_t)((mipmapCount > 0) ? mipmapCount : 1);
    t.inUse = true;

    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = t.format;
    ici.extent = { t.width, t.height, 1 };
    ici.mipLevels = t.mipLevels; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{}; ai.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(g_rvk.allocator, &ici, &ai, &t.image, &t.alloc, nullptr));

    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = t.image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = t.format;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, t.mipLevels, 0, 1 };
    VK_CHECK(vkCreateImageView(g_rvk.device, &vci, nullptr, &t.view));

    // default sampler (filter/wrap adjusted by rvkTextureParameters)
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = (float)t.mipLevels;
    VK_CHECK(vkCreateSampler(g_rvk.device, &sci, nullptr, &t.sampler));

    if (data) {
        uint32_t bpp = rvkPixelFormatBpp(format);
        VkDeviceSize size = (VkDeviceSize)width * height * (bpp ? bpp : 4);
        uint32_t staging = rvkCreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        memcpy(g_rvk.buffers[staging].mapped, data, size);
        VkCommandBuffer cmd = rvkBeginSingleTimeCommands();
        rvkImageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, t.mipLevels);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { t.width, t.height, 1 };
        vkCmdCopyBufferToImage(cmd, g_rvk.buffers[staging].buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        rvkImageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, t.mipLevels);
        rvkEndSingleTimeCommands(cmd);
        vmaDestroyBuffer(g_rvk.allocator, g_rvk.buffers[staging].buffer, g_rvk.buffers[staging].alloc);
        g_rvk.buffers[staging] = {};
        t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    return id;
}

unsigned int rvkLoadTextureDepth(int width, int height, bool useRenderBuffer)
{
    (void)useRenderBuffer;
    uint32_t id = rvkAllocTextureSlot();
    RvkTexture &t = g_rvk.textures[id];
    t.format = VK_FORMAT_D32_SFLOAT; t.isDepth = true; t.inUse = true;
    t.width = (uint32_t)width; t.height = (uint32_t)height;
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = t.format; ici.extent = { t.width, t.height, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo ai{}; ai.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(g_rvk.allocator, &ici, &ai, &t.image, &t.alloc, nullptr));
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = t.image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = t.format;
    vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    VK_CHECK(vkCreateImageView(g_rvk.device, &vci, nullptr, &t.view));
    return id;
}

unsigned int rvkLoadTextureCubemap(const void *data, int size, int format, int mipmapCount)
{ (void)data; (void)size; (void)format; (void)mipmapCount; /* TODO(P4): 6-layer cube image */ return 0; }

void rvkUpdateTexture(unsigned int id, int offsetX, int offsetY, int width, int height, int format, const void *data)
{
    if (id == 0 || id >= g_rvk.textures.size() || !data) return;
    RvkTexture &t = g_rvk.textures[id];
    uint32_t bpp = rvkPixelFormatBpp(format); if (!bpp) bpp = 4;
    VkDeviceSize size = (VkDeviceSize)width * height * bpp;
    uint32_t staging = rvkCreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    memcpy(g_rvk.buffers[staging].mapped, data, size);
    VkCommandBuffer cmd = rvkBeginSingleTimeCommands();
    rvkImageBarrier(cmd, t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, t.mipLevels);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset = { offsetX, offsetY, 0 };
    region.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
    vkCmdCopyBufferToImage(cmd, g_rvk.buffers[staging].buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    rvkImageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, t.mipLevels);
    rvkEndSingleTimeCommands(cmd);
    t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vmaDestroyBuffer(g_rvk.allocator, g_rvk.buffers[staging].buffer, g_rvk.buffers[staging].alloc);
    g_rvk.buffers[staging] = {};
}

void rvkUnloadTexture(unsigned int id)
{
    if (id == 0 || id >= g_rvk.textures.size() || !g_rvk.textures[id].inUse) return;
    RvkTexture t = g_rvk.textures[id];
    rvkDeferDestroy([t]{
        if (t.sampler) vkDestroySampler(g_rvk.device, t.sampler, nullptr);
        if (t.view) vkDestroyImageView(g_rvk.device, t.view, nullptr);
        if (t.image) vmaDestroyImage(g_rvk.allocator, t.image, t.alloc);
    });
    g_rvk.textures[id] = {};
}

void rvkGenTextureMipmaps(unsigned int id, int width, int height, int format, int *mipmaps)
{ (void)id; (void)width; (void)height; (void)format; if (mipmaps) *mipmaps = 1; /* TODO(P4): vkCmdBlitImage chain */ }

void *rvkReadTexturePixels(unsigned int id, int width, int height, int format)
{ (void)id; (void)width; (void)height; (void)format; /* TODO(P4): copy image->host buffer */ return nullptr; }

unsigned char *rvkReadScreenPixels(int width, int height)
{ (void)width; (void)height; /* TODO(P4): copy swapchain image->host (used for headless capture) */ return nullptr; }

void rvkGetGlTextureFormats(int format, unsigned int *glInternalFormat, unsigned int *glFormat, unsigned int *glType)
{ (void)format; if (glInternalFormat) *glInternalFormat = 0; if (glFormat) *glFormat = 0; if (glType) *glType = 0; }

const char *rvkGetPixelFormatName(unsigned int format)
{
    switch (format) {
        case RL_PIXELFORMAT_UNCOMPRESSED_GRAYSCALE: return "GRAYSCALE";
        case RL_PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA: return "GRAY_ALPHA";
        case RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8: return "R8G8B8";
        case RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8: return "R8G8B8A8";
        case RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32A32: return "R32G32B32A32";
        default: return "UNKNOWN";
    }
}

//----------------------------------------------------------------------------------
// Texture state
//----------------------------------------------------------------------------------
void rvkActiveTextureSlot(int slot) { (void)slot; /* TODO(P5): multi-texture binding index */ }
void rvkEnableTexture(unsigned int id) { rvkSetTexture(id); }
void rvkDisableTexture(void) { rvkSetTexture(g_rvk.defaultTextureId); }
void rvkEnableTextureCubemap(unsigned int id) { (void)id; /* TODO(P4) */ }
void rvkDisableTextureCubemap(void) {}
void rvkTextureParameters(unsigned int id, int param, int value) { (void)id; (void)param; (void)value; /* TODO(P4): recreate sampler */ }
void rvkCubemapParameters(unsigned int id, int param, int value) { (void)id; (void)param; (void)value; }

//----------------------------------------------------------------------------------
// Shader state
//----------------------------------------------------------------------------------
void rvkEnableShader(unsigned int id) { if (id < g_rvk.programs.size()) g_rvk.state.shaderProgram = id; }
void rvkDisableShader(void) { g_rvk.state.shaderProgram = g_rvk.defaultShaderId; }

//----------------------------------------------------------------------------------
// Vertex buffers
//----------------------------------------------------------------------------------
unsigned int rvkLoadVertexArray(void) { return 0; }   // no VAO in Vulkan; layout tracked per-pipeline
unsigned int rvkLoadVertexBuffer(const void *buffer, int size, bool dynamic)
{
    uint32_t id = rvkCreateBuffer((VkDeviceSize)size,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO,
        dynamic ? (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT) : 0);
    if (buffer) rvkUploadToBuffer(id, buffer, (VkDeviceSize)size, 0);
    return id;
}
unsigned int rvkLoadVertexBufferElement(const void *buffer, int size, bool dynamic)
{
    uint32_t id = rvkCreateBuffer((VkDeviceSize)size,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO,
        dynamic ? (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT) : 0);
    if (buffer) rvkUploadToBuffer(id, buffer, (VkDeviceSize)size, 0);
    return id;
}
void rvkUpdateVertexBuffer(unsigned int bufferId, const void *data, int dataSize, int offset)
{ if (bufferId && bufferId < g_rvk.buffers.size()) rvkUploadToBuffer(bufferId, data, (VkDeviceSize)dataSize, (VkDeviceSize)offset); }
void rvkUpdateVertexBufferElements(unsigned int id, const void *data, int dataSize, int offset)
{ rvkUpdateVertexBuffer(id, data, dataSize, offset); }
void rvkUnloadVertexArray(unsigned int vaoId) { (void)vaoId; }
void rvkUnloadVertexBuffer(unsigned int vboId)
{
    if (vboId == 0 || vboId >= g_rvk.buffers.size() || !g_rvk.buffers[vboId].inUse) return;
    RvkBuffer b = g_rvk.buffers[vboId];
    rvkDeferDestroy([b]{ vmaDestroyBuffer(g_rvk.allocator, b.buffer, b.alloc); });
    g_rvk.buffers[vboId] = {};
}
void rvkSetVertexAttribute(unsigned int index, int compSize, int type, bool normalized, int stride, int offset)
{ (void)index; (void)compSize; (void)type; (void)normalized; (void)stride; (void)offset; /* TODO(P6): record into vertex-layout descriptor for PSO */ }
void rvkSetVertexAttributeDivisor(unsigned int index, int divisor) { (void)index; (void)divisor; /* TODO(P8): instancing */ }
void rvkSetVertexAttributeDefault(int locIndex, const void *value, int attribType, int count) { (void)locIndex; (void)value; (void)attribType; (void)count; }
void rvkDrawVertexArray(int offset, int count) { (void)offset; (void)count; /* TODO(P6) */ }
void rvkDrawVertexArrayElements(int offset, int count, const void *buffer) { (void)offset; (void)count; (void)buffer; /* TODO(P6) */ }
void rvkDrawVertexArrayInstanced(int offset, int count, int instances) { (void)offset; (void)count; (void)instances; /* TODO(P8) */ }
void rvkDrawVertexArrayElementsInstanced(int offset, int count, const void *buffer, int instances) { (void)offset; (void)count; (void)buffer; (void)instances; /* TODO(P8) */ }

bool rvkEnableVertexArray(unsigned int vaoId) { (void)vaoId; return false; }
void rvkDisableVertexArray(void) {}
void rvkEnableVertexBuffer(unsigned int id) { (void)id; }
void rvkDisableVertexBuffer(void) {}
void rvkEnableVertexBufferElement(unsigned int id) { (void)id; }
void rvkDisableVertexBufferElement(void) {}
void rvkEnableVertexAttribute(unsigned int index) { (void)index; }
void rvkDisableVertexAttribute(unsigned int index) { (void)index; }

//----------------------------------------------------------------------------------
// Framebuffers (dynamic-rendering target sets)
//----------------------------------------------------------------------------------
unsigned int rvkLoadFramebuffer(void)
{
    g_rvk.framebuffers.push_back({});
    uint32_t id = (uint32_t)g_rvk.framebuffers.size() - 1;
    g_rvk.framebuffers[id].inUse = true;
    return id;
}
void rvkFramebufferAttach(unsigned int id, unsigned int texId, int attachType, int texType, int mipLevel)
{
    (void)texType; (void)mipLevel;
    if (id == 0 || id >= g_rvk.framebuffers.size()) return;
    RvkFramebuffer &fb = g_rvk.framebuffers[id];
    if (attachType == RL_ATTACHMENT_DEPTH) fb.depthTex = texId;
    else if (attachType >= RL_ATTACHMENT_COLOR_CHANNEL0 && attachType <= RL_ATTACHMENT_COLOR_CHANNEL7) {
        fb.colorTex[attachType] = texId;
        if ((uint32_t)(attachType + 1) > fb.colorCount) fb.colorCount = attachType + 1;
    }
    if (texId < g_rvk.textures.size()) { fb.width = g_rvk.textures[texId].width; fb.height = g_rvk.textures[texId].height; }
}
bool rvkFramebufferComplete(unsigned int id) { return id != 0 && id < g_rvk.framebuffers.size() && g_rvk.framebuffers[id].inUse; }
void rvkUnloadFramebuffer(unsigned int id) { if (id && id < g_rvk.framebuffers.size()) g_rvk.framebuffers[id] = {}; }
void rvkCopyFramebuffer(int x, int y, int width, int height, int format, void *pixels) { (void)x; (void)y; (void)width; (void)height; (void)format; (void)pixels; /* TODO(P7) */ }
void rvkResizeFramebuffer(int width, int height) { (void)width; (void)height; /* TODO(P7) */ }

void rvkEnableFramebuffer(unsigned int id) { g_rvk.activeFramebuffer = id; /* TODO(P7): switch dynamic-rendering target at next draw */ }
void rvkDisableFramebuffer(void) { g_rvk.activeFramebuffer = 0; }
unsigned int rvkGetActiveFramebuffer(void) { return g_rvk.activeFramebuffer; }
void rvkActiveDrawBuffers(int count) { (void)count; }
void rvkBlitFramebuffer(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, int mask) { (void)sx;(void)sy;(void)sw;(void)sh;(void)dx;(void)dy;(void)dw;(void)dh;(void)mask; /* TODO(P7): vkCmdBlitImage */ }
void rvkBindFramebuffer(unsigned int target, unsigned int framebuffer) { (void)target; g_rvk.activeFramebuffer = framebuffer; }

} // extern "C"
