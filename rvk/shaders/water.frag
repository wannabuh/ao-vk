#version 450
// The water's surface (rvk/water.cpp): drawn opaque into the scene, over a copy of the scene and its depth taken just
// before (what is under and behind the water). Per pixel:
//   - the surface normal: the waves' (water.vert) plus ripples from the detail texture, scrolled at two scales;
//   - what is seen through it: the scene copy, shifted by the ripples (refraction), lit by caustics on the bottom,
//     fading with the length of the view ray under the water into the water's own colour (absorption, scattering);
//   - what it reflects: the scene, marched along the mirrored ray through the copy's depth (screen-space), or the sky;
//     mixed in by Fresnel; the sun's glint on top;
//   - foam where the water is shallow (the shore) and on sharp crests;
//   - the game's fog over all of it.
// W.look.x blends two looks: 0 = the game's water made richer (its colour, its texture, more opaque), 1 = realistic.
#include "water_common.glsl"

layout(set = 0, binding = 5) uniform sampler2DArrayShadow shadowMap;   // the sun's cascades
layout(set = 0, binding = 9) uniform sampler2D sceneCopy;              // the HDR scene before the water
layout(set = 0, binding = 10) uniform sampler2D detail;                // RG: ripple slope, B: noise, A: cells
layout(set = 0, binding = 11) uniform sampler2D gameTexture;           // the game's water texture
layout(set = 0, binding = 12) uniform sampler2D envAtlas;              // the environment probe (env_common.glsl)
#include "env_common.glsl"

layout(location = 0) in vec3 vPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vClip;
layout(location = 3) in vec4 vPrevClip;
layout(location = 4) in vec3 vRest;
layout(location = 5) in float vFold;
layout(location = 6) in float vSpacing;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

const vec3 kLuma = vec3(0.3, 0.59, 0.11);

// Sun visibility (1 lit): the first cascade that covers the point, one filtered tap.
float SunShadow(vec3 posW)
{
    if (FL.shadowParams.x <= 0.5)
        return 1.0;
    int count = int(FL.shadowParams.z);
    for (int c = 0; c < count; ++c) {
        vec4 sc = FL.shadowViewProj[c] * vec4(posW, 1.0);
        vec3 ndc = sc.xyz / sc.w;
        if (max(abs(ndc.x), abs(ndc.y)) >= 1.0 || ndc.z <= 0.0 || ndc.z >= 1.0)
            continue;
        float lit = texture(shadowMap, vec4(ndc.xy * 0.5 + 0.5, float(c), ndc.z));
        return 1.0 - (1.0 - lit) * FL.shadowParams.y;
    }
    return 1.0;
}

// The ripples' slope (dh/dx, dh/dz) at a world point: two layers of the detail texture drifting across each other.
vec2 Ripples(vec2 xz, float t)
{
    vec2 wd = W.wind.xy, wp = vec2(-wd.y, wd.x);
    vec2 a = texture(detail, xz * 0.11 + wd * (t * 0.021)).rg * 2.0 - 1.0;
    vec2 b = texture(detail, xz * 0.037 - wp * (t * 0.011) + vec2(0.37, 0.71)).rg * 2.0 - 1.0;
    vec2 c = texture(detail, xz * 0.29 + (wd + wp) * (t * 0.035) + vec2(0.13, 0.53)).rg * 2.0 - 1.0;
    return a * 0.55 + b * 0.75 + c * 0.3;
}

// Caustics on the bottom at a world point: the bright web where two drifting layers of cells meet.
float Caustics(vec2 xz, float t)
{
    // (Each layer's lookup bent by the noise, so the web wobbles instead of sliding.)
    vec2 bend = (texture(detail, xz * 0.05 + vec2(t * 0.004)).bb - 0.5) * 0.3;
    float a = texture(detail, xz * 0.21 + bend + vec2(t * 0.013, t * 0.007)).a;
    float b = texture(detail, xz * 0.17 - bend + vec2(-t * 0.009, t * 0.012) + vec2(0.5, 0.25)).a;
    float web = min(a, b);
    return pow(clamp(1.0 - web, 0.0, 1.0), 5.0);
}

// The sky towards a direction (where no reflected scene is found): the frame's sky colour overhead going over to the
// fog's colour at the horizon (the game's sky meets its fog there).
vec3 Sky(vec3 dir)
{
    vec3 horizon = W.fogColour.rgb;
    vec3 top = W.sky.rgb;
    if (dot(top, vec3(1.0)) <= 0.0)
        top = horizon * vec3(0.75, 0.85, 1.1);
    float up = clamp(dir.y, 0.0, 1.0);
    return mix(horizon, top, sqrt(up));
}

// Screen-space reflection: the reflected ray marched through the scene copy's depth (view space). xyz: the colour
// found; w: confidence (0 = nothing found: the sky).
vec3 ToView(vec3 p) { return (W.view * vec4(p, 1.0)).xyz; }
vec2 ViewToPixel(vec3 q)
{
    vec2 ndc = vec2(q.x * W.proj.z, q.y * W.proj.w) / q.z;
    return vec2((ndc.x + 1.0) * 0.5 * W.viewport.x, (1.0 - ndc.y) * 0.5 * W.viewport.y);
}
vec4 Reflect(vec3 posW, vec3 dirW, float noise)
{
    int steps = int(W.time.w);
    if (steps <= 0)
        return vec4(0.0);
    vec3 origin = ToView(posW);
    vec3 dir = normalize(mat3(W.view) * dirW);
    float maxDist = 60.0 + origin.z * 0.6;
    float prevT = 0.0;
    for (int i = 1; i <= steps; ++i) {
        float u = (float(i) - 1.0 + noise) / float(steps);
        float t = maxDist * u * u + 0.1;
        vec3 q = origin + dir * t;
        if (q.z <= 0.05)
            break;
        vec2 hp = ViewToPixel(q);
        if (any(lessThan(hp, vec2(0.0))) || any(greaterThanEqual(hp, W.viewport.xy)))
            break;
        float sd = texelFetch(sceneDepth, ivec2(hp), 0).r;
        if (sd < 1.0) {
            float behind = q.z - ViewZ(sd);
            float thickness = max(0.3, (t - prevT) * 1.5 + 0.02 * q.z);
            if (behind > 0.0 && behind < thickness) {
                float a = prevT, b = t;
                for (int k = 0; k < 6; ++k) {
                    float m = 0.5 * (a + b);
                    vec3 qm = origin + dir * m;
                    float bm = qm.z - ViewZ(texelFetch(sceneDepth, ivec2(ViewToPixel(qm)), 0).r);
                    if (bm > 0.0) b = m; else a = m;
                }
                // A real hit: the ray meets the surface there. A ray that only passed behind something nearer (a
                // character between the camera and where the ray goes) would show that thing's pixels shifted along
                // the screen - an upright copy of it under the real one. Then it marches on.
                vec3 qb = origin + dir * b;
                float gap = qb.z - ViewZ(texelFetch(sceneDepth, ivec2(ViewToPixel(qb)), 0).r);
                if (abs(gap) > 0.08 + 0.006 * qb.z) {
                    prevT = t;
                    continue;
                }
                vec2 hit = ViewToPixel(qb);
                vec2 uv = hit * W.viewport.zw;
                float edge = smoothstep(0.0, 0.1, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)));
                float far = 1.0 - smoothstep(0.6, 1.0, b / maxDist);
                return vec4(texelFetch(sceneCopy, ivec2(hit), 0).rgb, edge * far);
            }
        }
        prevT = t;
    }
    return vec4(0.0);
}

float FogFactor(float d)
{
    uint mode = uint(W.fogParams.w);
    if (W.fogColour.w < 0.5 || mode == 0u) return 1.0;
    if (mode == 3u) return clamp((W.fogParams.y - d) / max(W.fogParams.y - W.fogParams.x, 1e-6), 0.0, 1.0);
    if (mode == 1u) return clamp(exp(-W.fogParams.z * d), 0.0, 1.0);
    float x = W.fogParams.z * d;
    return clamp(exp(-x * x), 0.0, 1.0);
}

void main()
{
    bool mesh = W.time.z > 0.5;
    if (int(W.sky.w) == 4) {                    // RANDYVK_WATER_DEBUG=4: the grid, unmasked
        float sd = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
        outScene = vec4(sd < gl_FragCoord.z ? 1.0 : 0.0, fract(vRest.x * 0.1), fract(vRest.z * 0.1), 1.0);
        return;
    }
    if (!mesh && W.mapInfo.z > 0.5 && texture(waterMap, MapUv(vRest.xz)).g < 0.5)
        discard;
    float style = W.look.x;
    float t = W.time.x;
    vec3 eye = W.camera.xyz;
    vec3 toEye = eye - vPos;
    float dist = length(toEye);
    vec3 V = toEye / max(dist, 1e-4);

    // The normal: the waves', plus ripples (fading out with distance, where they would only shimmer).
    vec3 N = mesh ? normalize(cross(dFdx(vPos), dFdy(vPos))) : normalize(vNormal);
    if (dot(N, V) < 0.0 && mesh) N = -N;
    float rippleFade = 1.0 - smoothstep(25.0, 220.0, dist);
    vec2 slope = mesh ? vec2(0.0) : Ripples(vPos.xz, t) * (W.look2.z * 0.22 * rippleFade);
    N = normalize(N + vec3(-slope.x, 0.0, -slope.y));
    bool under = !mesh && eye.y < vRest.y;
    vec3 Ns = under ? -N : N;
    float NdotV = max(dot(Ns, V), 1e-3);

    // The light on the water: the terrain's ambient plus the sun.
    vec3 L = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
    float shadow = SunShadow(vPos + vec3(0.0, 0.05, 0.0));
    vec3 sun = FL.sunColor.rgb;
    vec3 ambient = W.ambient.rgb;
    vec3 light = ambient + sun * (max(L.y, 0.0) * shadow);

    // Under the surface: how far the view ray goes through the water to what is behind it (the scene copy's depth).
    vec2 uv = gl_FragCoord.xy * W.viewport.zw;
    float zWater = vClip.w;
    float dScene = texture(sceneDepth, uv).r;
    float zScene = dScene >= 1.0 ? 1e5 : ViewZ(dScene);
    // Refraction: the ripples shift what is seen through the water, more the deeper it is; a shift onto something in
    // front of the water would show it through the water - then none.
    vec3 nView = mat3(W.view) * N;
    float pathGuess = dist * max(zScene / zWater - 1.0, 0.0);
    vec2 shift = nView.xy * vec2(1.0, -1.0) * (W.look.z * 0.06 * clamp(pathGuess, 0.0, 3.0) / (1.0 + zWater * 0.04));
    vec2 uvR = uv + shift;
    float dR = texture(sceneDepth, uvR).r;
    float zR = dR >= 1.0 ? 1e5 : ViewZ(dR);
    if (zR < zWater) { uvR = uv; zR = zScene; dR = dScene; }
    float path = dist * max(zR / zWater - 1.0, 0.0);             // world length of the ray under the water
    vec3 bottom = eye - V * (dist * zR / zWater);                 // what the ray reaches
    float depthBelow = dR >= 1.0 ? 1e4 : max(vRest.y - bottom.y, 0.0);
    vec3 behind = texture(sceneCopy, uvR).rgb;
    vec3 behindRaw = texture(sceneCopy, uv).rgb;

    // Caustics on the bottom: sunlit, fading with depth and gone where the bottom is in shadow.
    if (dR < 1.0 && W.look2.y > 0.0 && !under) {
        float c = Caustics(bottom.xz, t) * smoothstep(0.0, 0.4, depthBelow) * exp(-depthBelow * 0.35);
        float sunOn = clamp(dot(sun, kLuma) * 2.0, 0.0, 1.0) * SunShadow(bottom + vec3(0.0, 0.05, 0.0));
        behind *= 1.0 + c * sunOn * W.look2.y * 1.1;
    }

    // The water's own colour and how clear it is. Enhanced AO: the game's colour, as opaque as the game drew it
    // (its alpha) at about two units of water; realistic: clear water absorbing red first, a dark blue-green deep.
    float clarity = max(W.look.w, 0.05);
    float aoAlpha = clamp(W.tint.a, 0.05, 0.98);
    vec3 absorbAo = vec3(-log(1.0 - aoAlpha) * 0.5);
    vec3 absorbReal = vec3(0.42, 0.085, 0.055);
    vec3 absorb = mix(absorbAo, absorbReal, style) / clarity;
    vec3 tint = W.tint.rgb;
    vec3 deepAo = tint * 0.75 + 0.02;
    if (W.look2.w > 0.0) {                     // the game's texture, as the game tiles it (every 40 units), drifting
        vec3 tex = texture(gameTexture, vPos.xz / 40.0 + vec2(t * 0.01, t * 0.006) + slope * 0.4).rgb;
        deepAo *= mix(vec3(1.0), tex * 1.6, W.look2.w * (1.0 - style));
    }
    vec3 deepReal = vec3(0.015, 0.075, 0.09);
    vec3 deep = mix(deepAo, deepReal, style) * light;
    vec3 transmit = exp(-absorb * min(path, 400.0));
    vec3 water = behind * transmit + deep * (1.0 - transmit);

    // Reflection: Fresnel (water's 2% head-on) towards the mirrored scene or the sky; the enhanced AO look a little
    // less mirror-like.
    vec3 Rd = reflect(-V, Ns);
    if (!under) Rd.y = max(Rd.y, 0.02);
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + FL.taa.z);
    float F = 0.02 + 0.98 * pow(1.0 - NdotV, 5.0);
    F *= W.look.y * mix(0.75, 1.0, style);
    vec3 reflected;
    if (under) {
        // From below: beyond the critical angle the surface mirrors the water itself.
        float sinT = length(cross(V, Ns)) * 1.33;
        reflected = deep;
        F = sinT >= 1.0 ? 1.0 : F;
    } else {
        vec4 ssr = Reflect(vPos + Ns * 0.05, Rd, noise);
        vec3 sky = Sky(Rd);
        // What the camera has seen that way (the environment probe), sharp-ish: the ripples blur it anyway.
        if (W.env.x > 0.5) {
            vec4 p = mix(textureLod(envAtlas, EnvAtlasUv(Rd, 0), 0.0), textureLod(envAtlas, EnvAtlasUv(Rd, 1), 0.0), 0.4);
            sky = mix(sky, p.rgb, clamp(p.a, 0.0, 1.0) * W.env.y);
        }
        // The sky's reflection is shadowed where the sun is (a crude stand-in for the terrain around blocking it).
        reflected = mix(sky, ssr.rgb, ssr.w);
    }
    vec3 colour = mix(water, reflected, clamp(F, 0.0, 1.0));

    // The sun's glint: GGX with a roughness that grows with distance (a far ripple field is a blur of glints).
    if (!under && dot(sun, vec3(1.0)) > 0.0) {
        float rough = mix(0.06, 0.22, smoothstep(10.0, 300.0, dist)) + 0.1 * (1.0 - W.look2.z);
        vec3 H = normalize(L + V);
        float NdotH = max(dot(Ns, H), 0.0), NdotL = max(dot(Ns, L), 0.0);
        float a2 = rough * rough * rough * rough;
        float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
        float D = a2 / (kPi * dd * dd);
        float Fs = 0.02 + 0.98 * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        float vis = 0.25 / max(NdotL * NdotV, 0.05) * NdotL;
        colour += sun * (D * Fs * vis * shadow * W.look.y);
    }

    // Foam: on the shore (shallow water, broken up by the noise, moving with the waves) and on sharp crests.
    float foam = 0.0;
    if (W.look2.x > 0.0 && !mesh) {
        float shore = dR >= 1.0 ? 0.0 : 1.0 - smoothstep(0.0, 0.9, depthBelow);
        float crest = smoothstep(0.75, 0.35, vFold);
        float pattern = texture(detail, vPos.xz * 0.23 + vec2(t * 0.02, -t * 0.013)).b * 0.6 +
                        texture(detail, vPos.xz * 0.61 - vec2(t * 0.031, t * 0.017)).a * 0.4;
        float amount = clamp((shore * shore * 1.1 + crest * 0.8) * W.look2.x * mix(0.7, 1.0, style), 0.0, 1.0);
        foam = smoothstep(1.0 - amount, 1.0 - amount + 0.2, pattern) * amount;
        foam *= 1.0 - smoothstep(60.0, 260.0, dist);
        vec3 foamLight = ambient + sun * (max(dot(N, L), 0.0) * shadow);
        colour = mix(colour, vec3(0.85, 0.88, 0.9) * foamLight, foam);
    }

    // Where the surface meets the shore: fade over to what is behind, so the water has no hard line along the land.
    float edge = dR >= 1.0 ? 1.0 : smoothstep(0.0, 0.12, path);
    colour = mix(behindRaw, colour, edge);

    // The game's fog, by the view distance.
    float fog = FogFactor(dist);
    colour = mix(W.fogColour.rgb, colour, fog);

    outScene = vec4(colour, 1.0);
    // RANDYVK_WATER_DEBUG: 1 the depths (r: water view depth / 100, g: the scene behind / 100, b: path / 10), 2 the
    // normal, 3 the map (r: coverage, g: height + 0.5).
    int debugView = int(W.sky.w);
    if (debugView == 1) outScene = vec4(zWater / 100.0, min(zR, 1e3) / 100.0, path / 10.0, 1.0);
    else if (debugView == 2) outScene = vec4(N * 0.5 + 0.5, 1.0);
    else if (debugView == 3) outScene = vec4(texture(waterMap, MapUv(vRest.xz)).g, texture(waterMap, MapUv(vRest.xz)).r + 0.5, 0.0, 1.0);
    outGlow = vec4(0.0);
    // Local lights' share 0, reflectivity 0 (the water reflects by itself; the screen-space pass would add it twice),
    // the direct sunlight's share for the contact shadows.
    outLocal = vec4(0.0, 0.0, 0.2 * shadow, 1.0);
    vec2 now = vClip.xy / vClip.w, before = vPrevClip.xy / max(vPrevClip.w, 1e-6);
    outMotion = vec4(vPrevClip.w > 1e-6 ? (now - before) * 0.5 * W.viewport.xy * vec2(1.0, -1.0) : vec2(0.0), 0.0, 1.0);
    outAlbedo = vec4(clamp(mix(deep / max(dot(light, kLuma), 0.05), vec3(0.85), foam), 0.0, 1.0), 1.0);
}
