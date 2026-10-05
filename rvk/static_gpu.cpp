// Static geometry on the GPU: vertex buffers the game no longer writes reach the renderer as shared snapshots
// (DrawShared). Each snapshot is uploaded once into a buffer of its own in video memory, and its draws read the
// vertices from there instead of copying them into the frame's ring every time. The CPU copy stays (the snapshot), so
// everything that looks at a draw's vertices on the CPU (mesh cache, plants, shadow cache) works as before.
//
// A snapshot the game replaced (it wrote the buffer again) is only held by this cache: dropped soon; others when not
// drawn for a while.
#include "internal.h"

#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

VkBuffer Device::StaticBufferFor(const std::shared_ptr<const std::vector<uint8_t>>& data)
{
    if (!m_staticResident || !data || data->empty() || !m_inFrame)
        return VK_NULL_HANDLE;
    StaticGeometry& g = m_staticGeometry[data.get()];
    g.lastFrame = m_frameNumber;
    if (g.buffer && g.data == data)
        return g.buffer;
    if (g.buffer) {                                  // another snapshot at the same address
        m_deadBuffers.push_back({DeathTag(), {g.buffer, g.allocation}});
        m_staticBytes -= g.data ? g.data->size() : 0;
        g.buffer = VK_NULL_HANDLE;
    }
    g.data = data;
    VkDeviceSize bytes = (VkDeviceSize(data->size()) + 15) & ~VkDeviceSize(15);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateBuffer(m_allocator, &bi, &ac, &g.buffer, &g.allocation, nullptr) != VK_SUCCESS) {
        g.buffer = VK_NULL_HANDLE;
        g.data.reset();
        return VK_NULL_HANDLE;
    }
    m_staticBytes += data->size();
    EnsureRingSpace(bytes + 64);
    void* cpu;
    VkDeviceSize staging = Allocate(bytes, 16, &cpu);
    std::memcpy(cpu, data->data(), data->size());
    VkCommandBuffer cmd = UploadCommands();
    VkBufferCopy region{staging, 0, bytes};
    vkCmdCopyBuffer(cmd, m_frames[m_frameIndex].ring, g.buffer, 1, &region);
    m_skinUploadsPending = true;                     // the upload buffer's closing barrier covers it
    static bool logged;
    if (!logged) {
        logged = true;
        Log("static geometry kept on the GPU");
    }
    return g.buffer;
}

// The indices of a static mesh (D3D7 has no index buffers: the game passes a pointer), uploaded once and kept while
// the mesh is drawn, so a static draw doesn't copy them into the ring every frame.
VkBuffer Device::StaticIndexBufferFor(const uint16_t* indices, uint32_t indexCount)
{
    if (!m_staticResident || !indices || !indexCount || !m_inFrame)
        return VK_NULL_HANDLE;
    StaticIndices& s = m_staticIndices[indices];
    s.lastFrame = m_frameNumber;
    if (s.buffer && s.count == indexCount)
        return s.buffer;
    if (s.buffer) {                                  // another, differently sized range at the same address
        m_deadBuffers.push_back({DeathTag(), {s.buffer, s.allocation}});
        m_staticIndexBytes -= VkDeviceSize(s.count) * 2;
        s.buffer = VK_NULL_HANDLE;
    }
    VkDeviceSize bytes = (VkDeviceSize(indexCount) * 2 + 15) & ~VkDeviceSize(15);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateBuffer(m_allocator, &bi, &ac, &s.buffer, &s.allocation, nullptr) != VK_SUCCESS) {
        s.buffer = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    s.count = indexCount;
    m_staticIndexBytes += VkDeviceSize(indexCount) * 2;
    EnsureRingSpace(bytes + 64);
    void* cpu;
    VkDeviceSize staging = Allocate(bytes, 16, &cpu);
    std::memcpy(cpu, indices, size_t(indexCount) * 2);
    VkCommandBuffer cmd = UploadCommands();
    VkBufferCopy region{staging, 0, bytes};
    vkCmdCopyBuffer(cmd, m_frames[m_frameIndex].ring, s.buffer, 1, &region);
    m_skinUploadsPending = true;
    return s.buffer;
}

void Device::BeginStaticFrame()
{
    if ((m_frameNumber & 63) != 0)
        return;
    for (auto it = m_staticGeometry.begin(); it != m_staticGeometry.end();) {
        StaticGeometry& g = it->second;
        // Ours alone (the game replaced it) and not drawn for two frames, or not drawn for ten seconds or so.
        bool replaced = g.data && g.data.use_count() == 1 && g.lastFrame + 2 < m_frameNumber;
        if (replaced || g.lastFrame + 1200 < m_frameNumber) {
            if (g.buffer) m_deadBuffers.push_back({DeathTag(), {g.buffer, g.allocation}});
            m_staticBytes -= g.data ? g.data->size() : 0;
            it = m_staticGeometry.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_staticIndices.begin(); it != m_staticIndices.end();) {
        StaticIndices& s = it->second;
        if (s.lastFrame + 1200 < m_frameNumber) {
            if (s.buffer) m_deadBuffers.push_back({DeathTag(), {s.buffer, s.allocation}});
            m_staticIndexBytes -= VkDeviceSize(s.count) * 2;
            it = m_staticIndices.erase(it);
        } else {
            ++it;
        }
    }
    if ((m_frameNumber & 4095) == 0)
        Log("static geometry on the GPU: %zu buffers, %.1f MB; %zu index buffers, %.1f MB", m_staticGeometry.size(),
            double(m_staticBytes) / 1048576.0, m_staticIndices.size(), double(m_staticIndexBytes) / 1048576.0);
}

void Device::DestroyStaticGeometry()
{
    for (auto& [key, g] : m_staticGeometry)
        if (g.buffer) vmaDestroyBuffer(m_allocator, g.buffer, g.allocation);
    m_staticGeometry.clear();
    m_staticBytes = 0;
    for (auto& [key, s] : m_staticIndices)
        if (s.buffer) vmaDestroyBuffer(m_allocator, s.buffer, s.allocation);
    m_staticIndices.clear();
    m_staticIndexBytes = 0;
}

void Device::DrawShared(uint32_t primitive, uint32_t fvf, const std::shared_ptr<const std::vector<uint8_t>>& data,
                        size_t byteOffset, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount)
{
    if (!data)
        return;
    const uint32_t stride = FvfStride(fvf);
    // Only whole vertices from the buffer's start (vertex offsets in the draw), and 3D world geometry - not what the
    // shaders treat specially by format (pre-transformed interface quads come through the ring as before).
    if (stride && byteOffset % stride == 0 && (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZ) {
        m_drawStaticBuffer = StaticBufferFor(data);
        m_drawStaticOffset = byteOffset;
        if (m_drawStaticBuffer && indices && indexCount)
            m_drawStaticIb = StaticIndexBufferFor(indices, indexCount);
    }
    Draw(primitive, fvf, data->data() + byteOffset, vertexCount, indices, indexCount);
    m_drawStaticBuffer = VK_NULL_HANDLE;
    m_drawStaticIb = VK_NULL_HANDLE;
}

}  // namespace rvk
