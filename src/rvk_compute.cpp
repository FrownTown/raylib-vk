/**********************************************************************************************
*
*   rvk_compute.cpp - Compute pipelines, dispatch, SSBO management, image binding
*
*   Implements the compute path: SSBO (shader storage buffer) create/update/read/copy/bind,
*   image-texture binding for compute, and compute dispatch with sync2 barriers between
*   compute and graphics work.
*
*   STATUS: P8 scaffold. SSBO buffer creation/update/copy are implemented (VMA); compute
*   pipeline creation, dispatch recording and barriers are marked TODO(P8).
*
**********************************************************************************************/
#include "rvk_internal.hpp"

#include <cstring>

// from rvk_memory.cpp
extern uint32_t rvkAllocBufferSlot();

extern "C" {

//----------------------------------------------------------------------------------
// Compute dispatch
//----------------------------------------------------------------------------------
void rvkComputeShaderDispatch(unsigned int groupX, unsigned int groupY, unsigned int groupZ)
{
    (void)groupX; (void)groupY; (void)groupZ;
    // TODO(P8): bind the active compute pipeline + descriptor set (SSBOs/images bound via
    //   rvkBindShaderBuffer/rvkBindImageTexture), insert a sync2 barrier, then vkCmdDispatch
    //   on a (graphics-capable) command buffer; barrier results for subsequent graphics reads.
}

//----------------------------------------------------------------------------------
// Shader storage buffers (SSBO)
//----------------------------------------------------------------------------------
unsigned int rvkLoadShaderBuffer(unsigned int size, const void *data, int usageHint)
{
    (void)usageHint;
    uint32_t id = rvkAllocBufferSlot();
    RvkBuffer &b = g_rvk.buffers[id];
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(g_rvk.allocator, &bi, &ai, &b.buffer, &b.alloc, &info));
    b.size = size; b.mapped = info.pMappedData; b.inUse = true;
    if (data && b.mapped) memcpy(b.mapped, data, size);
    return id;
}

void rvkUnloadShaderBuffer(unsigned int ssboId)
{
    if (ssboId == 0 || ssboId >= g_rvk.buffers.size() || !g_rvk.buffers[ssboId].inUse) return;
    RvkBuffer b = g_rvk.buffers[ssboId];
    rvkDeferDestroy([b]{ vmaDestroyBuffer(g_rvk.allocator, b.buffer, b.alloc); });
    g_rvk.buffers[ssboId] = {};
}

void rvkUpdateShaderBuffer(unsigned int id, const void *data, unsigned int dataSize, unsigned int offset)
{
    if (id == 0 || id >= g_rvk.buffers.size() || !data) return;
    RvkBuffer &b = g_rvk.buffers[id];
    if (b.mapped) memcpy((char*)b.mapped + offset, data, dataSize);
}

void rvkBindShaderBuffer(unsigned int id, unsigned int index)
{ (void)id; (void)index; /* TODO(P8): record into compute/graphics descriptor set at binding=index */ }

void rvkReadShaderBuffer(unsigned int id, void *dest, unsigned int count, unsigned int offset)
{
    if (id == 0 || id >= g_rvk.buffers.size() || !dest) return;
    RvkBuffer &b = g_rvk.buffers[id];
    if (b.mapped) memcpy(dest, (char*)b.mapped + offset, count);
    // TODO(P8): for device-local SSBOs, stage through a host-visible readback buffer.
}

void rvkCopyShaderBuffer(unsigned int destId, unsigned int srcId, unsigned int destOffset, unsigned int srcOffset, unsigned int count)
{
    if (destId >= g_rvk.buffers.size() || srcId >= g_rvk.buffers.size()) return;
    VkCommandBuffer cmd = rvkBeginSingleTimeCommands();
    VkBufferCopy region{ srcOffset, destOffset, count };
    vkCmdCopyBuffer(cmd, g_rvk.buffers[srcId].buffer, g_rvk.buffers[destId].buffer, 1, &region);
    rvkEndSingleTimeCommands(cmd);
}

unsigned int rvkGetShaderBufferSize(unsigned int id)
{ return (id < g_rvk.buffers.size()) ? (unsigned int)g_rvk.buffers[id].size : 0; }

//----------------------------------------------------------------------------------
// Image binding (compute image load/store)
//----------------------------------------------------------------------------------
void rvkBindImageTexture(unsigned int id, unsigned int index, int format, bool readonly)
{ (void)id; (void)index; (void)format; (void)readonly; /* TODO(P8): bind as storage image descriptor */ }

} // extern "C"
