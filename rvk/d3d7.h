// Direct3D 7 constants and structures the renderer understands, with the original numeric values and
// memory layouts (so Randy's arguments can be passed straight through). Only what Anarchy Online uses,
// see docs/d3d7-vocabulary.md.
#pragma once

#include <cstdint>

namespace rvk::d3d {

// D3DPRIMITIVETYPE
enum Primitive : uint32_t { PointList = 1, LineList = 2, LineStrip = 3, TriangleList = 4, TriangleStrip = 5, TriangleFan = 6 };

// D3DTRANSFORMSTATETYPE
enum TransformState : uint32_t { World = 1, View = 2, Projection = 3, Texture0 = 16, Texture1 = 17 };

// D3DRENDERSTATETYPE
enum RenderState : uint32_t {
    RS_ZENABLE = 7, RS_FILLMODE = 8, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15,
    RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25,
    RS_DITHERENABLE = 26, RS_ALPHABLENDENABLE = 27, RS_FOGENABLE = 28, RS_SPECULARENABLE = 29,
    RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_FOGSTART = 36, RS_FOGEND = 37, RS_FOGDENSITY = 38,
    RS_COLORKEYENABLE = 41, RS_ZBIAS = 46, RS_RANGEFOGENABLE = 47, RS_TEXTUREFACTOR = 60,
    RS_WRAP0 = 128, RS_CLIPPING = 136, RS_LIGHTING = 137, RS_AMBIENT = 139, RS_FOGVERTEXMODE = 140,
    RS_COLORVERTEX = 141, RS_LOCALVIEWER = 142, RS_NORMALIZENORMALS = 143, RS_DIFFUSEMATERIALSOURCE = 145,
    RS_SPECULARMATERIALSOURCE = 146, RS_AMBIENTMATERIALSOURCE = 147, RS_EMISSIVEMATERIALSOURCE = 148,
    RS_COUNT = 160
};

// D3DTEXTURESTAGESTATETYPE
enum TextureStageState : uint32_t {
    TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4, TSS_ALPHAARG1 = 5, TSS_ALPHAARG2 = 6,
    TSS_TEXCOORDINDEX = 11, TSS_ADDRESS = 12, TSS_ADDRESSU = 13, TSS_ADDRESSV = 14, TSS_BORDERCOLOR = 15,
    TSS_MAGFILTER = 16, TSS_MINFILTER = 17, TSS_MIPFILTER = 18, TSS_MIPMAPLODBIAS = 19, TSS_MAXMIPLEVEL = 20,
    TSS_MAXANISOTROPY = 21, TSS_TEXTURETRANSFORMFLAGS = 24, TSS_COUNT = 25
};

// D3DTEXTUREOP
enum TextureOp : uint32_t {
    TOP_DISABLE = 1, TOP_SELECTARG1 = 2, TOP_SELECTARG2 = 3, TOP_MODULATE = 4, TOP_MODULATE2X = 5, TOP_MODULATE4X = 6,
    TOP_ADD = 7, TOP_ADDSIGNED = 8, TOP_ADDSIGNED2X = 9, TOP_SUBTRACT = 10, TOP_ADDSMOOTH = 11
};

// D3DTA_*
enum TextureArg : uint32_t { TA_DIFFUSE = 0, TA_CURRENT = 1, TA_TEXTURE = 2, TA_TFACTOR = 3, TA_SPECULAR = 4,
                             TA_COMPLEMENT = 0x10, TA_ALPHAREPLICATE = 0x20 };

// D3DBLEND, D3DCMPFUNC, D3DCULL, D3DTEXTUREADDRESS, filters, fog modes, material colour sources
enum Blend : uint32_t { BLEND_ZERO = 1, BLEND_ONE, BLEND_SRCCOLOR, BLEND_INVSRCCOLOR, BLEND_SRCALPHA, BLEND_INVSRCALPHA,
                        BLEND_DESTALPHA, BLEND_INVDESTALPHA, BLEND_DESTCOLOR, BLEND_INVDESTCOLOR, BLEND_SRCALPHASAT,
                        BLEND_BOTHSRCALPHA, BLEND_BOTHINVSRCALPHA };
enum Cmp : uint32_t { CMP_NEVER = 1, CMP_LESS, CMP_EQUAL, CMP_LESSEQUAL, CMP_GREATER, CMP_NOTEQUAL, CMP_GREATEREQUAL, CMP_ALWAYS };
enum Cull : uint32_t { CULL_NONE = 1, CULL_CW = 2, CULL_CCW = 3 };
enum Address : uint32_t { TADDRESS_WRAP = 1, TADDRESS_MIRROR = 2, TADDRESS_CLAMP = 3, TADDRESS_BORDER = 4 };
enum Filter : uint32_t { TFG_POINT = 1, TFG_LINEAR = 2, TFN_POINT = 1, TFN_LINEAR = 2, TFP_NONE = 1, TFP_POINT = 2, TFP_LINEAR = 3 };
enum FogMode : uint32_t { FOG_NONE = 0, FOG_EXP = 1, FOG_EXP2 = 2, FOG_LINEAR = 3 };
enum MaterialSource : uint32_t { MCS_MATERIAL = 0, MCS_COLOR1 = 1, MCS_COLOR2 = 2 };

// D3DTSS_TCI_* (high word of TEXCOORDINDEX) and D3DTTFF_*
enum : uint32_t { TCI_PASSTHRU = 0, TCI_CAMERASPACENORMAL = 0x10000, TCI_CAMERASPACEPOSITION = 0x20000,
                  TCI_CAMERASPACEREFLECTIONVECTOR = 0x30000 };
enum : uint32_t { TTFF_DISABLE = 0, TTFF_COUNT2 = 2, TTFF_COUNT3 = 3, TTFF_COUNT4 = 4, TTFF_PROJECTED = 256 };

// D3DFVF_*
enum : uint32_t { FVF_POSITION_MASK = 0x00E, FVF_XYZ = 0x002, FVF_XYZRHW = 0x004, FVF_NORMAL = 0x010, FVF_PSIZE = 0x020,
                  FVF_DIFFUSE = 0x040, FVF_SPECULAR = 0x080, FVF_TEXCOUNT_MASK = 0xF00, FVF_TEXCOUNT_SHIFT = 8 };

// D3DCLEAR_*
enum : uint32_t { CLEAR_TARGET = 1, CLEAR_ZBUFFER = 2, CLEAR_STENCIL = 4 };

// D3DLIGHTTYPE
enum LightType : uint32_t { LIGHT_POINT = 1, LIGHT_SPOT = 2, LIGHT_DIRECTIONAL = 3 };

struct Color { float r, g, b, a; };        // D3DCOLORVALUE
struct Vector { float x, y, z; };           // D3DVECTOR
struct Matrix { float m[4][4]; };           // D3DMATRIX, row-major, row vectors (v * M)

struct Material {                           // D3DMATERIAL7
    Color diffuse, ambient, specular, emissive;
    float power;
};

struct Light {                              // D3DLIGHT7
    LightType type;
    Color diffuse, specular, ambient;
    Vector position, direction;
    float range, falloff, attenuation0, attenuation1, attenuation2, theta, phi;
};

struct Viewport {                           // D3DVIEWPORT7
    uint32_t x, y, width, height;
    float minZ, maxZ;
};

static_assert(sizeof(Material) == 68 && sizeof(Light) == 104 && sizeof(Viewport) == 24, "D3D7 layouts");

}  // namespace rvk::d3d
