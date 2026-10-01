// rvk: a Vulkan renderer with a Direct3D 7 shaped front end (the part of D3D7 Anarchy Online uses).
//
// Usage per frame: BeginFrame(), state and draw calls, EndFrame(). Rendering goes into the main colour
// target (or one set with SetRenderTarget) plus a shared depth buffer; EndFrame() copies the main target
// to the window's swapchain (if a window was given) and can save it as a screenshot.
#pragma once

#include "d3d7.h"
#include "vk.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct VmaAllocator_T;
struct VmaAllocation_T;

namespace rvk {

class Device;

// Texture formats (the D3DX 7 / DirectDraw pixel formats the client can create).
enum class Format : uint32_t {
    A8R8G8B8, X8R8G8B8, R5G6B5, A1R5G5B5, X1R5G5B5, A4R4G4B4, L8, A8, A8L8,
    DXT1, DXT2, DXT3, DXT4, DXT5, Count
};

// Bytes for one row of 4x4 blocks (compressed) or of pixels, and rows of blocks/pixels, for a level.
uint32_t FormatRowBytes(Format format, uint32_t width);
uint32_t FormatRows(Format format, uint32_t height);
bool FormatIsCompressed(Format format);

class Texture {
public:
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    uint32_t Levels() const { return m_levels; }
    Format GetFormat() const { return m_format; }
    bool IsRenderTarget() const { return m_renderTarget; }

private:
    friend class Device;
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VmaAllocation_T* m_allocation = nullptr;
    uint32_t m_width = 0, m_height = 0, m_levels = 1;
    Format m_format = Format::A8R8G8B8;
    bool m_renderTarget = false;
    // Layout as of the end of the commands recorded so far (main command buffer for render targets;
    // plain textures only change layout in the upload command buffer, which runs first).
    VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// D3D7-style vertex buffer (IDirect3DVertexBuffer7). Contents live in CPU memory; each draw copies the
// vertices it uses, so rewriting the buffer between draws (as the game's CPU skinning does) is safe.
class VertexBuffer {
public:
    uint32_t Fvf() const { return m_fvf; }
    uint32_t Stride() const { return m_stride; }
    uint32_t Count() const { return m_count; }

private:
    friend class Device;
    uint32_t m_fvf = 0, m_stride = 0, m_count = 0;
    std::vector<uint8_t> m_data;
    bool m_locked = false;
};

struct DeviceInfo {
    std::string gpu;
    std::string driver;
    uint32_t apiVersion = 0;
};

class Device {
public:
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // window may be null (headless: no swapchain, screenshots still work).
    bool Init(HWND window, uint32_t width, uint32_t height, std::string* error);
    bool Resize(uint32_t width, uint32_t height);        // main target size; call outside a frame
    const DeviceInfo& Info() const { return m_info; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

    void BeginFrame();
    void EndFrame();
    void RequestScreenshot(const std::string& bmpPath);   // saved during the next EndFrame()

    // ---- D3D7-style interface (IDirect3DDevice7 semantics and enum values) ----
    void Clear(uint32_t flags, uint32_t argb, float z);
    void SetViewport(const d3d::Viewport& vp);
    void SetRenderState(uint32_t state, uint32_t value);
    void SetTextureStageState(uint32_t stage, uint32_t type, uint32_t value);
    void SetTransform(uint32_t type, const d3d::Matrix& m);
    void SetMaterial(const d3d::Material& m);
    void SetLight(uint32_t index, const d3d::Light& light);
    void LightEnable(uint32_t index, bool enable);
    void SetTexture(uint32_t stage, Texture* texture);
    // Null = the main target. Like D3D, resets the viewport to the whole target.
    void SetRenderTarget(Texture* target);
    Texture* GetRenderTarget() const { return m_target == m_main ? nullptr : m_target; }

    void DrawPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount);
    void DrawIndexedPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                              const uint16_t* indices, uint32_t indexCount);
    void DrawPrimitiveVB(uint32_t primitive, VertexBuffer* vb, uint32_t startVertex, uint32_t vertexCount);
    void DrawIndexedPrimitiveVB(uint32_t primitive, VertexBuffer* vb, uint32_t startVertex, uint32_t vertexCount,
                                const uint16_t* indices, uint32_t indexCount);

    // ---- resources ----
    Texture* CreateTexture(uint32_t width, uint32_t height, Format format, uint32_t levels);
    // A8R8G8B8 convenience: create one level and upload tightly packed pixels.
    Texture* CreateTexture(uint32_t width, uint32_t height, const void* argbPixels);
    // Uploads one mip level region. x, y, width, height must be multiples of 4 for DXT formats (or reach the
    // level's edge). pitch = bytes per row of pixels (or of 4x4 blocks). The new contents are visible to the
    // whole frame being recorded (uploads run before the frame's draws).
    void UpdateTexture(Texture* texture, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                       const void* data, uint32_t pitch);
    Texture* CreateRenderTarget(uint32_t width, uint32_t height);   // A8R8G8B8, sampleable
    void DestroyTexture(Texture* texture);

    VertexBuffer* CreateVertexBuffer(uint32_t fvf, uint32_t vertexCount);
    void* Lock(VertexBuffer* vb);
    void Unlock(VertexBuffer* vb);
    void DestroyVertexBuffer(VertexBuffer* vb);

    static uint32_t FvfStride(uint32_t fvf);

    static constexpr uint32_t kMaxLights = 8;
    static constexpr uint32_t kFramesInFlight = 2;

private:
    struct Frame {
        VkCommandBuffer upload = VK_NULL_HANDLE, main = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkBuffer ring = VK_NULL_HANDLE;
        VmaAllocation_T* ringAllocation = nullptr;
        uint8_t* ringData = nullptr;
        VkDeviceSize ringOffset = 0;
        std::vector<Texture*> pendingDestroy;
        bool uploadsRecorded = false;
    };

    struct LightSlot {
        d3d::Light light{};
        bool enabled = false;
    };

    bool CreateInstance(std::string* error);
    bool PickDevice(std::string* error);
    bool CreateLogicalDevice(std::string* error);
    bool CreateMainTargets(std::string* error);
    void DestroyMainTargets();
    bool CreateSwapchain(std::string* error);
    void DestroySwapchain();
    bool CreatePipelines(std::string* error);
    bool CreateFrames(std::string* error);
    Texture* CreateImage(uint32_t width, uint32_t height, Format format, uint32_t levels, bool renderTarget);

    VkDeviceSize Allocate(VkDeviceSize size, VkDeviceSize alignment, void** cpu);
    VkSampler SamplerFor(uint32_t stage);
    void Draw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
              const uint16_t* indices, uint32_t indexCount);
    void ApplyDynamicState(uint32_t primitive, uint32_t fvf);
    void BeginRenderingOn(Texture* target);
    void Transition(VkCommandBuffer cmd, Texture* t, VkImageLayout to);
    VkCommandBuffer UploadCommands();
    void SaveScreenshot();
    void DestroyTextureNow(Texture* texture);

    // Vulkan objects
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VmaAllocator_T* m_allocator = nullptr;
    VkPhysicalDeviceProperties m_props{};
    DeviceInfo m_info;
    std::array<bool, size_t(Format::Count)> m_formatSupported{};

    HWND m_window = nullptr;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_swapImages;
    std::vector<VkSemaphore> m_renderDone;     // one per swapchain image
    VkExtent2D m_swapExtent{};
    bool m_swapchainStale = false;

    uint32_t m_width = 0, m_height = 0;
    Texture* m_main = nullptr;                 // main colour target (a render target texture)
    Texture* m_target = nullptr;               // current colour target
    VkImage m_depth = VK_NULL_HANDLE;          // shared by every target; sized to the largest seen
    VkImageView m_depthView = VK_NULL_HANDLE;
    VmaAllocation_T* m_depthAllocation = nullptr;
    VkImageLayout m_depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    static constexpr VkFormat kColorFormat = VK_FORMAT_B8G8R8A8_UNORM;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    VkCommandPool m_pool = VK_NULL_HANDLE;
    std::array<Frame, kFramesInFlight> m_frames;
    uint32_t m_frameIndex = 0;
    bool m_inFrame = false;
    bool m_rendering = false;
    static constexpr VkDeviceSize kRingSize = 64ull << 20;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::array<VkPipeline, 3> m_pipelines{};   // per topology class: points, lines, triangles
    VkBuffer m_nullBuffer = VK_NULL_HANDLE;    // zeros, bound at stride 0 for attributes a format lacks
    VmaAllocation_T* m_nullAllocation = nullptr;
    Texture* m_blackTexture = nullptr;         // what an unbound stage samples
    std::unordered_map<uint64_t, VkSampler> m_samplers;

    std::string m_screenshotPath;
    VkBuffer m_readback = VK_NULL_HANDLE;
    VmaAllocation_T* m_readbackAllocation = nullptr;
    void* m_readbackData = nullptr;

    // D3D7 state
    std::array<uint32_t, d3d::RS_COUNT> m_rs{};
    std::array<std::array<uint32_t, d3d::TSS_COUNT>, 2> m_tss{};
    d3d::Matrix m_world{}, m_view{}, m_proj{};
    std::array<d3d::Matrix, 2> m_texMatrix{};
    d3d::Material m_material{};
    std::vector<LightSlot> m_lights;
    std::array<Texture*, 2> m_textures{};
    d3d::Viewport m_viewport{};
};

}  // namespace rvk
