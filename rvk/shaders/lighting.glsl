// Direct3D 7 lighting equation, shared by the per-vertex (ffp.vert) and per-pixel (ffp.frag) paths.
// Requires constants.glsl and a function float PointShadow(Light l, vec3 posW, vec3 normalW, float nl) (visibility of
// a frame light with a cube shadow map). Accumulates the light terms; the caller combines them with the material colours.

// PBR materials (ffp.frag, D.mat.z & MAT_PBR): set before AccumulateLights. The diffuse terms stay the game's
// (the caller weighs them by 1 - metallic); the specular term is GGX with the material's roughness and F0 instead of
// D3D's Blinn-Phong, lit by each light's diffuse colour (the game's specular colours are mostly black).
bool gPbr = false;
float gPbrAlpha = 0.25;          // GGX alpha (roughness squared, widened by the normal's variance)
vec3 gPbrF0 = vec3(0.04);        // reflectance facing the surface: 4% for non-metals, the albedo for metals
float gPbrSunVisibility = 1.0;   // how much of the sun reaches the pixel, fully (the sun's highlight, see below)

const float kPbrPi = 3.14159265;

// GGX / height-correlated Smith / Schlick, times nl, in the game's light units: its Lambert term is albedo x nl
// (no 1 / pi), so the specular BRDF is scaled by pi to match.
vec3 PbrSpecular(vec3 n, vec3 L, vec3 V)
{
    float nl = max(dot(n, L), 0.0);
    if (nl <= 0.0) return vec3(0.0);
    vec3 H = normalize(L + V);
    float nh = max(dot(n, H), 0.0), nv = max(dot(n, V), 1e-4), vh = max(dot(V, H), 0.0);
    float a2 = gPbrAlpha * gPbrAlpha;
    float d = nh * nh * (a2 - 1.0) + 1.0;
    float ndf = a2 / (kPbrPi * d * d);
    float vis = 0.5 / (nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2) + 1e-5);
    float f = pow(1.0 - vh, 5.0);
    vec3 F = gPbrF0 + (1.0 - gPbrF0) * f;
    return F * (ndf * vis * nl * kPbrPi);
}

// The split-sum environment term without its lookup table (Karis, "Physically Based Shading on Mobile").
vec3 EnvBrdfApprox(vec3 f0, float roughness, float nv)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022), c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}
void AccumulateLight(Light l, vec3 posW, vec3 normalW, vec3 toEye, float sunScale, float localScale, inout vec3 ambient,
                     inout vec3 diff, inout vec3 spec)
{
    uint type = uint(l.position.w);
    vec3 L;
    float att = 1.0;
    if (type == 3u) {
        L = -normalize(l.direction.xyz);
        att = sunScale;                          // shadowed sunlight (1 = unshadowed)
    } else {
        vec3 d = l.position.xyz - posW;
        float dist = length(d);
        if (dist > l.direction.w) return;
        L = d / max(dist, 1e-6);
        float denom = l.atten.x + l.atten.y * dist + l.atten.z * dist * dist;
        att = (denom > 0.0 ? 1.0 / denom : 1.0) * localScale;
        // D3D cuts lights off at their range. Per vertex the cut-off is smeared over the triangles; per
        // pixel it would draw a hard circle, so fade the light out towards its range instead.
        if ((C.flags.x & F_PERPIXEL) != 0u) {
            float r = dist / max(l.direction.w, 1e-6);
            float w = clamp(1.0 - r * r * r * r, 0.0, 1.0);
            att *= w * w;
        }
        if (type == 2u) {
            float rho = dot(-L, normalize(l.direction.xyz));
            if (rho <= l.spot.y) att = 0.0;
            else if (rho < l.spot.x)
                att *= pow(clamp((rho - l.spot.y) / max(l.spot.x - l.spot.y, 1e-6), 0.0, 1.0), l.atten.w);
        }
    }
    ambient += att * l.ambient.rgb;
    float nl = max(dot(normalW, L), 0.0);
    if (l.spot.z > 0.0 && nl > 0.0 && att > 0.0)
        att *= PointShadow(l, posW, normalW, nl);   // shadows take the light's diffuse and specular, not its ambient
    diff += att * nl * l.diffuse.rgb;
    if (gPbr) {
        // The sun's highlight is shadowed fully (sunScale keeps part of the sunlight in full shadow - baked-looking
        // shade - but a highlight has none to keep) and fades as the sun nears the horizon: a low sun's highlight at
        // grazing angles is the brightest there is, on surfaces its long shadows (beyond the cascades) should cover.
        float a = type == 3u ? gPbrSunVisibility * smoothstep(0.03, 0.25, L.y) : att;
        if (nl > 0.0 && a > 0.0)
            spec += a * l.diffuse.rgb * PbrSpecular(normalize(normalW), L, toEye);
    } else if ((C.flags.x & F_SPECULAR) != 0u && nl > 0.0) {
        float nh = max(dot(normalW, normalize(L + toEye)), 0.0);
        spec += att * pow(nh, C.misc.x) * l.specular.rgb;
    }
}

// The frame lights (light override) go to diffLocal / specLocal, so they can light beyond the game's clamp.
void AccumulateLights(vec3 posW, vec3 normalW, float sunScale, float localScale, inout vec3 ambient, inout vec3 diff,
                      inout vec3 spec, inout vec3 diffLocal, inout vec3 specLocal)
{
    vec3 toEye = (C.flags.x & F_LOCALVIEWER) != 0u || gPbr ? normalize(C.eyePos.xyz - posW) : -C.eyeDir.xyz;
    for (uint i = 0u; i < C.lightInfo.x; ++i)
        AccumulateLight(C.lights[i], posW, normalW, toEye, sunScale, localScale, ambient, diff, spec);
    // Only the frame lights whose range reaches the draw's bounding box (the CPU's mask): most draws have none or a
    // few of the 64.
    if ((C.flags.x & F_LIGHTOVERRIDE) != 0u)
        for (uint word = 0u; word < 2u; ++word) {
            uint mask = D.lightMask[word];
            while (mask != 0u) {
                uint i = word * 32u + uint(findLSB(mask));
                mask &= mask - 1u;
                if (i + 1u == C.lightInfo.z)     // a character's own light doesn't light the character
                    continue;
                // Characters aren't shadowed by the lights characters carry (ambient.w = 1): a crowd's lights sit at
                // head height a unit or less from the next body, the cube trails it by a frame, and that close the
                // body's own jaw, arms and wings threw sharp patches across it, jumping as everyone moved.
                Light l = FL.lights[i];
                if ((C.flags.x & F_CHARACTER) != 0u && l.ambient.w > 0.5) l.spot.z = 0.0;
                AccumulateLight(l, posW, normalW, toEye, sunScale, localScale, ambient, diffLocal, specLocal);
            }
        }
}
