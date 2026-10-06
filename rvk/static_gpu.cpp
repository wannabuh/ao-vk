// Static geometry on the GPU: vertex buffers the game no longer writes reach the renderer as shared snapshots
// (DrawShared). Each snapshot goes into a chunked arena (M0) at a stable, stride-aligned offset, and its draws read
// the vertices from there instead of copying them into the frame's ring. The arena is what lets an indirect draw
// reference any static mesh's vertices by offset (the GPU-driven step). Indices stay in the ring. The CPU copy stays
// (the snapshot), so everything that looks at a draw's vertices on the CPU (mesh cache, plants, shadow cache) is
// unchanged.
//
// A snapshot the game replaced (it wrote the buffer again) is only held by this cache: dropped soon; others when not
// drawn for a while; their arena slots go back for reuse.
#include "internal.h"

#include <algorithm>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {
constexpr VkDeviceSize kStaticArenaChunk = 32u << 20;
VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a) { return a ? (v + a - 1) / a * a : v; }
}  // namespace

// A slot in a static-arena chunk: a freed slot that fits (re-aligned), else the current chunk's tail, else a new
// chunk. The offset is a multiple of `align` (the FVF stride), so a draw's vertexOffset = offset / stride is exact.
Device::ArenaChunk* Device::ArenaPlace(VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize* offset)
{
    for (auto& c : m_staticArena) {
        for (auto it = c->free.begin(); it != c->free.end(); ++it) {
            VkDeviceSize at = AlignUp(it->first, align), end = it->first + it->second;
            if (at + bytes <= end) {
                *offset = at;
                VkDeviceSize rest = end - (at + bytes);
                if (rest >= 16) { it->first = at + bytes; it->second = rest; }
                else c->free.erase(it);                    // (the < align bytes before `at`, if any, are wasted)
                return c.get();
            }
        }
    }
    if (m_staticArena.empty() || AlignUp(m_staticArena.back()->used, align) + bytes > m_staticArena.back()->size) {
        VkDeviceSize size = std::max(kStaticArenaChunk, bytes);
        auto c = std::make_unique<ArenaChunk>();
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |   // retained indices too
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateBuffer(m_allocator, &bi, &ac, &c->buffer, &c->allocation, nullptr) != VK_SUCCESS)
            return nullptr;
        c->size = size;
        Log("static arena: new chunk, %.1f MB", double(size) / 1048576.0);
        m_staticArena.push_back(std::move(c));
    }
    ArenaChunk* c = m_staticArena.back().get();
    *offset = AlignUp(c->used, align);
    c->used = *offset + bytes;
    return c;
}

// A slot is reused only once every submission that may still read it (the frames in flight, and the one being
// recorded, whose earlier draws may use it) has completed: until then a new snapshot's upload would overwrite vertices
// the GPU is drawing.
void Device::ArenaFree(ArenaChunk* chunk, VkDeviceSize offset, VkDeviceSize bytes)
{
    if (chunk)
        m_deadArenaSlots.push_back({DeathTag(), chunk, offset, bytes});
}

void Device::CollectArenaSlots()
{
    auto done = std::remove_if(m_deadArenaSlots.begin(), m_deadArenaSlots.end(), [&](const DeadArenaSlot& d) {
        if (d.tag > m_completed) return false;
        d.chunk->free.push_back({d.offset, d.bytes});
        return true;
    });
    m_deadArenaSlots.erase(done, m_deadArenaSlots.end());
}

VkBuffer Device::StaticBufferFor(const std::shared_ptr<const std::vector<uint8_t>>& data, VkDeviceSize* baseOffset,
                                 VkDeviceSize align, uint64_t* serial)
{
    if (!m_staticResident || !data || data->empty() || !m_inFrame)
        return VK_NULL_HANDLE;
    StaticGeometry& g = m_staticGeometry[data.get()];
    g.lastFrame = m_frameNumber;
    if (g.chunk && g.data == data) {
        *baseOffset = g.offset;
        *serial = g.serial;
        return g.chunk->buffer;
    }
    if (g.chunk) {                                   // another snapshot at the same address
        ArenaFree(g.chunk, g.offset, g.size);
        m_staticBytes -= g.size;
        g.chunk = nullptr;
    }
    g.data = data;
    VkDeviceSize bytes = AlignUp(VkDeviceSize(data->size()), std::max<VkDeviceSize>(16, align));
    VkDeviceSize offset = 0;
    ArenaChunk* chunk = ArenaPlace(bytes, std::max<VkDeviceSize>(16, align), &offset);
    if (!chunk) {
        g.data.reset();
        return VK_NULL_HANDLE;
    }
    g.chunk = chunk;
    g.offset = offset;
    g.size = bytes;
    g.serial = ++m_staticSerial;
    m_staticBytes += bytes;
    EnsureRingSpace(bytes + 64);
    void* cpu;
    VkDeviceSize staging = Allocate(bytes, 16, &cpu);
    std::memcpy(cpu, data->data(), data->size());
    VkCommandBuffer cmd = UploadCommands();
    VkBufferCopy region{staging, offset, bytes};
    vkCmdCopyBuffer(cmd, m_frames[m_frameIndex].ring, chunk->buffer, 1, &region);
    m_skinUploadsPending = true;                     // the upload buffer's closing barrier covers it
    static bool logged;
    if (!logged) {
        logged = true;
        Log("static geometry kept on the GPU (arena)");
    }
    *baseOffset = offset;
    *serial = g.serial;
    return chunk->buffer;
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
            if (g.chunk) {
                ArenaFree(g.chunk, g.offset, g.size);
                m_staticBytes -= g.size;
            }
            it = m_staticGeometry.erase(it);
        } else {
            ++it;
        }
    }
    if ((m_frameNumber & 4095) == 0)
        Log("static arena: %zu chunks, %zu meshes, %.1f MB", m_staticArena.size(), m_staticGeometry.size(),
            double(m_staticBytes) / 1048576.0);
}

void Device::DestroyStaticGeometry()
{
    for (auto& c : m_staticArena)
        if (c->buffer) vmaDestroyBuffer(m_allocator, c->buffer, c->allocation);
    m_staticArena.clear();
    m_deadArenaSlots.clear();
    m_staticGeometry.clear();
    m_staticBytes = 0;
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
        VkDeviceSize base = 0;
        m_drawStaticBuffer = StaticBufferFor(data, &base, stride, &m_drawStaticSerial);
        m_drawStaticOffset = base + byteOffset;
    }
    Draw(primitive, fvf, data->data() + byteOffset, vertexCount, indices, indexCount);
    m_drawStaticBuffer = VK_NULL_HANDLE;
}

// DrawShared with retained indices (docs/device-on-rvk.md phase 3): the indices are a snapshot the caller keeps while
// they don't change, so they go into the arena once like the vertices - the draw binds them there instead of copying
// them into the ring. Without static vertices (the arena off, or not a world-geometry format) it is DrawShared.
void Device::DrawSharedIndexed(uint32_t primitive, uint32_t fvf, const std::shared_ptr<const std::vector<uint8_t>>& data,
                               size_t byteOffset, uint32_t vertexCount,
                               const std::shared_ptr<const std::vector<uint8_t>>& indexData, uint32_t indexCount)
{
    if (!data || !indexData || indexData->size() < size_t(indexCount) * 2)
        return;
    const uint32_t stride = FvfStride(fvf);
    if (stride && byteOffset % stride == 0 && (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZ) {
        VkDeviceSize base = 0;
        m_drawStaticBuffer = StaticBufferFor(data, &base, stride, &m_drawStaticSerial);
        m_drawStaticOffset = base + byteOffset;
        if (m_drawStaticBuffer) {
            uint64_t serial = 0;
            VkDeviceSize indexBase = 0;
            m_drawStaticIndexBuffer = StaticBufferFor(indexData, &indexBase, 16, &serial);
            m_drawStaticIndexOffset = indexBase;
        }
    }
    Draw(primitive, fvf, data->data() + byteOffset, vertexCount, reinterpret_cast<const uint16_t*>(indexData->data()),
         indexCount);
    m_drawStaticBuffer = VK_NULL_HANDLE;
    m_drawStaticIndexBuffer = VK_NULL_HANDLE;
}

}  // namespace rvk
