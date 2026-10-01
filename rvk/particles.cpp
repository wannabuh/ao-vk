// GPU particles (an enhancement): the game's sprite effects (GfxVisualDiaBill - sparkles, auras, nano effects) announce
// themselves with ParticleEmitter before drawing. Every live sprite sheds tiny particles that a compute pass moves
// through a flow field (shaders/particles.comp) and that outlive it.
//
// Timing: the effect's sprites arrive during frame N; the simulation runs at the start of frame N+1, before any
// rendering, and writes each particle's quad in the game's sprite vertex format; frame N+1 draws those quads right
// after the effect's own sprite draw (Draw), so they get its texture, blending and the HDR glow like the sprites.
#include "internal.h"

#include <algorithm>
#include <cstdarg>
#include <cmath>
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
    float motionA[4], motionB[4], noise[4];
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
static_assert(sizeof(GpuBlock) == 80 && sizeof(GpuSprite) == 64 && sizeof(GpuParticle) == 64, "std430 layouts");

constexpr uint32_t kQuadStride = 24;           // FVF 0x142
constexpr uint32_t kGroupSize = 64;
constexpr uint64_t kForgetFrames = 120;        // a block unseen this long is free again
constexpr double kMotionSmoothing = 0.3;       // seconds: how quickly an effect's measured motion follows changes

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
    const VkDeviceSize drawn = VkDeviceSize(kParticleDrawnBlocks) * kParticlesPerBlock;
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        if (!buffer(drawn * 4 * kQuadStride, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
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

void Device::ParticleEmitter(uint64_t key, const float center[3], const float origin[3], const ParticleSprite* sprites,
                             uint32_t count)
{
    m_particlePending = nullptr;
    if (!m_particleParams.enable || !key || m_particleBlocks.empty())
        return;
    ParticleBlock* block = nullptr;
    for (ParticleBlock& b : m_particleBlocks)
        if (b.key == key) { block = &b; break; }
    if (!block) {
        // A free block, or one whose effect is gone and whose particles have died out. All busy: no particles for this
        // effect (taking a busy block would make effects take turns, and both flicker).
        double now = Clock();
        for (ParticleBlock& b : m_particleBlocks)
            if (!b.key || (b.lastSeen + 1 < m_frameNumber && !ParticlesMayLive(b, now))) {
                block = &b;
                break;
            }
        if (!block) {
            if (!m_particleFullLogged)
                Log("particles: all %u effect blocks busy; further effects get none", kParticleBlocks);
            m_particleFullLogged = true;
            return;
        }
        block->key = key;
        block->reset = true;
        block->havePrev = false;
        block->lastSeen = 0;
        block->lastAliveTime = 0.0;
        block->drawnFrame = 0;
        block->haveState = false;
        block->motion = {};
        block->uploadTime = 0.0;
        uint64_t h = key * 0x9E3779B97F4A7C15ull;   // each effect swirls through its own part of the noise
        for (int i = 0; i < 3; ++i, h = (h ^ (h >> 29)) * 0xBF58476D1CE4E5B9ull)
            block->noise[i] = float(h >> 40) / float(1 << 24) * 97.0f;
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
        std::memcpy(block->prevOrigin, block->origin, sizeof(block->origin));
        std::memcpy(block->origin, origin, sizeof(block->origin));
        block->lastSeen = m_frameNumber;
        double now = Clock();
        if (std::any_of(sprites, sprites + count, [](const ParticleSprite& sp) { return sp.alive != 0; }))
            block->lastAliveTime = now;
        float dt = block->uploadTime > 0.0 ? float(now - block->uploadTime) : 0.0f;
        block->uploadTime = now;
        if (block->havePrev && dt > 1e-4f && dt < 0.25f)
            MeasureParticleMotion(*block, dt);
    }
    // The camera the effect is drawn with: the quads made at the start of next frame face it (a frame old - invisible on
    // particles this small).
    m_particleView = m_view;
    m_particlePending = block;
}

void Device::SimulateParticles(VkCommandBuffer cmd)
{
    if (!m_particleHeldTextures.empty())
        ReleaseParticleTextures(false);
    double now = Clock();
    float dt = m_particleLastClock > 0.0 ? float(std::clamp(now - m_particleLastClock, 0.0, 0.05)) : 0.0f;
    m_particleLastClock = now;
    m_particleTime += dt;

    // Effects announced last frame (this frame's number is one higher by now), and those whose particles may still
    // be alive - they fade out on their own once the effect is gone.
    std::vector<uint32_t> active;
    for (uint32_t i = 0; i < m_particleBlocks.size(); ++i) {
        ParticleBlock& b = m_particleBlocks[i];
        b.simulated = false;
        bool live = ParticlesMayLive(b, now);
        bool announced = b.key && b.lastSeen + 1 == m_frameNumber;
        if (b.wasAnnounced && !announced) {
            ParticleLog("effect %llx (block %u) ended: last announced frame %llu, a sprite alive %.2f s ago, draw state %s, "
                        "particles may live %s", (unsigned long long)b.key, i, (unsigned long long)b.lastSeen,
                        b.lastAliveTime > 0.0 ? now - b.lastAliveTime : -1.0, b.haveState ? "kept" : "MISSING",
                        live ? "yes" : "NO");
            b.fading = true;
            b.skipLogged = false;
            b.fadingDraws = 0;
            b.endTime = now;
        } else if (b.fading && announced) {
            ParticleLog("effect %llx (block %u) announced again after %.2f s", (unsigned long long)b.key, i, now - b.endTime);
            b.fading = false;
        }
        if (b.fading && !live) {
            ParticleLog("effect %llx (block %u) faded out: %.2f s after its end, %u fading draws", (unsigned long long)b.key,
                        i, now - b.endTime, b.fadingDraws);
            b.fading = false;
        }
        b.wasAnnounced = announced;
        if (b.key && b.lastSeen + kForgetFrames < m_frameNumber && !live) {
            b.key = 0;
            b.wasAnnounced = b.fading = false;
        }
        if (b.key && (b.lastSeen + 1 == m_frameNumber || live))
            active.push_back(i);
    }
    if (active.empty() || !m_particleParams.enable || !m_particlePipeline)
        return;
    // More than can be drawn: the nearest to the camera (the others pause).
    const auto& v = m_particleView.m;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
    if (active.size() > kParticleDrawnBlocks) {
        auto distance = [&](uint32_t i) {
            const float* c = m_particleBlocks[i].center;
            float dx = c[0] - eye[0], dy = c[1] - eye[1], dz = c[2] - eye[2];
            return dx * dx + dy * dy + dz * dz;
        };
        std::nth_element(active.begin(), active.begin() + kParticleDrawnBlocks, active.end(),
                         [&](uint32_t a, uint32_t b) { return distance(a) < distance(b); });
        active.resize(kParticleDrawnBlocks);
    }
    // Particles per sprite changed: start over (particles beyond the old count hold stale state).
    uint32_t perSprite = std::clamp(m_particleParams.perSprite, 1u, kParticleChildren);
    if (perSprite != m_particleSimCount) {
        for (ParticleBlock& b : m_particleBlocks) b.reset = true;
        m_particleSimCount = perSprite;
    }

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
        gb[k] = {{b.center[0], b.center[1], b.center[2], b.reset ? 1.0f : 0.0f}, {active[k], 0, 0, 0}, {}, {},
                 {b.noise[0], b.noise[1], b.noise[2], 0.0f}};
        ParticleBlockParams(b, gb[k].motionA, gb[k].motionB);
        bool announced = b.lastSeen + 1 == m_frameNumber;   // else its sprites are gone: no new particles
        for (uint32_t s = 0; s < kParticleSlots; ++s) {
            const ParticleSprite& cur = b.sprites[s];
            const ParticleSprite& prev = b.prevSprites[s];
            bool prevAlive = announced && b.havePrev && prev.alive;
            GpuSprite& g = gs[size_t(k) * kParticleSlots + s];
            std::memcpy(g.posSize, cur.pos, 12);
            g.posSize[3] = cur.size;
            std::memcpy(g.prevPos, prevAlive ? prev.pos : cur.pos, 12);
            g.prevPos[3] = BitsAsFloat((announced && cur.alive ? 1u : 0u) | (prevAlive ? 2u : 0u));
            std::memcpy(g.uv, cur.uv, 16);
            g.color[0] = cur.color;
            g.color[1] = g.color[2] = g.color[3] = 0;
        }
        b.reset = false;
        b.simulated = true;
        b.quadRegion = k;
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
    float push[20] = {
        dt, float(m_particleTime), float(perSprite), float(m_frameNumber & 0xFFFFFF),
        p.size, p.life, p.curl, p.swirl,
        0.0f, p.drag, std::clamp(p.follow, 0.0f, 1.0f), p.speed,
        v[0][0], v[1][0], v[2][0], 0.0f,          // camera right: the view matrix's first column
        v[0][1], v[1][1], v[2][1], 0.0f,          // camera up: its second
    };
    vkCmdPushConstants(cmd, m_particleLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(cmd, perSprite * kParticleSlots / kGroupSize, n, 1);

    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// How the effect's sprites move, relative to its origin (which moves with a character): orbiting, bursting out or
// falling in, rising, how big, how fast, how quickly sprites come and go. Smoothed, so a single frame can't flip it.
void Device::MeasureParticleMotion(ParticleBlock& b, float dt)
{
    float originVel[3];
    for (int i = 0; i < 3; ++i)
        originVel[i] = (b.origin[i] - b.prevOrigin[i]) / dt;
    double orbit = 0, burst = 0, rise = 0, speed2 = 0, spread2 = 0;
    uint32_t matched = 0, alive = 0, births = 0;
    for (uint32_t s = 0; s < kParticleSlots; ++s) {
        const ParticleSprite& cur = b.sprites[s];
        const ParticleSprite& prev = b.prevSprites[s];
        if (!cur.alive)
            continue;
        ++alive;
        float r[3] = {cur.pos[0] - b.center[0], cur.pos[1] - b.center[1], cur.pos[2] - b.center[2]};
        spread2 += r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        if (!prev.alive) {
            ++births;
            continue;
        }
        float v[3];
        for (int i = 0; i < 3; ++i)
            v[i] = (cur.pos[i] - prev.pos[i]) / dt - originVel[i];
        float v2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (v2 > 30.0f * 30.0f)
            continue;                            // a slot reused for a new sprite elsewhere, or a teleport
        ++matched;
        speed2 += v2;
        rise += v[1];
        float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (rl > 1e-3f)
            burst += (v[0] * r[0] + v[1] * r[1] + v[2] * r[2]) / rl;
        float rh = std::sqrt(r[0] * r[0] + r[2] * r[2]);
        if (rh > 1e-3f)                          // along cross(up, r): the shader's swirl direction
            orbit += (v[0] * r[2] - v[2] * r[0]) / rh;
    }
    if (!alive)
        return;
    ParticleBlock::Motion m = b.motion;
    float k = m.valid ? float(1.0 - std::exp(-dt / kMotionSmoothing)) : 1.0f;
    auto blend = [k](float& value, double sample) { value += (float(sample) - value) * k; };
    blend(m.spread, std::sqrt(spread2 / alive));
    blend(m.turnover, double(births) / (double(alive) * dt));
    if (matched >= 2) {
        blend(m.orbit, orbit / matched);
        blend(m.burst, burst / matched);
        blend(m.rise, rise / matched);
        blend(m.speed, std::sqrt(speed2 / matched));
    }
    m.valid = true;
    b.motion = m;
}

void Device::ParticleBlockParams(const ParticleBlock& b, float a[4], float bb[4]) const
{
    const ParticleParams& p = m_particleParams;
    const ParticleBlock::Motion& m = b.motion;
    float A = m.valid ? std::clamp(p.adapt, 0.0f, 1.0f) : 0.0f;
    // Swirl: the way the effect turns (if it clearly does), faster for fast orbits.
    float dir = (A > 0.0f && std::fabs(m.orbit) > 0.15f && m.orbit < 0.0f) ? -1.0f : 1.0f;
    a[0] = p.swirl * dir * (1.0f + A * std::min(std::fabs(m.orbit) / 0.75f, 2.0f));
    // Bursts throw their particles out (more of the sprite's speed, less pull back); implosions pull harder.
    float out = std::clamp(m.burst, 0.0f, 2.0f), in = std::clamp(-m.burst, 0.0f, 2.0f);
    a[1] = std::min(p.inherit * (1.0f + A * out), 1.5f);
    a[2] = p.pull * (1.0f - A * 0.6f * std::min(out, 1.0f)) * (1.0f + A * in);
    // Rising (or falling) effects carry their particles along.
    a[3] = A * std::clamp(m.rise, -3.0f, 3.0f) * 0.8f;
    // Big effects get big eddies, fast ones a faster flow, effects whose sprites live long longer-lived particles.
    bb[0] = p.scale * std::pow(std::clamp(m.spread, 0.35f, 3.0f), A);
    bb[1] = p.curl * std::pow(std::clamp(m.speed / 0.8f, 0.6f, 2.0f), A * 0.75f);
    float spriteLife = m.turnover > 0.05f ? 1.0f / m.turnover : 2.0f;
    bb[2] = std::pow(std::clamp(spriteLife / 0.6f, 0.6f, 1.8f), A * 0.5f);
    bb[3] = 0.0f;
}

void Device::ParticleLog(const char* fmt, ...)
{
    if (!m_particleLogBudget)
        return;
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    Log("particles: %s%s", line, --m_particleLogBudget ? "" : " (no more particle diagnostics this session)");
}

bool Device::ParticlesMayLive(const ParticleBlock& block, double now) const
{
    // The longest a particle lives: 1.5x the set life (particles.comp) times the largest life factor an effect's motion
    // gives (ParticleBlockParams: 1.8^0.5), plus the spawn delay.
    return block.key && block.lastAliveTime > 0.0 && now - block.lastAliveTime < m_particleParams.life * 1.5 * 1.35 + 0.25;
}

void Device::DrawOrphanParticles()
{
    if (m_particleOrphansDone)
        return;
    m_particleOrphansDone = true;
    if (!m_particleParams.enable)
        return;
    double now = Clock();
    for (ParticleBlock& b : m_particleBlocks) {
        if (!b.simulated || b.drawnFrame == m_frameNumber || !ParticlesMayLive(b, now))
            continue;
        if (!b.haveState || b.state.target != m_target) {
            if (!b.skipLogged)
                ParticleLog("effect %llx not drawn while fading: %s", (unsigned long long)b.key,
                            !b.haveState ? "no draw state (never drawn, or its texture/target went away)"
                                         : "drawn into another target than this frame's scene");
            b.skipLogged = true;
            continue;
        }
        ++b.fadingDraws;
        ParticleBlock::DrawState current{m_rs, m_tss, m_textures, m_view, m_proj, {m_texMatrix[0], m_texMatrix[1]},
                                         m_viewport, m_target};
        auto apply = [&](const ParticleBlock::DrawState& st) {
            m_rs = st.rs;
            m_tss = st.tss;
            m_textures = st.textures;
            m_view = st.view;
            m_proj = st.proj;
            m_texMatrix[0] = st.texMatrix[0];
            m_texMatrix[1] = st.texMatrix[1];
            m_viewport = st.viewport;
            m_constantsDirty = true;
        };
        apply(b.state);
        DrawParticles(b, true);
        apply(current);
    }
}

bool Device::HoldParticleTexture(Texture* texture)
{
    // A render target going away: its particles can't be drawn into it any more.
    for (ParticleBlock& b : m_particleBlocks)
        if (b.haveState && b.state.target == texture)
            b.haveState = false;
    // An effect's texture is often released together with the effect - exactly when its particles start fading out.
    // Kept until they have (ReleaseParticleTextures).
    double now = Clock();
    for (const ParticleBlock& b : m_particleBlocks)
        if (b.haveState && (b.state.textures[0] == texture || b.state.textures[1] == texture) && ParticlesMayLive(b, now)) {
            m_particleHeldTextures.push_back(texture);
            return true;
        }
    return false;
}

void Device::ReleaseParticleTextures(bool all)
{
    double now = Clock();
    auto used = [&](Texture* t) {
        for (const ParticleBlock& b : m_particleBlocks)
            if (b.haveState && (b.state.textures[0] == t || b.state.textures[1] == t) && ParticlesMayLive(b, now))
                return true;
        return false;
    };
    auto it = std::remove_if(m_particleHeldTextures.begin(), m_particleHeldTextures.end(), [&](Texture* t) {
        if (!all && used(t))
            return false;
        for (ParticleBlock& b : m_particleBlocks)          // nothing may draw with it from now on
            if (b.haveState && (b.state.textures[0] == t || b.state.textures[1] == t))
                b.haveState = false;
        m_deadTextures.push_back({DeathTag(), t});
        return true;
    });
    m_particleHeldTextures.erase(it, m_particleHeldTextures.end());
}

void Device::DrawParticles(ParticleBlock& block, bool orphan)
{
    if (!block.simulated || !m_particleParams.enable)
        return;
    if (!orphan) {
        block.state = {m_rs, m_tss, m_textures, m_view, m_proj, {m_texMatrix[0], m_texMatrix[1]}, m_viewport, m_target};
        block.haveState = true;
    }
    block.drawnFrame = m_frameNumber;
    uint32_t index = uint32_t(&block - m_particleBlocks.data());
    ExternalGeometry geometry{m_particleQuads[m_frameIndex], m_particleIndices,
                              int32_t(block.quadRegion * kParticlesPerBlock * 4)};
    d3d::Matrix world = m_world;                // the quads are in world space
    m_world = Identity();
    if (m_dumpFile) {
        const ParticleBlock::Motion& m = block.motion;
        float a[4], b[4];
        ParticleBlockParams(block, a, b);
        std::fprintf(m_dumpFile, "# particles of effect %llx (block %u)%s after draw %u: motion orbit %.2f burst %.2f rise %.2f"
                     " spread %.2f speed %.2f turnover %.2f/s -> swirl %.2f inherit %.2f pull %.2f lift %.2f scale %.2f"
                     " curl %.2f life x%.2f\n", (unsigned long long)block.key, index,
                     orphan ? " - effect gone, fading out" : "", m_dumpDraw, m.orbit, m.burst, m.rise, m.spread, m.speed,
                     m.turnover, a[0], a[1], a[2], a[3], b[0], b[1], b[2]);
    }
    m_external = &geometry;
    uint32_t quads = m_particleSimCount * kParticleSlots;   // only the particles in use
    Draw(d3d::TriangleList, kParticleFvf, nullptr, quads * 4, nullptr, quads * 6);
    m_external = nullptr;
    m_world = world;
    ++m_particleDraws;
}

}  // namespace rvk
