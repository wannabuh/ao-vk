#version 450
// HDR scene -> 8-bit main target (hdr.cpp). Colours up to the knee pass unchanged, so the game's own look is kept;
// brighter ones roll off smoothly towards 1 by their brightest channel, which keeps their hue, and lose saturation
// the more they are overexposed, as bright light does.
#include "occlusion.glsl"
#define FL_BINDING 15
#include "frame_lights.glsl"                    // (the lightning's flash: FL.flash)
#include "dither.glsl"                         // (8-bit output: no bands in smooth skies)
#include "sky_common.glsl"                     // (the sky's twilight: the haze, far fog and mist in its colours)
layout(set = 0, binding = 16) uniform sampler2D rainNear;   // the rain (rvk rain.cpp m_rainNear: premultiplied; FL.rainNear.x)
layout(location = 0) out vec4 outColor;

// Colour grading of the tone mapped colour (0..1): white balance, a cooler, paler night, contrast (in a perceptual
// square-root space, around its middle), saturation, the day / night lookup tables, vignette. far: 0 near .. 1 at
// the far clip and the sky - the warmth is for what's near; the distance and the sky lean cool, as the air makes
// them (a warmed teal sky turned green-grey: user).
vec3 Grade(vec3 c, float far)
{
    const vec3 kLuma = vec3(0.3, 0.59, 0.11);
    float warmth = P.grade.z * mix(1.0, -0.5, smoothstep(0.3, 1.0, far));
    c *= vec3(1.0 + 0.12 * warmth, 1.0 + 0.02 * warmth, 1.0 - 0.12 * warmth);
    // By night (RVK_NightTint) colour fades and leans blue as eyes see in the dark - most in the darks, little in what
    // is bright (lamps, windows keep their colour).
    float nightTint = P.grade2.x * P.grade2.y;
    float darks = 1.0 - smoothstep(0.08, 0.45, dot(c, kLuma));
    c = mix(c, dot(c, kLuma) * vec3(0.78, 0.9, 1.18), min(nightTint * mix(0.3, 1.2, darks), 1.0));
    // The golden hour (RVK_GoldenHour; rvk sky.cpp FL.dawn[1].w): round sunrise and sunset the light warmer and the
    // shade a little cooler - split by brightness -, a touch more contrast and colour.
    float golden = FL.dawn[1].w;
    if (golden > 0.0) {
        vec3 split = mix(vec3(0.95, 0.98, 1.06), vec3(1.06, 1.0, 0.88), smoothstep(0.1, 0.6, dot(c, kLuma)));
        c *= mix(vec3(1.0), split, golden);
    }
    // The blue hour (RVK_BlueHour; rvk sky.cpp FL.dusk.y): after sunset the picture cooler, its darks greyer and bluer,
    // leading into the night's tint.
    float blueHour = FL.dusk.y;
    if (blueHour > 0.0) {
        float l = dot(c, kLuma);
        c = mix(c, c * vec3(0.9, 0.96, 1.12), 0.6 * blueHour);
        c = mix(c, l * vec3(0.88, 0.96, 1.14), 0.3 * blueHour * (1.0 - smoothstep(0.05, 0.5, l)));
    }
    vec3 s = sqrt(max(c, vec3(0.0)));
    s = (s - 0.5) * (P.grade.y + 0.06 * golden) + 0.5;
    c = max(s, vec3(0.0)) * max(s, vec3(0.0));
    c = mix(vec3(dot(c, kLuma)), c, P.grade.x + 0.05 * golden);
    c = clamp(c, 0.0, 1.0);
    if (P.grade.w > 0.0) {
        vec3 size = vec3(textureSize(lutDay, 0));
        vec3 day = texture(lutDay, c * (size - 1.0) / size + 0.5 / size).rgb;
        size = vec3(textureSize(lutNight, 0));
        vec3 night = texture(lutNight, c * (size - 1.0) / size + 0.5 / size).rgb;
        c = mix(c, mix(day, night, P.grade2.x), P.grade.w);
    }
    if (P.grade2.z > 0.0) {
        vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0)) - 0.5;
        c *= 1.0 - P.grade2.z * smoothstep(0.35, 0.9, length(uv * vec2(1.0, 0.75)) * 1.4);
    }
    return c;
}

// The air between the eye and a surface z (view depth) away (RVK_Atmos, docs/scattering-plan.md): thicker low down -
// valleys below the eye hazier, peaks above clearer (exponential with height, integrated along the ray) - its
// extinction taking red first (the far land bluer), the light it scatters the game's fog colour (the zone's air) and,
// towards the sun, its glow (a forward lobe: hazy air round a low sun lights up), the sun's light reddened as it sets.
// The storm's distant gusts (occlusion.glsl gust0..3): value noise (0..1) at a point.
float GustHash(vec3 p)
{
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float GustNoise(vec3 x)
{
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(GustHash(i), GustHash(i + vec3(1, 0, 0)), f.x), mix(GustHash(i + vec3(0, 1, 0)), GustHash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(GustHash(i + vec3(0, 0, 1)), GustHash(i + vec3(1, 0, 1)), f.x), mix(GustHash(i + vec3(0, 1, 1)), GustHash(i + 1.0), f.x), f.y),
               f.z);
}
// How much sand the haze holds along the view to view depth z, in units of z (z itself: an even haze): beyond the
// simulated sand round the camera (gust2.y), billows of dust drifting with the wind - thicker and thinner, a bigger
// scale and a finer one, stretched along it - thicker low down, thinner high up; six samples to 700 units out, an even
// haze beyond.
float DustDepth(vec2 uv, float z)
{
    vec3 f = P.gust1.xyz;
    vec3 r = normalize(vec3(f.z, 0.0, -f.x) + vec3(1e-6, 0.0, 0.0));   // (the camera never rolls: right level)
    vec3 u = cross(f, r);
    vec3 D = r * ((uv.x * 2.0 - 1.0) / P.gust1.w) + u * ((1.0 - uv.y * 2.0) / P.gust2.x) + f;   // (a view depth of 1)
    float L = length(D);
    float t0 = P.gust2.y / L, t1 = min(z, 700.0 / L);
    if (t1 <= t0)
        return z;
    const int kSamples = 6;
    float dt = (t1 - t0) / float(kSamples);
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec2 wd = P.gust3.xy, side = vec2(-wd.y, wd.x);
    float sum = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        float t = t0 + (float(i) + jitter) * dt;
        vec3 p = P.gust0.xyz + D * t;
        float h = p.y - P.gust0.w;
        float along = dot(p.xz, wd) + P.gust3.z, across = dot(p.xz, side) + P.gust3.w;
        float n = 0.65 * GustNoise(vec3(along / 70.0, h / 22.0 + P.gust2.w * 0.015, across / 40.0)) +
                  0.35 * GustNoise(vec3(along / 24.0 + 3.1, h / 9.0, across / 14.0 + 7.7));
        // (Between billows never under ~half as thick: at 0.1 the gaps let a third of land 700 units off show
        // through - with a low sun, grey-blue ridges and the march's dither on them in an orange-brown storm - the
        // user. The same average.)
        float billow = (0.45 + 2.0 * smoothstep(0.34, 0.68, n)) * mix(1.4, 0.55, smoothstep(0.0, 40.0, h));
        float near_ = smoothstep(P.gust2.y, 1.6 * P.gust2.y, t * L);   // (none yet where the near sand is)
        sum += mix(1.0, billow, near_ * P.gust2.z);
    }
    return z - (t1 - t0) + sum * dt;
}

// The valley mist (rvk mist.cpp) along the view ray V (view space, length dist): a layer up to P.fog.z over the eye,
// fading over ~8 units above, and the fog banks (FogBanks). phase: the air's glow towards the sun (Air).
// The twilight's difference to the game's fog colour along the horizon towards a view-space direction
// (sky_common.glsl SkyHorizonDelta): the game's far fog, the haze and the mist in the sky's colours.
vec3 gSkyDelta = vec3(0.0);
vec3 SkyDelta(vec3 Vv)
{
    vec3 w = vec3(dot(FL.skyView[0].xyz, Vv), dot(FL.skyView[1].xyz, Vv), dot(FL.skyView[2].xyz, Vv));
    return SkyHorizonDeltaAll(w, FL.sky[0], FL.sky[1], FL.sky[3], FL.skyRing);
}

// The air's colour along the pixel's ray (gSkyDelta set): the zone's haze in the twilight's colours - or with our own
// sky (FL.skyRing) its horizon there, paled a little as the haze is.
vec3 HazeColour()
{
    if (FL.skyRing[0].w > 0.5) {
        vec3 r = FL.sky[0].rgb + gSkyDelta;     // (the horizon: SkyHorizonDeltaAll gives it minus the fog)
        float l = dot(r, vec3(0.3, 0.59, 0.11));
        // (In a desert's dust, FL.desert.x: paled warm, not cool.)
        return max(r + (l * mix(vec3(0.78, 0.95, 1.25), vec3(1.1, 1.0, 0.82), FL.desert.x) - r) * 0.2, vec3(0.0));
    }
    return P.haze.rgb + gSkyDelta;
}

// Value noise on the ground's plane (the fog banks' shapes).
float BankHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float BankNoise(vec2 p)
{
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    return mix(mix(BankHash(i), BankHash(i + vec2(1.0, 0.0)), u.x), mix(BankHash(i + vec2(0.0, 1.0)), BankHash(i + 1.0), u.x),
               u.y);
}

// Fog banks (rvk mist.cpp; FL.fogBanks): the mist gathered into banks ~700 units across with clear ground between,
// each standing up to FL.fogBanks.z over the low ground (the heavier, the taller), soft at its top, drifting with the
// wind - their optical depth along the view ray V (view space, length dist), marched in 10 steps over its first 1500
// units (growing; offset per pixel, so no bands).
float FogBanks(vec3 V, float dist)
{
    float len = min(dist, 1500.0);
    vec3 Vn = V / max(dist, 1e-4);
    vec3 w = vec3(dot(FL.skyView[0].xyz, Vn), dot(FL.skyView[1].xyz, Vn), dot(FL.skyView[2].xyz, Vn));
    float lowGround = P.fog.z - 25.0;            // (the low ground under the eye, against the eye: mist.cpp kMistDepth)
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    const int kSteps = 10;
    float tau = 0.0, t0 = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        float t1 = len * pow((float(i) + 1.0) / float(kSteps), 1.6);
        float t = mix(t0, t1, jitter);
        vec3 r = w * t;
        float h = r.y - lowGround;               // (over the low ground)
        if (h < FL.fogBanks.z + 10.0) {
            vec2 q = (FL.fogBanks.xy + r.xz) / 700.0;
            float n = 0.65 * BankNoise(q) + 0.35 * BankNoise(q * 2.3 + 17.0);
            float cover = smoothstep(0.42, 0.68, n);
            float top = FL.fogBanks.z * (0.45 + 0.55 * cover);
            tau += cover * (1.0 - smoothstep(top - 18.0, top + 6.0, h)) * (t1 - t0);
        }
        t0 = t1;
    }
    return tau * 0.02 * FL.fogBanks.w;
}

vec3 Mist(vec3 c, vec3 V, float dist, float phase)
{
    // Its depth along the ray: the density's integral over the heights the ray crosses, over how fast it climbs - and
    // the fog banks'. Lit as the air - the zone's colour, paler - and the sun's (moon's) glow.
    if (P.fog.w > 0.0 || FL.fogBanks.w > 0.0) {
        float h0 = 0.0, h1 = dot(V, P.air1.xyz), top = P.fog.z;
        float tauM;
        if (abs(h1 - h0) < 0.5) {
            float h = 0.5 * (h0 + h1);
            tauM = dist * (h < top ? 1.0 : exp(-(h - top) / 8.0));
        } else {
            // (G: the density's integral from -inf: full below the top, then 8 units more fading.)
            #define MIST_G(y) ((y) < top ? (y) : top + 8.0 * (1.0 - exp(-((y) - top) / 8.0)))
            tauM = dist * (MIST_G(h1) - MIST_G(h0)) / (h1 - h0);
            #undef MIST_G
        }
        tauM *= P.fog.w;
        if (FL.fogBanks.w > 0.0) tauM += FogBanks(V, dist);
        float m = 1.0 - exp(-tauM);
        vec3 air = HazeColour();
        float l = dot(air, vec3(0.3, 0.59, 0.11));
        vec3 mist = mix(air, vec3(l), 0.45) * 1.2 + P.air3.rgb * (phase * P.air1.w * 0.5);
        c = mix(c, mist, m);
    }
    return c;
}

// A rainbow (rvk sky.cpp UpdateRainbow; FL.neon.yzw: towards the point opposite the sun, x its strength) over the sky
// and the clouds - not the land: the bow round that point, the primary from 40.6 (violet, inside) to 42.4 degrees
// (red), the secondary from 50.4 (red) to 53.4 (violet) at ~40%, Alexander's dark band between them a little darker
// and the sky inside the primary a little lighter; fading into the haze towards the horizon. Its light scales with
// what is behind it (as bright as the sunlit rain), at least a little over dark clouds.
// The rainbow's colours by where in the bow (0 violet .. 1 red).
vec3 Spectrum(float t)
{
    float r = smoothstep(0.45, 0.8, t) + 0.35 * (1.0 - smoothstep(0.0, 0.18, t));
    float g = clamp(1.0 - abs(t - 0.5) * 2.6, 0.0, 1.0);
    float b = 1.0 - smoothstep(0.15, 0.55, t);
    return vec3(r, g, b);
}

vec3 Rainbow(vec3 c)
{
    float strength = length(FL.neon.yzw);
    if (strength < 1e-3 || FL.desert.z <= 0.0 || FL.desert.w <= 0.0)
        return c;
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0));
    vec3 ray = normalize(vec3((uv.x * 2.0 - 1.0) / FL.desert.z, (1.0 - uv.y * 2.0) / FL.desert.w, 1.0));
    float up = dot(FL.skyView[1].xyz, ray);
    float low = smoothstep(0.0, 0.08, up);
    if (low <= 0.0)
        return c;
    float a = degrees(acos(clamp(dot(ray, FL.neon.yzw / strength), -1.0, 1.0)));
    if (a < 25.0 || a > 58.0)
        return c;
    float l = dot(c, vec3(0.3, 0.59, 0.11));
    float t1 = (a - 40.6) / 1.8, t2 = (53.4 - a) / 3.0;   // (each 0 violet .. 1 red)
    float e1 = smoothstep(-0.3, 0.1, t1) * (1.0 - smoothstep(0.9, 1.3, t1));
    float e2 = smoothstep(-0.3, 0.1, t2) * (1.0 - smoothstep(0.9, 1.3, t2));
    vec3 bow = Spectrum(clamp(t1, 0.0, 1.0)) * e1 + Spectrum(clamp(t2, 0.0, 1.0)) * e2 * 0.4;
    float inside = a < 40.6 ? 0.05 * smoothstep(25.0, 34.0, a) : 0.0;
    float band = smoothstep(42.0, 43.0, a) * (1.0 - smoothstep(49.8, 50.8, a));
    float k = min(strength, 2.0) * low;
    c *= 1.0 - 0.1 * band * k;
    return c + (bow * (0.12 + 0.55 * l) + vec3(inside * l)) * k;
}

// The pixel's view ray out to view depth z: V (its length dist) and the air's glow towards the sun along it.
void AirRay(float z, out vec3 V, out float dist, out float phase)
{
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0));
    V = vec3((uv.x * 2.0 - 1.0) / P.air0.x, (1.0 - uv.y * 2.0) / P.air0.y, 1.0) * z;
    dist = length(V);
    float g = P.air2.w, cosT = dot(V / max(dist, 1e-4), P.air2.xyz);
    phase = min((1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosT, 1.5), 5.0);
}

vec3 Air(vec3 c, vec3 V, float dist, float phase)
{
    float x = dot(V, P.air1.xyz) / P.air0.w;      // (height over the eye, in scale heights)
    float f = abs(x) < 1e-3 ? 1.0 : (1.0 - exp(-x)) / x;
    float tau = P.air0.z * dist * clamp(f, 0.0, 4.0);
    vec3 through = exp(-tau * vec3(0.8, 1.0, 1.3));
    vec3 scattered = (1.0 - through) * HazeColour() + (1.0 - exp(-tau)) * P.air3.rgb * (phase * P.air1.w);
    return c * through + scattered;
}

// Heat shimmer (RVK_HeatShimmer; FL.desert.y, rvk Device::DesertAir): over a desert's distant land in a high sun the
// hot air bends the light, and the land low on the screen near the horizon wavers - where the scene is read moved by up
// to ~1.5 pixels at 1440 lines (between pixels: read bilinear), in thin, wide cells drifting upwards. Only land past ~60
// units (what's near, characters with it, stays still); not the sky. Where the moved read would take something much
// nearer than the pixel (an edge in front), the pixel's own. Returns the offset in pixels (0 0: none).
vec2 HeatShimmer(float d)
{
    float z = P.proj.y / min(d - P.proj.x, -1e-9);
    float far_ = smoothstep(60.0, 140.0, z);
    if (far_ <= 0.0 || FL.desert.z <= 0.0 || FL.desert.w <= 0.0)
        return vec2(0.0);
    vec2 size = vec2(textureSize(scene, 0));
    vec2 uv = gl_FragCoord.xy / size;
    vec3 ray = normalize(vec3((uv.x * 2.0 - 1.0) / FL.desert.z, (1.0 - uv.y * 2.0) / FL.desert.w, 1.0));
    float up = dot(FL.skyView[1].xyz, ray);      // (the ray's world height: level near the horizon)
    float level = smoothstep(-0.16, -0.04, up) * (1.0 - smoothstep(0.01, 0.05, up));
    float amount = FL.desert.y * far_ * level;
    if (amount <= 0.0)
        return vec2(0.0);
    float t = FL.wind.z;
    vec2 p = gl_FragCoord.xy / size.y * vec2(45.0, 160.0);
    vec2 wave = vec2(GustNoise(vec3(p.x, p.y + t * 9.0, t * 0.8)), GustNoise(vec3(p.x + 17.3, p.y + t * 9.0 + 5.1, t * 0.8 + 3.0)));
    vec2 off = (wave - 0.5) * 2.0 * vec2(0.5, 1.0) * (1.5 * size.y / 1440.0) * amount;
    ivec2 moved = clamp(ivec2(gl_FragCoord.xy + off), ivec2(0), ivec2(size) - 1);
    float dm = texelFetch(depthTex, moved, 0).r;
    if (dm < 1.0 && P.proj.y / min(dm - P.proj.x, -1e-9) < 0.8 * z)
        return vec2(0.0);
    return off;
}

// The scene at a pixel position (gl_FragCoord's units), bilinear between its pixels (it is bound point-sampled).
vec4 SceneAt(vec2 pos)
{
    vec2 size = vec2(textureSize(scene, 0)), p = pos - 0.5, f = fract(p);
    ivec2 a = clamp(ivec2(floor(p)), ivec2(0), ivec2(size) - 1), b = clamp(a + 1, ivec2(0), ivec2(size) - 1);
    return mix(mix(texelFetch(scene, a, 0), texelFetch(scene, ivec2(b.x, a.y), 0), f.x),
               mix(texelFetch(scene, ivec2(a.x, b.y), 0), texelFetch(scene, b, 0), f.x), f.y);
}

void main()
{
    // How far the pixel is: its view depth over the far clip's (the sky: wholly).
    float d = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
    vec2 shimmer = FL.desert.y > 0.0 && d < 1.0 && (int(P.proj.w) & 16) != 0 ? HeatShimmer(d) : vec2(0.0);
    vec4 s = shimmer != vec2(0.0) ? SceneAt(gl_FragCoord.xy + shimmer) : texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
    vec3 glow = P.params.z > 0.0 ? Bloom() * P.params.z : vec3(0.0);
    // The neon look (RVK_NeonSat): the halo a little more saturated - coloured light glows its colour.
    if (FL.neon.x > 0.0) {
        float gl = dot(glow, vec3(0.3, 0.59, 0.11));
        glow = max(mix(vec3(gl), glow, 1.0 + 0.35 * FL.neon.x), vec3(0.0));
    }
    vec3 c = Reflected(s.rgb * AmbientFactor() + Indirect());
    float far = 1.0;
    // The rain (rvk rain.cpp's rain layer: not in the scene - its reflected light, premultiplied, and cover): laid
    // over the finished picture below, after the haze, the storm's fog and the dust - a few units off, it is veiled by
    // none of them. (Veiled as far as what was behind it, the streaks vanished where they crossed the horizon - the
    // user.)
    vec4 rain = FL.rainNear.x > 0.5 ? texelFetch(rainNear, ivec2(gl_FragCoord.xy), 0) : vec4(0.0);
    // Lightning (rvk rain.cpp Lightning): the strike's light on everything by its own colour, falling off away from
    // it - by night the land lit as by day for an instant - and in the sky the clouds round the strike glowing.
    if (FL.flash.w > 0.0) {
        vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0));
        vec3 ray = vec3((uv.x * 2.0 - 1.0) / FL.flashProj.x, (1.0 - uv.y * 2.0) / FL.flashProj.y, 1.0);
        vec3 a = texelFetch(albedo, ivec2(gl_FragCoord.xy), 0).rgb;
        vec3 strike = FL.flashView.xyz * FL.flashView.w;
        if (d < 1.0) {
            float z = FL.flashProj.w / min(d - FL.flashProj.z, -1e-9);
            float apart = distance(ray * z, strike);
            c += a * FL.flash.rgb / (1.0 + apart * apart / (450.0 * 450.0));
        } else {
            float near_ = exp((dot(normalize(ray), FL.flashView.xyz) - 1.0) / 0.025);
            c += a * FL.flash.rgb * 0.5 + vec3(0.8, 0.85, 1.0) * (FL.flash.w * near_ * 0.6);
        }
    }
    if ((int(P.proj.w) & 16) == 0) {
        far = 0.0;                               // (no depth readable: warmth everywhere)
    } else if (d < 1.0) {
        // (Over the far clip - the game's fog end; with no far clip to speak of, over 3 km.)
        // (D3D: m[2][2] = f / (f - n) > 1, m[3][2] = -n m[2][2] < 0 - both terms of the view depth negative.)
        float z = P.proj.y / min(d - P.proj.x, -1e-9);
        float zFar = P.proj.x > 1.000001 ? P.proj.y / (1.0 - P.proj.x) : 3000.0;
        far = clamp(z / zFar, 0.0, 1.0);
        // The game's far fog in the sky's twilight colours (SkyDelta): as much as the game fogged the land, the
        // difference the twilight makes to its colour there - the land fades into the sky drawn behind it.
        // (Our own sky - FL.skyRing: the scene already fogged into its horizon, rvk draw.cpp; only the haze's colour.)
        if (P.fog.y > P.fog.x && P.air0.x != 0.0) {   // (no view rays before the first world camera)
            vec3 Vd;
            float dd, ph;
            AirRay(z, Vd, dd, ph);
            gSkyDelta = SkyDelta(Vd / max(dd, 1e-4));
            if (FL.skyRing[0].w <= 0.5)
                c = max(c + gSkyDelta * clamp((z - P.fog.x) / (P.fog.y - P.fog.x), 0.0, 1.0), vec3(0.0));
        }
        // The haze over the distant land (RVK_VolLand): the air between the eye and the land, thickening from the
        // eye out - each ridge farther paler and bluer - and giving way to the game's own fog where that starts
        // (its fog end the far land's colour: no seam with the sky).
        if (P.haze.w > 0.0 && P.fog.y > P.fog.x) {
            float gameFog = clamp((z - P.fog.x) / (P.fog.y - P.fog.x), 0.0, 1.0);
            if (P.air0.z > 0.0) {
                vec3 V;
                float dist, phase;
                AirRay(z, V, dist, phase);
                float k = min(P.haze.w * 2.5, 1.0);
                c = mix(c, Air(c, V, dist, phase), k * (1.0 - gameFog));
                // (The mist not handed over to the game's fog: the sky behind gets it too, below - one layer.)
                c = mix(c, Mist(c, V, dist, phase), k);
            } else {
                float air = 1.0 - exp(-z / (0.4 * P.fog.y));
                c = mix(c, P.haze.rgb, P.haze.w * air * (1.0 - gameFog));
            }
        }
    } else if (P.haze.w > 0.0 && P.fog.y > P.fog.x && P.air0.z > 0.0 && (P.fog.w > 0.0 || FL.fogBanks.w > 0.0)) {
        // The sky through the valley mist too, as far as the game's fog end: else a mist over the land stopped at
        // the hilltops in a hard line against the sky (ao-vk #14). Level rays stay in the layer, rising ones leave it.
        vec3 V;
        float dist, phase;
        AirRay(P.fog.y, V, dist, phase);
        gSkyDelta = SkyDelta(V / max(dist, 1e-4));
        c = mix(c, Mist(c, V, dist, phase), min(P.haze.w * 2.5, 1.0));
    }
    // A sand or ash storm (rvk wind.cpp): the air thick with sand to the horizon - the sky wholly, the land through a
    // veil that thickens with distance, surging with the wind.
    bool smokeNear = mod(P.dust2.w, 2.0) > 0.5;
    float fogBits = mod(P.dust2.w, 4.0), rainShare = floor(P.dust2.w / 4.0 + 0.01) / 15.0;   // (+ 4 x the rain mist's share in 15ths)
    float volumetric = 1.0;                      // (the sun's shafts: behind the fog only as much as shows through)
    if (P.dust.w > 0.0 && fogBits > 1.5) {
        // The storm's air a lit volume (RVK_SandFog, sand_fog.frag): what is behind shows by its transmittance, the
        // light the dust scatters over it. The blowing sand (near: in front of nearly all of it) kept whole - its own
        // light S and cover a - the fog over what shows through it.
        vec4 fog = SandFogAt();
        float T = clamp(fog.a, 0.0, 1.0);
        // What shows through reddened: the dust takes blue first (exp(-k tau) = T^k per colour) - distant hills go
        // red-brown in the storm, not just paler (the user; at 0.6 / 1.6 the nearer hills went strangely red).
        vec3 Tc = pow(vec3(max(T, 1e-6)), mix(vec3(0.8, 1.0, 1.25), vec3(1.0), rainShare));   // (the rain mist: grey)
        // The light on the land under the storm the dust's colour (as bright): the sun's light the dust took on its
        // way down reaches the land scattered by it - orange under an orange sky, not the sun's own white (the user).
        // The sky's pixels not: the fog holds them.
        if (d < 1.0 && P.dust2.z > 0.0) {
            vec3 tint = P.dust.rgb / max(dot(P.dust.rgb, vec3(0.3, 0.59, 0.11)), 1e-4);
            Tc *= mix(vec3(1.0), min(tint, vec3(2.5)), P.dust2.z);
        }
        if (smokeNear) {
            vec4 smoke = texture(smokeCover, gl_FragCoord.xy / vec2(textureSize(scene, 0)));
            float a = clamp(smoke.a, 0.0, 1.0);
            vec3 S = min(smoke.rgb, c);
            c = S + (c - S) * Tc + fog.rgb * (1.0 - a);
            // (What lies behind as far and its shafts as hidden as where no sand blows: kept for the sand's share,
            // they brought the distant dunes' outline back out through it - the user.)
            far *= T;
            volumetric = T;
            glow *= Tc;                          // (the scene the bloom came from holds the blowing sand already)
        } else {
            c = c * Tc + fog.rgb;
            far *= T;
            volumetric = T;
            // (The bloom - made from the scene before the fog - only as much as shows through: else what the dust
            // hides glowed through it, a ship in the sky ringed with light - the user.)
            glow *= Tc;
        }
    } else if (P.dust.w > 0.0) {
        float dist = d < 1.0 ? P.proj.y / min(d - P.proj.x, -1e-9) : 1e5;
        vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0));
        float surge = 1.0 + P.dust2.y * (0.6 * sin(P.dust2.x * 0.9 + uv.x * 5.0) + 0.4 * sin(P.dust2.x * 2.3 - uv.y * 7.0 + uv.x * 3.0));
        // (Sand in the air right in front too: a veil at every distance, P.dust2.z, under the haze that thickens
        // with it.)
        // (The distant gusts, RVK_SandGusts: the haze thicker and thinner in billows drifting with the wind; without
        // them, the old surge - waves over the screen.)
        float depth = P.gust2.z > 0.0 ? DustDepth(uv, dist) : dist * max(surge, 0.2);
        float through = exp(-P.dust.w * depth) * (1.0 - P.dust2.z * clamp(surge, 0.5, 1.5));
        through = clamp(through, 0.0, 1.0);
        // Smoke and blowing sand (drawn over the scene before this, without depth) are near: the pixel taken apart
        // again - the smoke's own light S and cover a (the half-size smoke image, premultiplied), what is behind it
        // c - S - and each veiled by its own distance: the smoke by a few units' air, what is behind by its own. (A
        // single veil by the depth behind gave the sand over the sky the sky's full haze and over near leaves next to
        // none - patchy; and with it set by the cover, thin sand looking up cleared the sky's haze - the user.)
        if (smokeNear) {
            vec4 smoke = texture(smokeCover, uv);
            float a = clamp(smoke.a, 0.0, 1.0);
            vec3 S = min(smoke.rgb, c);
            float near_ = clamp(exp(-P.dust.w * 6.0 * max(surge, 0.2)) * (1.0 - P.dust2.z * clamp(surge, 0.5, 1.5)), 0.0, 1.0);
            c = S * near_ + P.dust.rgb * a * (1.0 - near_) + mix(P.dust.rgb * (1.0 - a), c - S, through);
            through = mix(through, near_, a);
        } else {
            c = mix(P.dust.rgb, c, through);
        }
        // What the sand hides is no longer sky: the night's and the grading's sky (by far) only as much as shows
        // through - else a darker, greyer band of "sky" over a storm that covers it.
        far *= through;
    }
    // A rainbow after rain over the sky and the clouds (not the land: the sky's depth).
    if (d >= 1.0 && (int(P.proj.w) & 16) != 0)
        c = Rainbow(c);
    // Water: a drop hides a little of what is behind it, gives some back focused (a lens: brighter), and adds the light
    // it reflects - clear, lighter streaks, not painted ones (the user: as our water).
    float rainA = clamp(rain.a, 0.0, 1.0);
    c = c * (1.0 - 0.15 * rainA) + c * (0.2 * rainA) + rain.rgb;
    c = max((c + Volumetric() * volumetric + glow) * P.params.y, vec3(0.0));
    // Night (RVK_NightDark x how much it is night): darker - the darks and middle tones down, the sky more; what is
    // bright kept (lamps, windows, the moon, stars, moonlit cloud edges), so it stands out of the dark.
    float nightDark = P.grade2.x * P.grade2.w;
    if (P.grade2.w < 0.0) {
        // The light rig's land exposure (rvk light_rig.cpp): the same darks and middle tones down (or up), the bright
        // kept - by the hour's physical light against the game's own, not by a night ramp. The sky a little more, as
        // below, by as far as the land is taken down.
        float g = -P.grade2.w;
        float sky = smoothstep(0.7, 1.0, far);
        float bright = smoothstep(mix(0.25, 0.35, sky), mix(0.8, 0.9, sky), dot(c, vec3(0.3, 0.59, 0.11)));
        float down = g * mix(1.0, 0.8, sky * clamp((1.0 - g) / 0.38, 0.0, 1.0));
        c *= mix(down, 1.0, bright);
    } else if (nightDark > 0.0) {
        // (Nearer, from a lower brightness: lamp-lit ground keeps most of its light; the sky and clouds as tuned.)
        float sky = smoothstep(0.7, 1.0, far);
        float bright = smoothstep(mix(0.25, 0.35, sky), mix(0.8, 0.9, sky), dot(c, vec3(0.3, 0.59, 0.11)));
        float down = exp2(-0.7 * nightDark) * mix(1.0, 0.8, smoothstep(0.7, 1.0, far) * min(nightDark, 1.0));
        c *= mix(down, 1.0, bright);
    }
    float m = max(c.r, max(c.g, c.b)), k = P.params.x;
    if (m > k) {
        float room = 1.0 - k;
        float shown = room > 0.0 ? k + room * (1.0 - exp(-(m - k) / room)) : 1.0;
        float white = m > 1.0 ? 1.0 - 1.0 / (1.0 + 0.25 * (m - 1.0)) : 0.0;   // 0 at 1, 0.2 at 2, 0.5 at 5
        // (The neon look, RVK_NeonSat: saturated light - signs, screens, coloured lamps - keeps its colour as it rolls
        // off; near-grey light - the sun on pale stone - still goes white.)
        float sat = (m - min(c.r, min(c.g, c.b))) / m;
        white *= 1.0 - FL.neon.x * smoothstep(0.35, 0.8, sat);
        c = mix(c * (shown / m), vec3(shown), white);
    }
    // Alpha: with TAA (flag 16), how much of this pixel additive effects made - their glow, as shown - and the rain's
    // near streaks: new each frame, the TAA (taa.frag) and FSR 2 (its reactive mask, rvk fsr2.cpp) give those pixels
    // more of the new frame (FSR 2 averaged the streaks away); else the scene's alpha.
    float alpha = clamp(s.a, 0.0, 1.0);
    if ((int(P.proj.w) & 16) != 0) {
        vec3 effect = texelFetch(glowRaw, ivec2(gl_FragCoord.xy), 0).rgb * P.params.y;
        alpha = max(smoothstep(0.01, 0.25, dot(effect, vec3(0.3, 0.59, 0.11))), smoothstep(0.0, 0.3, rain.a));
    }
    outColor = vec4(Dither8(Grade(min(c, vec3(1.0)), far), gl_FragCoord.xy, FL.taa.z), alpha);   // (moving: the TAA averages it)
}
