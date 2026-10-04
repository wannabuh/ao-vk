// Which of the game's visuals is drawing: RViewPort_t::Render walks the render lists and keeps the visual it is
// rendering at RViewPort_t +0x164 (docs/frame.md); hooking Render (to know the viewport) is enough to name the visual
// behind every draw, from its RTTI class. randy-vk.ini [Native] Visuals=on.
#pragma once

#include "native/native.h"

#include <cstdint>

namespace rnative::scene {

// What a visual is, for the renderer (rvk::VisualKind has the same values).
enum class Kind : uint32_t {
    Unknown = 0,      // no visual (interface, DisplaySystem's own passes) or not classified
    Character,        // RCATMesh_t: a character's body
    CharacterPart,    // anything attached to a character (head, hair, weapon, armour pieces)
    Static,           // RTriMesh_t and its DisplaySystem subclasses (GenericMeshObject_t: buildings, props)
    Terrain,          // AnarchyGround_t
    Room,             // VisualRoom_t
    Water,            // VisualLiquid_t
    Sky,              // the sky and its layers
    BlobShadow,       // GfxVisualSimpleShadow_c
    Effect,           // other GfxVisual* (particles, beams, trails)
    Other,
};

struct VisualInfo {
    Kind kind = Kind::Unknown;
    const char* className = "";   // RTTI ".?AVName@@" without the decoration
    const void* owner = nullptr;  // Character / CharacterPart: the character (its RCATMesh_t frame)
};

// Randy's lights of the frame (after RViewPort_t::Process placed them in the world), each with the character that
// carries it (its RCATMesh_t frame up its parents), if any.
struct SceneLight {
    uint8_t d3dLight[104];        // D3DLIGHT7, world space
    const void* owner;
};
using LightSinkFn = void (*)(const SceneLight* lights, size_t count);
void SetLightSink(LightSinkFn sink);

// The visual being rendered right now (null outside RViewPort_t::Render), and what it is.
const void* CurrentVisual();
VisualInfo Describe(const void* visual);

void Install(HMODULE orig);
bool Installed();

}  // namespace rnative::scene
