// Dynamic state emulation (dynemu.h, docs/mac.md). The renderer makes its scene, pre-pass and shadow pipelines with the
// vertex input and the colour blend enable and equation dynamic, and sets them per draw (draw.cpp SetVertexInput,
// ApplyDynamicState). macOS's drivers (MoltenVK, KosmicKrisp) have neither VK_EXT_vertex_input_dynamic_state nor extended
// dynamic state 3's blend states; there such a pipeline is kept here as a template - its create info copied - behind a
// handle of our own, the vkCmdSet* calls of those states are recorded per command buffer, and a draw binds the template's
// variant for the recorded state: made on first use (through the renderer's pipeline cache, saved with it), kept until
// the template is destroyed. Everything else goes to the driver as it is.

#include "dynemu.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rvk {
void Log(const char* fmt, ...);
}

namespace rvk::vk {
namespace {

constexpr uint32_t kMaxAttachments = 8;
constexpr uint32_t kMaxVertexInputs = 32;        // bindings, and attributes (the renderer's: 2 and 6)
constexpr uint64_t kTemplateTag = 0xD5E0000000000000ull;

// The driver's functions as LoadDevice loaded them, before the wrappers below took their places.
struct Real {
    PFN_vkCreateGraphicsPipelines createGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
    PFN_vkCmdDraw cmdDraw = nullptr;
    PFN_vkCmdDrawIndexed cmdDrawIndexed = nullptr;
    PFN_vkCmdDrawIndirect cmdDrawIndirect = nullptr;
    PFN_vkCmdDrawIndexedIndirect cmdDrawIndexedIndirect = nullptr;
} g_real;

bool g_emulateVertexInput = false, g_emulateBlend = false;
std::atomic<uint32_t> g_templateCount{0}, g_variantCount{0}, g_failedCount{0};

template <typename H>
uint64_t Bits(H handle)
{
    uint64_t v = 0;
    std::memcpy(&v, &handle, sizeof(handle));
    return v;
}

template <typename H>
H FromBits(uint64_t v)
{
    H handle;
    std::memcpy(&handle, &v, sizeof(handle));
    return handle;
}

// A template's copies of its create info's pieces.
class Arena {
public:
    void* Alloc(size_t bytes)
    {
        const size_t n = (bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
        m_blocks.push_back(std::make_unique<std::max_align_t[]>(n ? n : 1));
        return m_blocks.back().get();
    }
    template <typename T>
    T* Copy(const T* src, size_t count = 1)
    {
        if (!src || !count) return nullptr;
        T* dst = static_cast<T*>(Alloc(sizeof(T) * count));
        std::memcpy(dst, src, sizeof(T) * count);
        return dst;
    }
    const void* CopyBytes(const void* src, size_t size)
    {
        if (!src || !size) return nullptr;
        void* dst = Alloc(size);
        std::memcpy(dst, src, size);
        return dst;
    }
    const char* CopyString(const char* s) { return s ? static_cast<const char*>(CopyBytes(s, std::strlen(s) + 1)) : nullptr; }

private:
    std::vector<std::unique_ptr<std::max_align_t[]>> m_blocks;
};

// The extension structures a pipeline's create info may chain (the renderer's: the rendering info, the shading rate
// state, the tessellation domain origin). Anything else and the pipeline isn't emulated.
bool CopyChain(Arena& arena, const void* src, const void** dst)
{
    const void* head = nullptr;
    VkBaseOutStructure* prev = nullptr;
    for (auto* in = static_cast<const VkBaseInStructure*>(src); in; in = in->pNext) {
        size_t size = 0;
        switch (in->sType) {
        case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO: size = sizeof(VkPipelineRenderingCreateInfo); break;
        case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR:
            size = sizeof(VkPipelineFragmentShadingRateStateCreateInfoKHR);
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
            size = sizeof(VkPipelineTessellationDomainOriginStateCreateInfo);
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
            size = sizeof(VkPipelineRasterizationDepthClipStateCreateInfoEXT);
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
            size = sizeof(VkPipelineRasterizationProvokingVertexStateCreateInfoEXT);
            break;
        default:
            Log("dynamic state emulation: create info structure %d not known", int(in->sType));
            return false;
        }
        auto* out = static_cast<VkBaseOutStructure*>(arena.Alloc(size));
        std::memcpy(out, in, size);
        out->pNext = nullptr;
        if (in->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            auto* r = reinterpret_cast<VkPipelineRenderingCreateInfo*>(out);
            r->pColorAttachmentFormats = arena.Copy(r->pColorAttachmentFormats, r->colorAttachmentCount);
        }
        if (prev) prev->pNext = out;
        else head = out;
        prev = out;
    }
    *dst = head;
    return true;
}

template <typename T>
T* CopyState(Arena& arena, const T* in, bool* ok)
{
    if (!in) return nullptr;
    T* c = arena.Copy(in);
    if (!CopyChain(arena, in->pNext, &c->pNext)) *ok = false;
    return c;
}

struct WordsHash {
    size_t operator()(const std::vector<uint32_t>& words) const
    {
        uint32_t h = 2166136261u;                // FNV-1a
        for (uint32_t w : words) h = (h ^ w) * 16777619u;
        return h;
    }
};

struct Template {
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkGraphicsPipelineCreateInfo ci{};           // its pointers: into the arena, ds
    Arena arena;
    std::vector<VkDynamicState> dynamic;         // the create info's, without the emulated
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    bool dynVertex = false, dynBlendEnable = false, dynBlendEquation = false;
    uint32_t attachments = 0;                    // colour attachments
    std::vector<VkShaderModule> modules;         // kept while the template is (DestroyShaderModule)
    std::mutex mutex;                            // the variants
    std::unordered_map<std::vector<uint32_t>, VkPipeline, WordsHash> variants;   // (failed: VK_NULL_HANDLE)
    bool loggedFailure = false;
};

// A command buffer's recorded state. Only the thread recording it touches it (Vulkan: externally synchronised).
struct CmdState {
    Template* bound = nullptr;                   // the template bound for graphics (null: a real pipeline, or none)
    bool needBind = false;                       // the template was bound since its variant was
    bool dirty = false;                          // the state changed since
    VkPipeline current = VK_NULL_HANDLE;         // the variant bound last
    std::vector<uint32_t> vertex{0u, 0u};        // binding count, attribute count, 4 words each
    std::vector<uint32_t> vertexScratch, key;
    VkBool32 enable[kMaxAttachments] = {};
    VkColorBlendEquationEXT equation[kMaxAttachments] = {};

    void Reset()
    {
        bound = nullptr;
        needBind = dirty = false;
        current = VK_NULL_HANDLE;
        vertex.assign({0u, 0u});
        std::memset(enable, 0, sizeof(enable));
        std::memset(equation, 0, sizeof(equation));
    }
};

struct ModuleRef {
    uint32_t refs = 0;
    bool destroyed = false;                      // the renderer destroyed it: really, with its last template
};

std::mutex g_mutex;                              // the maps (not the command buffers' states)
std::unordered_map<VkCommandBuffer, std::unique_ptr<CmdState>> g_cmds;
std::unordered_map<uint64_t, std::unique_ptr<Template>> g_templates;
std::unordered_map<uint64_t, ModuleRef> g_modules;
uint64_t g_nextHandle = 0;

CmdState* State(VkCommandBuffer cmd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::unique_ptr<CmdState>& s = g_cmds[cmd];
    if (!s) s = std::make_unique<CmdState>();
    return s.get();
}

Template* FindTemplate(VkPipeline pipeline)
{
    const uint64_t bits = Bits(pipeline);
    if ((bits & 0xFFFF000000000000ull) != kTemplateTag) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_templates.find(bits);
    return it == g_templates.end() ? nullptr : it->second.get();
}

bool CopyCreateInfo(Template& t, const VkGraphicsPipelineCreateInfo& in)
{
    Arena& a = t.arena;
    VkGraphicsPipelineCreateInfo ci = in;
    bool ok = CopyChain(a, in.pNext, &ci.pNext);
    auto* stages = a.Copy(in.pStages, in.stageCount);
    for (uint32_t s = 0; s < in.stageCount; ++s) {
        if (stages[s].pNext) ok = false;         // (inline shader code, required subgroup sizes: not used)
        stages[s].pName = a.CopyString(stages[s].pName);
        if (const VkSpecializationInfo* sp = stages[s].pSpecializationInfo) {
            VkSpecializationInfo* c = a.Copy(sp);
            c->pMapEntries = a.Copy(sp->pMapEntries, sp->mapEntryCount);
            c->pData = a.CopyBytes(sp->pData, sp->dataSize);
            stages[s].pSpecializationInfo = c;
        }
        if (stages[s].module) t.modules.push_back(stages[s].module);
    }
    ci.pStages = stages;
    if (auto* vi = CopyState(a, in.pVertexInputState, &ok)) {
        vi->pVertexBindingDescriptions = a.Copy(vi->pVertexBindingDescriptions, vi->vertexBindingDescriptionCount);
        vi->pVertexAttributeDescriptions = a.Copy(vi->pVertexAttributeDescriptions, vi->vertexAttributeDescriptionCount);
        ci.pVertexInputState = vi;
    }
    ci.pInputAssemblyState = CopyState(a, in.pInputAssemblyState, &ok);
    ci.pTessellationState = CopyState(a, in.pTessellationState, &ok);
    if (auto* vp = CopyState(a, in.pViewportState, &ok)) {
        vp->pViewports = a.Copy(vp->pViewports, vp->viewportCount);
        vp->pScissors = a.Copy(vp->pScissors, vp->scissorCount);
        ci.pViewportState = vp;
    }
    ci.pRasterizationState = CopyState(a, in.pRasterizationState, &ok);
    if (auto* ms = CopyState(a, in.pMultisampleState, &ok)) {
        ms->pSampleMask = a.Copy(ms->pSampleMask, (uint32_t(ms->rasterizationSamples) + 31) / 32);
        ci.pMultisampleState = ms;
    }
    ci.pDepthStencilState = CopyState(a, in.pDepthStencilState, &ok);
    if (auto* cb = CopyState(a, in.pColorBlendState, &ok)) {
        if (cb->attachmentCount > kMaxAttachments) ok = false;
        cb->pAttachments = a.Copy(cb->pAttachments, cb->attachmentCount);
        t.attachments = cb->pAttachments ? cb->attachmentCount : 0;
        ci.pColorBlendState = cb;
    }
    ci.pDynamicState = &t.ds;
    ci.flags &= ~VkPipelineCreateFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT);
    ci.basePipelineHandle = VK_NULL_HANDLE;
    ci.basePipelineIndex = -1;
    t.ci = ci;
    return ok;
}

VkResult CreateOne(VkDevice device, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo& info,
                   const VkAllocationCallbacks* alloc, VkPipeline* out)
{
    bool dynVertex = false, dynEnable = false, dynEquation = false;
    std::vector<VkDynamicState> kept;
    if (info.pDynamicState)
        for (uint32_t i = 0; i < info.pDynamicState->dynamicStateCount; ++i) {
            const VkDynamicState s = info.pDynamicState->pDynamicStates[i];
            if (s == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT && g_emulateVertexInput) dynVertex = true;
            else if (s == VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT && g_emulateBlend) dynEnable = true;
            else if (s == VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT && g_emulateBlend) dynEquation = true;
            else kept.push_back(s);
        }
    if (!dynVertex && !dynEnable && !dynEquation)
        return g_real.createGraphicsPipelines(device, cache, 1, &info, alloc, out);

    auto t = std::make_unique<Template>();
    t->device = device;
    t->cache = cache;
    t->dynamic = std::move(kept);
    t->ds.flags = info.pDynamicState->flags;
    t->ds.dynamicStateCount = uint32_t(t->dynamic.size());
    t->ds.pDynamicStates = t->dynamic.data();
    t->dynVertex = dynVertex;
    t->dynBlendEnable = dynEnable;
    t->dynBlendEquation = dynEquation;
    if (!CopyCreateInfo(*t, info)) {
        // Made as it is without the emulated states: its vertex input as the create info has it, its blending as its
        // attachments - wrong where they change per draw, but drawn.
        Log("dynamic state emulation: a pipeline it can't copy is made with fixed vertex input and blending");
        VkGraphicsPipelineCreateInfo ci = info;
        ci.pDynamicState = &t->ds;
        return g_real.createGraphicsPipelines(device, cache, 1, &ci, alloc, out);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (VkShaderModule m : t->modules) ++g_modules[Bits(m)].refs;
    const uint64_t handle = kTemplateTag | ++g_nextHandle;
    g_templates[handle] = std::move(t);
    *out = FromBits<VkPipeline>(handle);
    ++g_templateCount;
    return VK_SUCCESS;
}

VkPipeline CreateVariant(Template& t, const CmdState& st)
{
    VkGraphicsPipelineCreateInfo ci = t.ci;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkVertexInputBindingDescription bindings[kMaxVertexInputs];
    VkVertexInputAttributeDescription attributes[kMaxVertexInputs];
    if (t.dynVertex) {
        const uint32_t* w = st.vertex.data();
        const uint32_t bc = w[0], ac = w[1];
        w += 2;
        for (uint32_t i = 0; i < bc; ++i, w += 4) {
            bindings[i] = {w[0], w[1], VkVertexInputRate(w[2])};
            static bool logged = false;
            if (w[3] != 1 && !logged) {          // (VK_EXT_vertex_attribute_divisor's: the renderer's are all 1)
                Log("dynamic state emulation: vertex divisor %u taken as 1", w[3]);
                logged = true;
            }
        }
        for (uint32_t i = 0; i < ac; ++i, w += 4)
            attributes[i] = {w[0], w[1], VkFormat(w[2]), w[3]};
        vi.vertexBindingDescriptionCount = bc;
        vi.pVertexBindingDescriptions = bindings;
        vi.vertexAttributeDescriptionCount = ac;
        vi.pVertexAttributeDescriptions = attributes;
        ci.pVertexInputState = &vi;
    }
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineColorBlendAttachmentState att[kMaxAttachments];
    if (t.attachments && (t.dynBlendEnable || t.dynBlendEquation)) {
        cb = *ci.pColorBlendState;
        std::memcpy(att, cb.pAttachments, sizeof(att[0]) * t.attachments);
        for (uint32_t i = 0; i < t.attachments; ++i) {
            if (t.dynBlendEnable) att[i].blendEnable = st.enable[i];
            if (t.dynBlendEquation) {
                const VkColorBlendEquationEXT& q = st.equation[i];
                att[i].srcColorBlendFactor = q.srcColorBlendFactor;
                att[i].dstColorBlendFactor = q.dstColorBlendFactor;
                att[i].colorBlendOp = q.colorBlendOp;
                att[i].srcAlphaBlendFactor = q.srcAlphaBlendFactor;
                att[i].dstAlphaBlendFactor = q.dstAlphaBlendFactor;
                att[i].alphaBlendOp = q.alphaBlendOp;
            }
        }
        cb.pAttachments = att;
        ci.pColorBlendState = &cb;
    }
    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = g_real.createGraphicsPipelines(t.device, t.cache, 1, &ci, nullptr, &p);
    if (r != VK_SUCCESS) {
        ++g_failedCount;
        if (!t.loggedFailure) {
            Log("dynamic state emulation: the driver refused a variant (VkResult %d); its draws are skipped", int(r));
            t.loggedFailure = true;
        }
        return VK_NULL_HANDLE;
    }
    ++g_variantCount;
    return p;
}

VkPipeline Resolve(Template& t, CmdState& st)
{
    std::vector<uint32_t>& key = st.key;
    key.clear();
    if (t.dynVertex) key.insert(key.end(), st.vertex.begin(), st.vertex.end());
    if (t.attachments && (t.dynBlendEnable || t.dynBlendEquation)) {
        const VkPipelineColorBlendAttachmentState* att = t.ci.pColorBlendState->pAttachments;
        for (uint32_t i = 0; i < t.attachments; ++i) {
            const VkBool32 e = t.dynBlendEnable ? st.enable[i] : att[i].blendEnable;
            key.push_back(e);
            if (e && t.dynBlendEquation) {       // (the equation of an attachment not blending doesn't matter)
                const VkColorBlendEquationEXT& q = st.equation[i];
                key.insert(key.end(), {uint32_t(q.srcColorBlendFactor), uint32_t(q.dstColorBlendFactor),
                                       uint32_t(q.colorBlendOp), uint32_t(q.srcAlphaBlendFactor),
                                       uint32_t(q.dstAlphaBlendFactor), uint32_t(q.alphaBlendOp)});
            }
        }
    }
    std::lock_guard<std::mutex> lock(t.mutex);
    auto it = t.variants.find(key);
    if (it != t.variants.end()) return it->second;
    const VkPipeline p = CreateVariant(t, st);
    t.variants.emplace(key, p);
    return p;
}

// Before a draw: the variant for the recorded state bound, if a template is. False: no variant - the draw is skipped.
bool Prepare(VkCommandBuffer cmd)
{
    CmdState* st = State(cmd);
    if (!st->bound || !(st->needBind || st->dirty)) return true;
    const VkPipeline p = Resolve(*st->bound, *st);
    st->dirty = false;
    if (!p) {
        st->needBind = true;                     // (the next draws look again: a cached failure, or a new state)
        return false;
    }
    if (st->needBind || p != st->current) {
        g_real.cmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        st->current = p;
    }
    st->needBind = false;
    return true;
}

// ---- the wrappers ----

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                       const VkGraphicsPipelineCreateInfo* infos,
                                                       const VkAllocationCallbacks* alloc, VkPipeline* out)
{
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < count; ++i) {
        const VkResult r = CreateOne(device, cache, infos[i], alloc, &out[i]);
        if (r != VK_SUCCESS) {
            out[i] = VK_NULL_HANDLE;
            if (result == VK_SUCCESS) result = r;
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* alloc)
{
    std::unique_ptr<Template> t;
    std::vector<VkShaderModule> release;
    if (FindTemplate(pipeline)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_templates.find(Bits(pipeline));
        if (it != g_templates.end()) {
            t = std::move(it->second);
            g_templates.erase(it);
            for (VkShaderModule m : t->modules) {
                auto mi = g_modules.find(Bits(m));
                if (mi == g_modules.end() || --mi->second.refs) continue;
                if (mi->second.destroyed) release.push_back(m);
                g_modules.erase(mi);
            }
            for (auto& [cmd, st] : g_cmds)       // (a recording that still has it bound is past its use)
                if (st->bound == t.get()) {
                    st->bound = nullptr;
                    st->current = VK_NULL_HANDLE;
                }
        }
    }
    if (!t) {
        g_real.destroyPipeline(device, pipeline, alloc);
        return;
    }
    for (auto& [key, p] : t->variants)
        if (p) g_real.destroyPipeline(device, p, nullptr);
    for (VkShaderModule m : release) g_real.destroyShaderModule(device, m, nullptr);
    --g_templateCount;
}

VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(VkDevice device, VkShaderModule module, const VkAllocationCallbacks* alloc)
{
    if (module) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_modules.find(Bits(module));
        if (it != g_modules.end() && it->second.refs) {
            it->second.destroyed = true;         // (a template still makes variants with it)
            return;
        }
    }
    g_real.destroyShaderModule(device, module, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo* info)
{
    State(cmd)->Reset();
    return g_real.beginCommandBuffer(cmd, info);
}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline)
{
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        CmdState* st = State(cmd);
        if (Template* t = FindTemplate(pipeline)) {
            st->bound = t;
            st->needBind = true;
            return;
        }
        st->bound = nullptr;
        st->current = VK_NULL_HANDLE;
    }
    g_real.cmdBindPipeline(cmd, point, pipeline);
}

VKAPI_ATTR void VKAPI_CALL CmdSetVertexInput(VkCommandBuffer cmd, uint32_t bindingCount,
                                             const VkVertexInputBindingDescription2EXT* bindings, uint32_t attributeCount,
                                             const VkVertexInputAttributeDescription2EXT* attributes)
{
    CmdState* st = State(cmd);
    if (bindingCount > kMaxVertexInputs) bindingCount = kMaxVertexInputs;
    if (attributeCount > kMaxVertexInputs) attributeCount = kMaxVertexInputs;
    std::vector<uint32_t>& w = st->vertexScratch;
    w.clear();
    w.push_back(bindingCount);
    w.push_back(attributeCount);
    for (uint32_t i = 0; i < bindingCount; ++i)
        w.insert(w.end(), {bindings[i].binding, bindings[i].stride, uint32_t(bindings[i].inputRate), bindings[i].divisor});
    for (uint32_t i = 0; i < attributeCount; ++i)
        w.insert(w.end(), {attributes[i].location, attributes[i].binding, uint32_t(attributes[i].format),
                           attributes[i].offset});
    if (w != st->vertex) {
        st->vertex.swap(w);
        st->dirty = true;
    }
}

VKAPI_ATTR void VKAPI_CALL CmdSetColorBlendEnable(VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                                  const VkBool32* enables)
{
    CmdState* st = State(cmd);
    for (uint32_t i = 0; i < count && first + i < kMaxAttachments; ++i) {
        const VkBool32 e = enables[i] ? VK_TRUE : VK_FALSE;
        if (st->enable[first + i] != e) {
            st->enable[first + i] = e;
            st->dirty = true;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL CmdSetColorBlendEquation(VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                                    const VkColorBlendEquationEXT* equations)
{
    CmdState* st = State(cmd);
    for (uint32_t i = 0; i < count && first + i < kMaxAttachments; ++i)
        if (std::memcmp(&st->equation[first + i], &equations[i], sizeof(equations[i])) != 0) {
            st->equation[first + i] = equations[i];
            st->dirty = true;
        }
}

VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer cmd, uint32_t vertexCount, uint32_t instanceCount,
                                   uint32_t firstVertex, uint32_t firstInstance)
{
    if (Prepare(cmd)) g_real.cmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cmd, uint32_t indexCount, uint32_t instanceCount,
                                          uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    if (Prepare(cmd)) g_real.cmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount,
                                           uint32_t stride)
{
    if (Prepare(cmd)) g_real.cmdDrawIndirect(cmd, buffer, offset, drawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                                                  uint32_t drawCount, uint32_t stride)
{
    if (Prepare(cmd)) g_real.cmdDrawIndexedIndirect(cmd, buffer, offset, drawCount, stride);
}

}  // namespace

void InstallDynamicStateEmulation(VkDevice device, bool vertexInput, bool blend)
{
    (void)device;
    if (!vertexInput && !blend) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);   // (a new device: the old one's are gone)
        g_cmds.clear();
        g_templates.clear();
        g_modules.clear();
    }
    g_templateCount = g_variantCount = g_failedCount = 0;
    g_emulateVertexInput = vertexInput;
    g_emulateBlend = blend;
    // The driver's functions, unless the wrappers are in place already (LoadDevice puts the driver's back).
    if (vkCreateGraphicsPipelines != &CreateGraphicsPipelines) {
        g_real.createGraphicsPipelines = vkCreateGraphicsPipelines;
        g_real.destroyPipeline = vkDestroyPipeline;
        g_real.destroyShaderModule = vkDestroyShaderModule;
        g_real.beginCommandBuffer = vkBeginCommandBuffer;
        g_real.cmdBindPipeline = vkCmdBindPipeline;
        g_real.cmdDraw = vkCmdDraw;
        g_real.cmdDrawIndexed = vkCmdDrawIndexed;
        g_real.cmdDrawIndirect = vkCmdDrawIndirect;
        g_real.cmdDrawIndexedIndirect = vkCmdDrawIndexedIndirect;
    }
    vkCreateGraphicsPipelines = &CreateGraphicsPipelines;
    vkDestroyPipeline = &DestroyPipeline;
    vkDestroyShaderModule = &DestroyShaderModule;
    vkBeginCommandBuffer = &BeginCommandBuffer;
    vkCmdBindPipeline = &CmdBindPipeline;
    vkCmdDraw = &CmdDraw;
    vkCmdDrawIndexed = &CmdDrawIndexed;
    vkCmdDrawIndirect = &CmdDrawIndirect;
    vkCmdDrawIndexedIndirect = &CmdDrawIndexedIndirect;
    if (vertexInput) vkCmdSetVertexInputEXT = &CmdSetVertexInput;
    if (blend) {
        vkCmdSetColorBlendEnableEXT = &CmdSetColorBlendEnable;
        vkCmdSetColorBlendEquationEXT = &CmdSetColorBlendEquation;
    }
    Log("dynamic state emulation: %s%s%s (pipelines made per state on first use)", vertexInput ? "vertex input" : "",
        vertexInput && blend ? ", " : "", blend ? "colour blend enable and equation" : "");
}

DynamicEmulationStats GetDynamicEmulationStats()
{
    return {g_templateCount.load(), g_variantCount.load(), g_failedCount.load()};
}

}  // namespace rvk::vk
