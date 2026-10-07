// The interface layer (docs/rvk.md "Interface refresh rate"): the game's interface (GUI.dll's windows) drawn into an
// image of its own, premultiplied by its coverage, and blended over the main target every frame - so frames that
// don't redraw it (the game skips GUI.dll's drawing, rnative gui.cpp) still show it.
//
// In the layer every draw blends, its colour as the game asked and its alpha as coverage: what a sequence of blended
// draws over a transparent image leaves is exactly what they would have left over the frame, once the layer is put
// over the frame with (ONE, INV_SRC_ALPHA) - for the interface's alpha-blended and additive draws. (An unblended
// draw keeps its texture's alpha as coverage; one multiplying the frame - DESTCOLOR - is approximated.)
#include "internal.h"

namespace rvk {

using namespace vk;
using namespace detail;

void Device::InterfaceSceneEnd()
{
    // The interface's first draw ends the 3D scene (Draw): particles the game no longer draws go first, then (HDR) the
    // scene is tone mapped into the 8-bit target the interface goes over. A frame that doesn't redraw the interface
    // has no such draw.
    if (!m_external && m_target == m_main && m_particleSaw3D && !m_particleOrphansDone) {
        m_particleOrphanTrigger = "the interface";
        DrawOrphanParticles();
    }
    if (m_scenePhase && m_target == m_scene && m_sceneSaw3D) {
        m_sceneEndDraw = m_dumpDraw;
        m_sceneEndFvf = 0;
        EndScene();
    }
}

void Device::InterfaceBegin(bool redraw)
{
    if (!m_inFrame || !m_main)
        return;
    InterfaceSceneEnd();
    // Only over the 8-bit main target (the layer's format; HDR: after the scene's tone mapping). Otherwise - a frame
    // without 3D still in the float scene - the interface is drawn straight into the target, and is redrawn each
    // frame (not ready).
    if (m_main->m_format != Format::A8R8G8B8) {
        m_uiLayerReady.store(false, std::memory_order_relaxed);
        return;
    }
    m_uiLayerOpen = true;
    if (!redraw)
        return;
    if (!m_uiLayer || m_uiLayer->m_width != m_main->m_width || m_uiLayer->m_height != m_main->m_height) {
        if (m_uiLayer) DestroyTexture(m_uiLayer);
        m_uiLayer = CreateImage(m_main->m_width, m_main->m_height, Format::A8R8G8B8, 1, true);
        m_uiLayerReady.store(false, std::memory_order_relaxed);
        if (!m_uiLayer)
            return;
    }
    m_uiLayerReturn = m_target;
    m_uiLayerActive = true;
    SetRenderTarget(m_uiLayer);                  // (ends the rendering on the main target, begins one on the layer)
    m_cache = StateCache{};                      // the layer's blend equations differ (ApplyDynamicState)
    VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    clear.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkClearRect rect{{{0, 0}, {m_uiLayer->m_width, m_uiLayer->m_height}}, 0, 1};
    vkCmdClearAttachments(m_frames[m_frameIndex].main, 1, &clear, 1, &rect);
}

void Device::InterfaceEnd()
{
    if (!m_inFrame || !m_uiLayerOpen)
        return;
    m_uiLayerOpen = false;
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    if (m_uiLayerActive) {
        m_uiLayerActive = false;
        m_renderEndCause = kEndTarget;
        EndRendering();
        Texture* back = m_uiLayerReturn && m_uiLayerReturn != m_uiLayer ? m_uiLayerReturn : m_main;
        m_target = back;
        m_viewport = {0, 0, back->m_width, back->m_height, 0.0f, 1.0f};
        m_uiLayerReady.store(true, std::memory_order_relaxed);
    } else {
        m_renderEndCause = kEndTarget;
        EndRendering();
    }
    // The layer over the main target - this frame's interface, or the last one drawn.
    if (m_uiLayer && m_uiCompositePipeline && m_uiLayerReady.load(std::memory_order_relaxed) && !m_hideInterface &&
        m_main->m_format == Format::A8R8G8B8 && m_uiLayer->m_width == m_main->m_width &&
        m_uiLayer->m_height == m_main->m_height) {
        Transition(cmd, m_uiLayer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const float params[4] = {};
        FullscreenPass(cmd, m_main, m_uiCompositePipeline, m_uiLayer->m_view, m_uiLayer->m_view, m_pointSampler, params,
                       sizeof(params), true);
    }
    // The pass pushed set 0 with its own layout and changed the pipeline: the next draw binds again (as EndScene).
    m_cache = StateCache{};
    m_arenaBound = false;
    m_bindlessBound = false;
    m_constantsDirty = true;
    BeginRenderingOn(m_target);
}

}  // namespace rvk
