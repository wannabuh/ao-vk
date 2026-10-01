// GPU particles (an enhancement): the game's sprite effects (GfxVisualDiaBill - sparkles, auras, nano effects) announce
// themselves with ParticleEmitter before drawing. Every live sprite sheds tiny particles that a compute pass moves
// through a flow field (shaders/particles.comp) and that outlive it.
//
// Timing: the effect's sprites arrive during frame N; the simulation runs at the start of frame N+1, before any
// rendering, and writes each particle's quad in the game's sprite vertex format; frame N+1 draws those quads right
// after the effect's own sprite draw (Draw), so they get its texture, blending and the HDR glow like the sprites.
#include "internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kParticlesSpirv[] = {
#include "particles.comp.inc"
};

// GPU layouts (std430), must match particles.comp.
struct GpuBlock {
    float center[4];
    uint32_t info[4];
};
struct GpuSprite {
    float posSize[4];
    float prevPos[4];          // w: bits (1 = alive, 2 = alive last frame)
    float uv[4];
    uint32_t color[4];
};
struct GpuParticle {
    float posAge[4], velLife[4], uv[4];
    uint32_t meta[4];
};
static_assert(sizeof(GpuBlock) == 32 && sizeof(GpuSprite) == 64 && sizeof(GpuParticle) == 64, "std430 layouts");

constexpr uint32_t kQuadStride = 24;           // FVF 0x142
constexpr uint32_t kGroupSize = 64;
constexpr uint64_t kForgetFrames = 120;        // a block unseen this long is free again

double Clock()
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

float BitsAsFloat(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

}  // namespace

bool Device::CreateParticleResources(std::string* error)
{
    VkDescriptorSetLayoutBinding b[4];
    for (uint32_t i = 0; i < 4; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 4;
    sl.pBindings = b;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_particleSetLayout), "particle set layout", error))
        return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_particleSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_particleLayout), "particle pipeline layout", error))
        return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(kParticlesSpirv);
    mi.pCode = kParticlesSpirv;
    VkShaderModule module;
    if (!Check(vkCreateShaderModule(m_device, &mi, nullptr, &module), "particle shader module", error))
        return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    ci.layout = m_particleLayout;
    bool ok = Check(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_particlePipeline),
                    "particle pipeline", error);
    vkDestroyShaderModule(m_device, module, nullptr);
    if (!ok)
        return false;

    // Particle state and the quads: GPU only. The quad index pattern: host written once.
    auto buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, bool host, VkBuffer* out, VmaAllocation_T** alloc,
                      void** mapped) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO;
        if (host)
            ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info;
        if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, out, alloc, &info), "particle buffer", error))
            return false;
        if (mapped) *mapped = info.pMappedData;
        return true;
    };
    const VkDeviceSize particles = VkDeviceSize(kParticleBlocks) * kParticlesPerBlock;
    if (!buffer(particles * sizeof(GpuParticle), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                false, &m_particleState, &m_particleStateAllocation, nullptr))
        return false;
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        if (!buffer(particles * 4 * kQuadStride, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    false, &m_particleQuads[i], &m_particleQuadsAllocation[i], nullptr))
            return false;
    void* mapped = nullptr;
    if (!buffer(VkDeviceSize(kParticlesPerBlock) * 6 * 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, &m_particleIndices,
                &m_particleIndicesAllocation, &mapped))
        return false;
    auto* idx = static_cast<uint16_t*>(mapped);
    for (uint32_t q = 0; q < kParticlesPerBlock; ++q) {
        uint16_t v = uint16_t(q * 4);
        const uint16_t tri[6] = {v, uint16_t(v + 1), uint16_t(v + 2), uint16_t(v + 1), uint16_t(v + 2), uint16_t(v + 3)};
        std::memcpy(idx + q * 6, tri, sizeof(tri));
    }
    vmaFlushAllocation(m_allocator, m_particleIndicesAllocation, 0, VK_WHOLE_SIZE);
    m_particleBlocks.resize(kParticleBlocks);
    return true;
}

void Device::DestroyParticleResources()
{
    if (m_particlePipeline) vkDestroyPipeline(m_device, m_particlePipeline, nullptr);
    if (m_particleLayout) vkDestroyPipelineLayout(m_device, m_particleLayout, nullptr);
    if (m_particleSetLayout) vkDestroyDescriptorSetLayout(m_device, m_particleSetLayout, nullptr);
    if (m_particleState) vmaDestroyBuffer(m_allocator, m_particleState, m_particleStateAllocation);
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        if (m_particleQuads[i]) vmaDestroyBuffer(m_allocator, m_particleQuads[i], m_particleQuadsAllocation[i]);
    if (m_particleIndices) vmaDestroyBuffer(m_allocator, m_particleIndices, m_particleIndicesAllocation);
}

void Device::ParticleEmitter(uint64_t key, const float center[3], const ParticleSprite* sprites, uint32_t count)
{
    m_particlePending = nullptr;
    if (!m_particleParams.enable || !key || m_particleBlocks.empty())
        return;
    ParticleBlock* block = nullptr;
    for (ParticleBlock& b : m_particleBlocks)
        if (b.key == key) { block = &b; break; }
    if (!block) {
        // A free block, else the one unseen longest (whose particles then vanish).
        for (ParticleBlock& b : m_particleBlocks)
            if (!block || (block->key && (!b.key || b.lastSeen < block->lastSeen)))
                block = &b;
        block->key = key;
        block->reset = true;
        block->havePrev = false;
        block->lastSeen = 0;
        block->sprites.assign(kParticleSlots, ParticleSprite{});
        block->prevSprites.assign(kParticleSlots, ParticleSprite{});
    }
    if (block->lastSeen != m_frameNumber) {     // the effect may draw more than once a frame (reflections): keep the first
        block->havePrev = block->lastSeen + 1 == m_frameNumber;
        block->prevSprites.swap(block->sprites);
        count = std::min(count, kParticleSlots);
        std::copy(sprites, sprites + count, block->sprites.begin());
        std::fill(block->sprites.begin() + count, block->sprites.end(), ParticleSprite{});
        std::memcpy(block->center, center, sizeof(block->center));
        block->lastSeen = m_frameNumber;
    }
    // The camera the effect is drawn with: the quads made at the start of next frame face it (a frame old - invisible on
    // particles this small).
    m_particleView = m_view;
    m_particlePending = block;
}

void Device::SimulateParticles(VkCommandBuffer cmd)
{
    double now = Clock();
    float dt = m_particleLastClock > 0.0 ? float(std::clamp(now - m_particleLastClock, 0.0, 0.05)) : 0.0f;
    m_particleLastClock = now;
    m_particleTime += dt;

    std::vector<uint32_t> active;
    for (uint32_t i = 0; i < m_particleBlocks.size(); ++i) {
        ParticleBlock& b = m_particleBlocks[i];
        b.simulated = false;
        if (b.key && b.lastSeen + kForgetFrames < m_frameNumber)
            b.key = 0;
        // Effects drawn last frame (this frame's number is one higher by now).
        if (b.key && b.lastSeen + 1 == m_frameNumber)
            active.push_back(i);
    }
    if (active.empty() || !m_particleParams.enable || !m_particlePipeline)
        return;

    if (!m_particleStateCleared) {                // all particles start dead (life 0)
        vkCmdFillBuffer(cmd, m_particleState, 0, VK_WHOLE_SIZE, 0);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        m_particleStateCleared = true;
    }

    // This frame's inputs in the ring: block headers, then each block's sprites (with last frame's positions).
    const uint32_t n = uint32_t(active.size());
    const VkDeviceSize align = m_props.limits.minStorageBufferOffsetAlignment;
    const VkDeviceSize blockBytes = VkDeviceSize(n) * sizeof(GpuBlock);
    const VkDeviceSize spriteBytes = VkDeviceSize(n) * kParticleSlots * sizeof(GpuSprite);
    EnsureRingSpace(blockBytes + spriteBytes + 2 * align);
    void* cpu;
    VkDeviceSize blockOffset = Allocate(blockBytes, align, &cpu);
    auto* gb = static_cast<GpuBlock*>(cpu);
    VkDeviceSize spriteOffset = Allocate(spriteBytes, align, &cpu);
    auto* gs = static_cast<GpuSprite*>(cpu);
    for (uint32_t k = 0; k < n; ++k) {
        ParticleBlock& b = m_particleBlocks[active[k]];
        gb[k] = {{b.center[0], b.center[1], b.center[2], b.reset ? 1.0f : 0.0f}, {active[k], 0, 0, 0}};
        for (uint32_t s = 0; s < kParticleSlots; ++s) {
            const ParticleSprite& cur = b.sprites[s];
            const ParticleSprite& prev = b.prevSprites[s];
            bool prevAlive = b.havePrev && prev.alive;
            GpuSprite& g = gs[size_t(k) * kParticleSlots + s];
            std::memcpy(g.posSize, cur.pos, 12);
            g.posSize[3] = cur.size;
            std::memcpy(g.prevPos, prevAlive ? prev.pos : cur.pos, 12);
            g.prevPos[3] = BitsAsFloat((cur.alive ? 1u : 0u) | (prevAlive ? 2u : 0u));
            std::memcpy(g.uv, cur.uv, 16);
            g.color[0] = cur.color;
            g.color[1] = g.color[2] = g.color[3] = 0;
        }
        b.reset = false;
        b.simulated = true;
    }

    // Last frame's simulation (state) and the quad buffer's last use (two frames ago, fenced) come first.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);

    Frame& f = m_frames[m_frameIndex];
    VkDescriptorBufferInfo infos[4] = {
        {f.ring, blockOffset, blockBytes},
        {f.ring, spriteOffset, spriteBytes},
        {m_particleState, 0, VK_WHOLE_SIZE},
        {m_particleQuads[m_frameIndex], 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_particlePipeline);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_particleLayout, 0, 4, writes);
    const ParticleParams& p = m_particleParams;
    const auto& v = m_particleView.m;
    float push[20] = {
        dt, float(m_particleTime), float(std::min(p.perSprite, kParticleChildren)), float(m_frameNumber & 0xFFFFFF),
        p.size, p.life, p.curl, p.swirl,
        p.pull, p.drag, p.inherit, p.speed,
        v[0][0], v[1][0], v[2][0], p.scale,       // camera right: the view matrix's first column
        v[0][1], v[1][1], v[2][1], 0.0f,          // camera up: its second
    };
    vkCmdPushConstants(cmd, m_particleLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(cmd, kParticlesPerBlock / kGroupSize, n, 1);

    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void Device::DrawParticles(ParticleBlock& block)
{
    if (!block.simulated || !m_particleParams.enable)
        return;
    uint32_t index = uint32_t(&block - m_particleBlocks.data());
    ExternalGeometry geometry{m_particleQuads[m_frameIndex], m_particleIndices, int32_t(index * kParticlesPerBlock * 4)};
    d3d::Matrix world = m_world;                // the quads are in world space
    m_world = Identity();
    if (m_dumpFile)
        std::fprintf(m_dumpFile, "# particles of effect %llx (block %u) after draw %u\n", (unsigned long long)block.key, index,
                     m_dumpDraw);
    m_external = &geometry;
    Draw(d3d::TriangleList, kParticleFvf, nullptr, kParticlesPerBlock * 4, nullptr, kParticlesPerBlock * 6);
    m_external = nullptr;
    m_world = world;
    ++m_particleDraws;
}

}  // namespace rvk
