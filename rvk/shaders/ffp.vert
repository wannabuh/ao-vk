#version 450
// Direct3D 7 fixed-function vertex processing.
#include "constants.glsl"

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inDiffuse;
layout(location = 3) in vec4 inSpecular;
layout(location = 4) in vec4 inTex0;
layout(location = 5) in vec4 inTex1;

layout(location = 0) out vec4 vDiffuse;
layout(location = 1) out vec4 vSpecular;
layout(location = 2) out vec4 vTex0;
layout(location = 3) out vec4 vTex1;
layout(location = 4) out float vFogDist;     // view-space distance (table fog)
layout(location = 5) out float vFogFactor;   // vertex fog factor

float FogFactor(uint mode, float d)
{
    if (mode == 3u) return clamp((C.fogParams.y - d) / max(C.fogParams.y - C.fogParams.x, 1e-6), 0.0, 1.0);
    if (mode == 1u) return clamp(exp(-C.fogParams.z * d), 0.0, 1.0);
    if (mode == 2u) { float x = C.fogParams.z * d; return clamp(exp(-x * x), 0.0, 1.0); }
    return 1.0;
}

vec4 MaterialColor(uint source, vec4 material, vec4 diffuse, vec4 specular, bool hasDiffuse, bool hasSpecular)
{
    if ((C.flags.x & F_COLORVERTEX) != 0u) {
        if (source == 1u && hasDiffuse) return diffuse;
        if (source == 2u && hasSpecular) return specular;
    }
    return material;
}

int TexCoordSize(uint fvf, uint set)
{
    uint code = (fvf >> (16u + 2u * set)) & 3u;
    return code == 0u ? 2 : code == 1u ? 3 : code == 2u ? 4 : 1;
}

vec4 TexCoord(uint stage, uint fvf, vec3 posV, vec3 normalV)
{
    uint tci = C.stageB[stage].z;
    uint ttf = C.stageB[stage].w;
    uint mode = tci & 0xFFFF0000u;
    uint set = tci & 0xFFFFu;
    vec4 c;
    int size;
    if (mode == 0x10000u) { c = vec4(normalV, 0.0); size = 3; }
    else if (mode == 0x20000u) { c = vec4(posV, 0.0); size = 3; }
    else if (mode == 0x30000u) { c = vec4(reflect(normalize(posV), normalV), 0.0); size = 3; }
    else { c = set == 0u ? inTex0 : inTex1; size = TexCoordSize(fvf, set); }
    if ((ttf & 0xFFu) == 0u)
        return c;
    // D3D pads the coordinate with a 1 after its last component before the texture matrix.
    vec4 p = size == 1 ? vec4(c.x, 1.0, 0.0, 0.0) : size == 2 ? vec4(c.xy, 1.0, 0.0)
           : size == 3 ? vec4(c.xyz, 1.0) : c;
    return C.texMatrix[stage] * p;
}

void main()
{
    uint fvf = C.vtx.x;
    bool rhw = (fvf & 0xEu) == 4u;
    bool hasNormal = (fvf & 0x10u) != 0u;
    bool hasDiffuse = (fvf & 0x40u) != 0u;
    bool hasSpecular = (fvf & 0x80u) != 0u;
    vec4 diffuse = hasDiffuse ? inDiffuse : vec4(1.0);
    vec4 specular = hasSpecular ? inSpecular : vec4(0.0);
    vec3 posV = vec3(0.0), normalV = vec3(0.0, 0.0, 1.0);

    if (rhw) {
        // Screen-space vertex. The Vulkan viewport is the D3D one shifted by half a pixel (D3D pixel
        // centres are at integer coordinates), so position relative to it.
        float w = inPos.w != 0.0 ? 1.0 / inPos.w : 1.0;
        vec2 ndc = vec2((inPos.x - C.viewport.x) / C.viewport.z * 2.0 - 1.0,
                        1.0 - (inPos.y - C.viewport.y) / C.viewport.w * 2.0);
        gl_Position = vec4(ndc * w, inPos.z * w, w);
        vDiffuse = diffuse;
        vSpecular = specular;
        vFogDist = inPos.z;
        vFogFactor = specular.a;          // pre-transformed vertices carry their fog factor in specular alpha
    } else {
        vec4 posW = D.world * vec4(inPos.xyz, 1.0);
        vec4 pv = C.view * posW;
        gl_Position = C.proj * pv;
        posV = pv.xyz;
        vec3 normalW = mat3(D.world) * (hasNormal ? inNormal : vec3(0.0));
        if ((C.flags.x & F_NORMALIZE) != 0u && dot(normalW, normalW) > 0.0)
            normalW = normalize(normalW);
        normalV = mat3(C.view) * normalW;

        if ((C.flags.x & F_LIGHTING) != 0u) {
            vec4 mDiffuse = MaterialColor(C.matSources.x, C.matDiffuse, diffuse, specular, hasDiffuse, hasSpecular);
            vec4 mAmbient = MaterialColor(C.matSources.y, C.matAmbient, diffuse, specular, hasDiffuse, hasSpecular);
            vec4 mSpecular = MaterialColor(C.matSources.z, C.matSpecular, diffuse, specular, hasDiffuse, hasSpecular);
            vec4 mEmissive = MaterialColor(C.matSources.w, C.matEmissive, diffuse, specular, hasDiffuse, hasSpecular);
            vec3 ambient = C.ambient.rgb, diff = vec3(0.0), spec = vec3(0.0);
            vec3 toEye = (C.flags.x & F_LOCALVIEWER) != 0u ? normalize(C.eyePos.xyz - posW.xyz) : -C.eyeDir.xyz;
            for (uint i = 0u; i < C.lightInfo.x; ++i) {
                Light l = C.lights[i];
                uint type = uint(l.position.w);
                vec3 L;
                float att = 1.0;
                if (type == 3u) {
                    L = -normalize(l.direction.xyz);
                } else {
                    vec3 d = l.position.xyz - posW.xyz;
                    float dist = length(d);
                    if (dist > l.direction.w) continue;
                    L = d / max(dist, 1e-6);
                    float denom = l.atten.x + l.atten.y * dist + l.atten.z * dist * dist;
                    att = denom > 0.0 ? 1.0 / denom : 1.0;
                    if (type == 2u) {
                        float rho = dot(-L, normalize(l.direction.xyz));
                        if (rho <= l.spot.y) att = 0.0;
                        else if (rho < l.spot.x)
                            att *= pow(clamp((rho - l.spot.y) / max(l.spot.x - l.spot.y, 1e-6), 0.0, 1.0), l.atten.w);
                    }
                }
                ambient += att * l.ambient.rgb;
                float nl = max(dot(normalW, L), 0.0);
                diff += att * nl * l.diffuse.rgb;
                if ((C.flags.x & F_SPECULAR) != 0u && nl > 0.0) {
                    float nh = max(dot(normalW, normalize(L + toEye)), 0.0);
                    spec += att * pow(nh, C.misc.x) * l.specular.rgb;
                }
            }
            diffuse = vec4(mEmissive.rgb + mAmbient.rgb * ambient + mDiffuse.rgb * diff, mDiffuse.a);
            specular = vec4(mSpecular.rgb * spec, specular.a);
        }
        vDiffuse = clamp(diffuse, 0.0, 1.0);
        vSpecular = clamp(specular, 0.0, 1.0);
        float dist = (C.flags.x & F_RANGEFOG) != 0u ? length(pv.xyz) : abs(pv.z);
        vFogDist = dist;
        vFogFactor = FogFactor(C.flags.y, dist);
    }
    vTex0 = TexCoord(0u, fvf, posV, normalV);
    vTex1 = TexCoord(1u, fvf, posV, normalV);
}
