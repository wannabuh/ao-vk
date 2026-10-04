// GPU skinning of the game's characters (shaders/skin.comp). A skinned piece (skin::Job: the mesh, the character's
// bones) drawn with DrawSkinned is skinned by a compute dispatch recorded into the frame's upload command buffer -
// which the GPU runs before the frame's main pass - into the frame's skin arena; the draw then reads its vertices
// from there instead of the ring. Nothing per vertex happens on the CPU: the mesh (vertices, the groups of vertices
// sharing a position, indices) is uploaded once, the bones per draw are a few hundred bytes.
//
// What the CPU-side features read from vertices comes from elsewhere for these draws: the box from the job
// (DrawMeshInfo), last frame's positions for motion vectors from the same dispatch (last frame's bones), the smooth
// normals for Phong tessellation from a second dispatch when the draw is tessellated.
#include "internal.h"

#include <algorithm>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kSkinSpirv[] = {
#include "skin.comp.inc"
};

constexpr uint32_t kGroupSize = 64;
constexpr VkDeviceSize kArenaAlign = 256;            // >= any minStorageBufferOffsetAlignment
constexpr VkDeviceSize kArenaInitial = 16u << 20, kArenaMax = 256u << 20;

struct SkinPush {
    uint32_t count, boneCount, prevBoneCount, flags;
    uint32_t outVertex, outPrev, outSmooth, membersBase;
};
static_assert(sizeof(SkinPush) == 32, "skin.comp push constants");

VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a)
{
    return (v + a - 1) / a * a;
}

}  // namespace

bool Device::CreateSkinResources(std::string* error)
{
    VkDescriptorSetLayoutBinding b[5];
    for (uint32_t i = 0; i < 5; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 5;
    sl.pBindings = b;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_skinSetLayout), "skin set layout", error))
        return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPush)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_skinSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_skinLayout), "skin pipeline layout", error))
        return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(kSkinSpirv);
    mi.pCode = kSkinSpirv;
    VkShaderModule module;
    if (!Check(vkCreateShaderModule(m_device, &mi, nullptr, &module), "skin shader module", error))
        return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main",
                nullptr};
    ci.layout = m_skinLayout;
    bool ok = Check(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_skinPipeline), "skin pipeline",
                    error);
    vkDestroyShaderModule(m_device, module, nullptr);
    m_skinArenaWanted = kArenaInitial;
    return ok;
}

void Device::DestroySkinResources()
{
    for (auto& [source, mesh] : m_skinMeshes)
        if (mesh.buffer) vmaDestroyBuffer(m_allocator, mesh.buffer, mesh.allocation);
    m_skinMeshes.clear();
    for (Frame& f : m_frames)
        if (f.skinArena) vmaDestroyBuffer(m_allocator, f.skinArena, f.skinArenaAllocation);
    if (m_skinPipeline) vkDestroyPipeline(m_device, m_skinPipeline, nullptr);
    if (m_skinLayout) vkDestroyPipelineLayout(m_device, m_skinLayout, nullptr);
    if (m_skinSetLayout) vkDestroyDescriptorSetLayout(m_device, m_skinSetLayout, nullptr);
}

// At a frame's start (its slot idle): the arena at the wanted size, emptied; meshes not drawn for a while freed.
void Device::BeginSkinFrame()
{
    Frame& f = m_frames[m_frameIndex];
    f.skinArenaOffset = 0;
    m_skinOutputs.clear();
    m_skinBonesAt.clear();
    if (m_gpuSkin && (!f.skinArena || f.skinArenaSize < m_skinArenaWanted)) {
        if (f.skinArena) vmaDestroyBuffer(m_allocator, f.skinArena, f.skinArenaAllocation);
        f.skinArena = VK_NULL_HANDLE;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = m_skinArenaWanted;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateBuffer(m_allocator, &bi, &ac, &f.skinArena, &f.skinArenaAllocation, nullptr) == VK_SUCCESS) {
            f.skinArenaSize = m_skinArenaWanted;
            if (m_skinArenaWanted > kArenaInitial)
                Log("skin arena: %llu MB", (unsigned long long)(m_skinArenaWanted >> 20));
        } else {
            f.skinArena = VK_NULL_HANDLE;
            f.skinArenaSize = 0;
            Log("skin arena: no memory for %llu MB - characters skinned on the CPU",
                (unsigned long long)(m_skinArenaWanted >> 20));
            m_skinArenaWanted = kArenaInitial;
        }
    }
    if ((m_frameNumber & 255) == 0)
        for (auto it = m_skinMeshes.begin(); it != m_skinMeshes.end();) {
            if (it->second.lastFrame + 600 < m_frameNumber) {
                m_deadBuffers.push_back({DeathTag(), {it->second.buffer, it->second.allocation}});
                it = m_skinMeshes.erase(it);
            } else {
                ++it;
            }
        }
}

// The mesh on the GPU (uploaded the first time): vertices, groups, members, indices in one buffer.
Device::SkinMesh* Device::SkinMeshFor(const std::shared_ptr<const skin::Source>& source)
{
    SkinMesh& m = m_skinMeshes[source.get()];
    m.lastFrame = m_frameNumber;
    if (m.source == source && m.buffer)
        return &m;
    if (m.buffer) {                                  // another mesh at the same address
        m_deadBuffers.push_back({DeathTag(), {m.buffer, m.allocation}});
        m.buffer = VK_NULL_HANDLE;
    }
    m.source = source;
    const std::vector<skin::TriVertex>& in = source->vertices;
    const uint32_t n = uint32_t(in.size());
    // The vertices sharing a position (as SmoothNormalsSkinned finds them): per vertex, its group in the member list.
    std::vector<int32_t> owner(n);
    {
        uint32_t size = 64;
        while (size < n * 2) size *= 2;
        std::vector<int32_t> table(size, -1);
        auto quant = [](float f) { return int32_t(std::lround(f * 2048.0f)); };
        for (uint32_t i = 0; i < n; ++i) {
            int32_t q[3] = {quant(in[i].bind[0]), quant(in[i].bind[1]), quant(in[i].bind[2])};
            uint32_t h = (uint32_t(q[0]) * 73856093u) ^ (uint32_t(q[1]) * 19349663u) ^ (uint32_t(q[2]) * 83492791u);
            int32_t found = -1;
            for (uint32_t slot = h & (size - 1);; slot = (slot + 1) & (size - 1)) {
                int32_t o = table[slot];
                if (o < 0) { table[slot] = int32_t(i); break; }
                if (quant(in[o].bind[0]) == q[0] && quant(in[o].bind[1]) == q[1] && quant(in[o].bind[2]) == q[2]) {
                    found = o;
                    break;
                }
            }
            owner[i] = found < 0 ? int32_t(i) : found;
        }
    }
    std::vector<uint32_t> groupCount(n, 0), groupFirst(n, 0), members(n);
    for (uint32_t i = 0; i < n; ++i) ++groupCount[uint32_t(owner[i])];
    for (uint32_t i = 0, at = 0; i < n; ++i) { groupFirst[i] = at; at += groupCount[i]; }
    {
        std::vector<uint32_t> fill(groupFirst);
        for (uint32_t i = 0; i < n; ++i) members[fill[uint32_t(owner[i])]++] = i;
    }
    // Layout: vertices | per vertex (first, count) of its owner's group | members | indices.
    m.groupsOffset = AlignUp(VkDeviceSize(n) * sizeof(skin::TriVertex), 16);
    m.membersBase = n * 2;
    m.indexOffset = AlignUp(m.groupsOffset + VkDeviceSize(n) * 3 * 4, 16);
    VkDeviceSize bytes = m.indexOffset + VkDeviceSize(source->indices.size()) * 2 + 4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateBuffer(m_allocator, &bi, &ac, &m.buffer, &m.allocation, nullptr) != VK_SUCCESS) {
        m.buffer = VK_NULL_HANDLE;
        m.source.reset();
        return nullptr;
    }
    // Staged through the ring, copied by the upload command buffer.
    EnsureRingSpace(bytes + 64);
    void* cpu;
    VkDeviceSize staging = Allocate(bytes, 16, &cpu);
    auto* dst = static_cast<uint8_t*>(cpu);
    std::memcpy(dst, in.data(), size_t(n) * sizeof(skin::TriVertex));
    auto* groups = reinterpret_cast<uint32_t*>(dst + m.groupsOffset);
    for (uint32_t i = 0; i < n; ++i) {
        groups[i * 2] = groupFirst[uint32_t(owner[i])];
        groups[i * 2 + 1] = groupCount[uint32_t(owner[i])];
    }
    std::memcpy(groups + n * 2, members.data(), size_t(n) * 4);
    if (!source->indices.empty())
        std::memcpy(dst + m.indexOffset, source->indices.data(), source->indices.size() * 2);
    VkCommandBuffer cmd = UploadCommands();
    VkBufferCopy region{staging, 0, bytes};
    vkCmdCopyBuffer(cmd, m_frames[m_frameIndex].ring, m.buffer, 1, &region);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    m_skinUploadsPending = true;
    return &m;
}

// Space in the frame's arena; false when full (the arena grows from the next frame on).
bool Device::SkinArenaAlloc(VkDeviceSize bytes, VkDeviceSize* offset)
{
    Frame& f = m_frames[m_frameIndex];
    VkDeviceSize at = AlignUp(f.skinArenaOffset, kArenaAlign);
    if (!f.skinArena || at + bytes > f.skinArenaSize) {
        m_skinArenaWanted = std::min(std::max(m_skinArenaWanted, f.skinArenaSize * 2), kArenaMax);
        return false;
    }
    f.skinArenaOffset = at + bytes;
    *offset = at;
    return true;
}

// The bones, as skin.comp reads them (three rows per bone), into the ring.
VkDeviceSize Device::SkinBones(const skin::Palette& bones, VkDeviceSize* bytes)
{
    // A character's pieces share their bones: written once per frame (and ring restart).
    auto found = m_skinBonesAt.find(&bones);
    if (found != m_skinBonesAt.end() && found->second.generation == m_ringGeneration) {
        *bytes = found->second.bytes;
        return found->second.offset;
    }
    *bytes = std::max<VkDeviceSize>(VkDeviceSize(bones.count) * 48, 48);
    void* cpu;
    VkDeviceSize offset = Allocate(*bytes, m_props.limits.minStorageBufferOffsetAlignment, &cpu);
    auto* rows = static_cast<float*>(cpu);
    for (uint32_t b = 0; b < bones.count; ++b)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                rows[b * 12 + r * 4 + c] = bones.bones[b].c[c][r];
    if (!bones.count) std::memset(rows, 0, 48);
    m_skinBonesAt[&bones] = {offset, *bytes, m_ringGeneration};
    return offset;
}

void Device::SkinDispatch(SkinMesh& mesh, const skin::Job& job, uint32_t flags, const SkinOutput& out)
{
    const uint32_t n = uint32_t(job.source->vertices.size());
    VkDeviceSize boneBytes, prevBytes;
    VkDeviceSize boneOffset = SkinBones(*job.bones, &boneBytes);
    const skin::Palette& prev = job.prevBones ? *job.prevBones : *job.bones;
    VkDeviceSize prevOffset = (flags & 2u) ? SkinBones(prev, &prevBytes) : boneOffset;
    if (!(flags & 2u)) prevBytes = boneBytes;
    Frame& f = m_frames[m_frameIndex];
    VkCommandBuffer cmd = UploadCommands();
    VkDescriptorBufferInfo infos[5] = {
        {mesh.buffer, 0, mesh.groupsOffset},
        {mesh.buffer, mesh.groupsOffset, VkDeviceSize(n) * 3 * 4},
        {f.ring, boneOffset, boneBytes},
        {f.ring, prevOffset, prevBytes},
        {f.skinArena, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[5] = {};
    for (uint32_t i = 0; i < 5; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinPipeline);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinLayout, 0, 5, writes);
    SkinPush push{n, job.bones->count, prev.count, flags | (job.rest ? 1u : 0u), uint32_t(out.vertexOffset / 4),
                  uint32_t(out.prevOffset / 4), uint32_t(out.smoothOffset / 4), mesh.membersBase};
    vkCmdPushConstants(cmd, m_skinLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (n + kGroupSize - 1) / kGroupSize, 1, 1);
    m_skinUploadsPending = true;
}

// The job's vertices (and last frame's positions) in this frame's arena: dispatched the first time the job is drawn
// this frame. Null: not possible (no arena space, no mesh buffer) - the draw goes the CPU way.
Device::SkinOutput* Device::SkinOnGpu(skin::Job& job)
{
    if (!m_gpuSkin || !m_skinPipeline || !job.source || !job.bones)
        return nullptr;
    auto found = m_skinOutputs.find(&job);
    if (found != m_skinOutputs.end())
        return &found->second;
    SkinMesh* mesh = SkinMeshFor(job.source);
    if (!mesh)
        return nullptr;
    static bool logged;
    if (!logged) {
        logged = true;
        Log("characters skinned on the GPU");
    }
    const VkDeviceSize n = job.source->vertices.size();
    SkinOutput out{};
    out.mesh = mesh;
    if (!SkinArenaAlloc(n * sizeof(skin::Vertex), &out.vertexOffset) || !SkinArenaAlloc(n * 12, &out.prevOffset))
        return nullptr;
    EnsureRingSpace(2 * (VkDeviceSize(job.bones->count) * 48 + 512));
    // Last frame's positions only on the job's first frame: drawn again later, the piece didn't move since.
    if (!job.gpuFrame)
        job.gpuFrame = m_frameNumber;
    bool moved = job.gpuFrame == m_frameNumber && job.prevBones && job.prevBones != job.bones;
    SkinDispatch(*mesh, job, moved ? 2u : 0u, out);
    out.moved = moved;
    return &(m_skinOutputs[&job] = out);
}

// The job's smooth normals (Phong tessellation) in the arena, dispatched once per frame when first needed.
bool Device::SkinSmoothOnGpu(const skin::Job& job, SkinOutput& out)
{
    if (out.smoothReady)
        return true;
    VkDeviceSize n = job.source->vertices.size();
    if (!SkinArenaAlloc(n * 12, &out.smoothOffset))
        return false;
    SkinDispatch(*out.mesh, job, 4u | 8u, out);
    out.smoothReady = true;
    return true;
}

// Before the upload command buffer ends: its skinning results become readable by the frame's draws.
void Device::FinishSkinUploads(VkCommandBuffer cmd)
{
    if (!m_skinUploadsPending)
        return;
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                       VK_ACCESS_2_INDEX_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    m_skinUploadsPending = false;
}

}  // namespace rvk
