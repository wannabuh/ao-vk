/* ao-vk's interface plugin API (docs/ui-dll.md): the one header randy31.dll (the host) and an interface plugin share.
 *
 * ao-vk loads the plugin - a DLL in the folder ao-vk-plugins next to randy31.dll that exports AoVkPluginAttach -
 * on the game's first call into randy31, before GUI.dll makes its fonts, and calls AoVkPluginAttach with the host
 * table. The plugin checks the table's version (and whatever else it depends on), fills in its own table and
 * returns non-zero; zero, and ao-vk unloads it and runs without it.
 *
 * Plain C: no C++ types, exceptions or allocations cross over. Images are the host's (image / imageFree). Everything
 * is called on the game thread. AOVK_PLUGIN_VERSION is raised whenever a table or struct changes; new fields go at
 * the end only, so a plugin built against an older header keeps working (it sees `size`). */
#ifndef AOVK_PLUGIN_H
#define AOVK_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AOVK_PLUGIN_VERSION 1
#define AOVK_PLUGIN_FOLDER "ao-vk-plugins"         /* next to randy31.dll */
#define AOVK_PLUGIN_ATTACH "AoVkPluginAttach"

/* A shape of the interface back end: a rounded rectangle with a vertical gradient fill, a border inside its edge and
 * a glow outside, on screen (pixels), drawn only inside `clip`. Colours rgba 0-1 (straight). */
typedef struct AoVkUiShape {
    float rect[4];                          /* x0, y0, x1, y1 */
    float clip[4];
    float radius, border, glow, glowOnly;   /* glowOnly 1: just the glow (drawn last, LESS_OR_EQUAL) */
    float fillTop[4], fillBottom[4];
    float borderColor[4], glowColor[4];
    void* image;                            /* an image instead (image()): its part uv, tinted by fillTop */
    float uv[4];
    float additive;                         /* 1: its light added (an icon on black) */
} AoVkUiShape;

/* Meshes with the shapes (RmlUi's geometry): triangles, each mesh some of the batch's indices moved by `translate`,
 * inside `clip`, times its image (a white one for none) and opacity. Drawn after the shapes' fills, before their
 * depth - behind windows in front. */
typedef struct AoVkUiVertex {
    float x, y;
    uint8_t rgba[4];                        /* premultiplied */
    float u, v;
} AoVkUiVertex;
typedef struct AoVkUiMesh {
    uint32_t firstIndex, indexCount;
    float translate[2];
    float clip[4];
    void* image;
    float opacity;
} AoVkUiMesh;
typedef struct AoVkUiMeshes {
    const AoVkUiMesh* meshes;
    uint32_t count;
    const AoVkUiVertex* vertices;
    uint32_t vertexCount;
    const uint32_t* indices;
    uint32_t indexCount;
} AoVkUiMeshes;

/* Frosted glass behind windows: the scene blurred, dimmed and tinted inside each rectangle given by window(), with a
 * rim light and a soft glow. */
typedef struct AoVkGlass {
    int32_t enable;
    uint32_t blur;                          /* downsampling steps 1-4: the blur's width */
    float dim;                              /* darkens the scene behind (0-1) */
    float tint;                             /* towards tintColor (0-1) */
    float edge;                             /* rim light and glow (0-1) */
    float radius;                           /* corner radius (pixels) */
    float tintColor[3], rimColor[3];        /* rgb 0-1 */
} AoVkGlass;

/* View::_CallRender's arguments (GUI.dll): what the host's hook was called with. */
typedef struct AoVkViewCall {
    uint32_t order;
    void* viewport;                         /* RViewPort_t* */
    const void* clip;                       /* const Rect* (four floats) */
    uint32_t dirty;                         /* a bool */
    uint32_t alpha;                         /* a float's bits */
    uint32_t color;
} AoVkViewCall;

typedef struct AoVkHost {
    uint32_t version;                       /* AOVK_PLUGIN_VERSION */
    uint32_t size;                          /* sizeof(AoVkHost) */
    void (*log)(const char* fmt, ...);      /* a line in randy-vk.log */
    /* RVK_UiSkin ("Interface look from the interface plugin"): 0 - the game's own look wanted (the Original preset).
     * Asked once a frame; the plugin draws nothing of its own while it is 0. */
    int32_t (*lookAllowed)(void);
    /* The glass's look (enable 0: none). Kept until changed; the host adds its own switch (lookAllowed). */
    void (*setGlass)(const AoVkGlass* glass);
    /* While the interface is drawn (between interfaceBegin and interfaceEnd): glass behind this rectangle. */
    void (*window)(float x0, float y0, float x1, float y1);
    /* While the interface is drawn: shapes and / or meshes, where GUI.dll's drawing has got to. */
    void (*shapes)(const AoVkUiShape* shapes, uint32_t count, const AoVkUiMeshes* meshes);
    /* An image from BGRA pixels (A8R8G8B8, tightly packed): a handle for AoVkUiShape::image / AoVkUiMesh::image, or
     * null (no renderer yet); and its end. */
    void* (*image)(const void* bgra, uint32_t width, uint32_t height);
    void (*imageFree)(void* image);
} AoVkHost;

/* The plugin's table, filled by AoVkPluginAttach. Any entry may be null. */
typedef struct AoVkPlugin {
    uint32_t version;                       /* the AOVK_PLUGIN_VERSION it was built against */
    uint32_t size;                          /* sizeof(AoVkPlugin) */
    /* Once a presented frame, before the host's own per-frame work (the plugin's own: hooking GUI.dll once it is
     * loaded, its settings, its hotkeys). The host hooks only WindowController_c::Render and View::_CallRender; the
     * plugin may hook any other GUI.dll entry itself. */
    void (*frame)(void);
    /* Around GUI.dll's WindowController_c::Render (the host's hook): `drawn` 0 - this frame's interface isn't drawn
     * (RVK_UiRate: the renderer shows the last one) - no view calls come between them then. */
    void (*interfaceBegin)(void* controller, int32_t drawn);
    void (*interfaceEnd)(void* controller, int32_t drawn);
    /* Around each View::_CallRender (the host's hook): `depth` 0 a window's top-level view, 1 its children, ... .
     * viewBegin non-zero: the plugin drew the view - GUI.dll's _CallRender (and viewEnd) not called for it. */
    int32_t (*viewBegin)(void* view, int32_t depth, const AoVkViewCall* call);
    void (*viewEnd)(void* view, int32_t depth, const AoVkViewCall* call);
} AoVkPlugin;

/* The plugin's export AoVkPluginAttach (__cdecl): non-zero when it attached. GUI.dll may or may not be loaded yet. */
typedef int32_t (*AoVkPluginAttachFn)(const AoVkHost* host, AoVkPlugin* plugin);

#ifdef __cplusplus
}
#endif

#endif
