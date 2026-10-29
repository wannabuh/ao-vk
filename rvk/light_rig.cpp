// The light rig (docs/light-rig-plan.md): one account of the frame's light for day and night alike. Day and night
// were lit by separate means - the game's sun on lit things and its baked lightmap on the ground by day; by night the
// game's light from under the ground faded out, our moon added, a dark night lightmap, the shadow map switching to the
// moon - each with its own fixes, and the hand-over at dusk in steps. Here the sun, the sky and the moon are each a
// physical shape by the sun's height (the air's transmittance for the sun, the sky's light through twilight, the
// moon's light at a few millionths of the sun's), a storm takes its share from all of them, and the eye adapts to the
// sum (RVK_Darkness: how far). The result is in the game's own units - each zone's day light, measured by day, is what
// noon is - and exposed already: the scene's half floats would not hold a night at a millionth of the day.
// So far computed and logged only, beside what the game gives now.
#include "internal.h"

#include <algorithm>
#include <cmath>

namespace rvk {

using namespace detail;

namespace {

constexpr float kSunLux = 1.1e5f;                // the noon sun on a surface facing it
constexpr float kMoonShare = 2.7e-6f;            // a full moon high up, of that (~0.3 lux)

// The sky's light on level ground (log10 lux) by the sun's elevation (degrees): ~20000 lux at noon, a few hundred
// at sunset, ~3 at the end of civil twilight (6 under), a thousandth past astronomical (18). (Typical clear-sky
// measurements; the night floor - airglow, starlight - ~0.0005 lux.)
float SkyLuxLog(float sunUpSine)
{
    static const float kDeg[] = {-18.0f, -16.0f, -14.0f, -12.0f, -10.0f, -8.0f, -6.0f, -4.0f, -2.0f, 0.0f, 2.0f,
                                 5.0f, 10.0f, 20.0f, 30.0f, 60.0f, 90.0f};
    static const float kLog[] = {-3.3f, -3.0f, -2.6f, -2.0f, -1.3f, -0.4f, 0.5f, 1.3f, 2.0f, 2.6f, 3.1f,
                                 3.5f, 3.8f, 4.0f, 4.15f, 4.25f, 4.3f};
    constexpr int n = int(sizeof(kDeg) / sizeof(kDeg[0]));
    const float e = std::asin(std::clamp(sunUpSine, -1.0f, 1.0f)) * 57.29578f;
    if (e <= kDeg[0]) return kLog[0];
    if (e >= kDeg[n - 1]) return kLog[n - 1];
    for (int i = 0; i + 1 < n; ++i)
        if (e <= kDeg[i + 1])
            return kLog[i] + (kLog[i + 1] - kLog[i]) * (e - kDeg[i]) / (kDeg[i + 1] - kDeg[i]);
    return kLog[n - 1];
}

}  // namespace

// Once a frame, after the moon and the sky's model.
void Device::UpdateLightRig()
{
    LightRig& r = m_rig;
    const float h = m_cond.sunUp;
    r.on = m_cond.sunKnown && m_lightRig;
    r.sunUp = h;
    if (!r.on) {
        // (Indoors: the game's light as it is; the adaptation starts afresh outside.)
        r.gameSun = m_sunLuminance;
        r.gameAmbient = std::max(m_sceneAmbient, 0.0f);
        r.sunGain = r.skyGain = 1.0f;
        r.landGain = r.meter = 1.0f;
        m_rigAdaptSet = m_rigGainSet = false;
        return;
    }
    // The sun on a surface facing it: the air's transmittance by its height, over noon's; its disc setting from
    // half a degree up to a degree under (refraction keeps it a little past the horizon).
    float hue[3], t = 0.0f, tNoon = 1.0f;
    SunThroughAir(h, hue, &t);
    SunThroughAir(1.0f, hue, &tNoon);
    r.sunN = t / std::max(tNoon, 1e-4f) * Smoothstep(-0.0175f, 0.0087f, h);
    float t10 = 1.0f;
    SunThroughAir(0.1736f, hue, &t10);
    r.sunFade = std::min(r.sunN * std::max(tNoon, 1e-4f) / std::max(t10, 1e-4f), 1.0f);
    r.sky = std::pow(10.0f, SkyLuxLog(h) - SkyLuxLog(1.0f));
    r.moonN = kMoonShare * std::max(m_moonLight, 0.0f) * std::clamp(m_moonFade, 0.0f, 1.0f);
    // A thunderstorm takes the sun's and the moon's direct light (as lighting.glsl), and the sky's by StormSkyLight.
    r.storm = StormOvercast();
    const float direct = 1.0f - kStormSunTaken * r.storm, skyKept = StormSkyLight();
    const float sun = r.sunN * direct, moon = r.moonN * direct, sky = r.sky * skyKept;
    const float moonUp = std::max(-m_moonDir[1], 0.0f);
    const float skyNoonLux = std::pow(10.0f, SkyLuxLog(1.0f)), noonLux = kSunLux + skyNoonLux;
    r.land = kSunLux * (sun * std::max(h, 0.0f) + moon * moonUp) + skyNoonLux * sky;
    {
        // (Clear air: a storm's own share - the sun it hides - is the shaders' FL.rainNear.y.)
        const float sunGround = kSunLux * r.sunN * std::max(h, 0.0f), skyGround = skyNoonLux * r.sky;
        const float noon = kSunLux * tNoon / std::max(tNoon, 1e-4f) / (kSunLux + skyNoonLux);
        r.sunShare = sunGround > 0.0f ? std::min(sunGround / (sunGround + skyGround) / noon, 1.0f) : 0.0f;
    }
    // The eye's adaptation: alpha of the way to seeing every light level alike (RVK_Darkness 0 lighter nights, 2
    // darker), eased in its log over ~2 s.
    // (0.905 at RVK_Darkness 1: a moonlit night's land at ~0.6 of what the game's own night light gives it - as the
    // old night darkening had it -, a moonless one darker, the blue hour about untouched.)
    const float alpha = 0.935f - 0.03f * std::clamp(m_darkness, 0.0f, 2.0f);
    const float target = std::log(std::max(r.land, 1e-6f) / noonLux) * -alpha;
    float la = std::log(std::max(r.adapt, 1e-6f));
    la = m_rigAdaptSet ? la + (target - la) * (1.0f - std::exp(-m_frameDt / 2.0f)) : target;
    m_rigAdaptSet = true;
    r.adapt = std::min(std::exp(la), 1e8f);
    // The ground's baked sky: its lightmap round the camera (the grass's capture: the eye's cell and a ring 5 units
    // out), eased over ~2 s, less the sun's part of it by its physical share of the light on level ground.
    {
        float sum[3] = {}, n = 0.0f;
        for (int k = 0; k < 9; ++k) {
            const float a = float(k) * 0.785398f, rr = k == 0 ? 0.0f : 5.0f;
            float y;
            uint32_t light = 0;
            if (!GroundAt(m_frameEye[0] + rr * std::cos(a), m_frameEye[2] + rr * std::sin(a), &y, nullptr, &light) ||
                (light >> 24) == 0)
                continue;
            sum[0] += float((light >> 16) & 0xFF) / 255.0f;
            sum[1] += float((light >> 8) & 0xFF) / 255.0f;
            sum[2] += float(light & 0xFF) / 255.0f;
            n += 1.0f;
        }
        if (n > 0.0f) {
            const float k = r.groundKnown ? 1.0f - std::exp(-m_frameDt / 2.0f) : 1.0f;
            for (int i = 0; i < 3; ++i) r.groundLight[i] += (sum[i] / n - r.groundLight[i]) * k;
            r.groundKnown = true;
        }
        const float sunGround = kSunLux * r.sunN * std::max(h, 0.0f), skyGround = skyNoonLux * r.sky;
        const float skyPart = sunGround + skyGround > 0.0f ? skyGround / (sunGround + skyGround) : 1.0f;
        for (int i = 0; i < 3; ++i) r.groundSky[i] = r.groundKnown ? r.groundLight[i] * skyPart * skyKept : 0.0f;
    }
    // The key light: the sun's or the moon's way.
    r.keySun = sun + moon > 0.0f ? sun / (sun + moon) : 1.0f;
    const float* dir = r.keySun >= 0.5f ? m_trueSunDir : m_moonDir;
    for (int i = 0; i < 3; ++i) r.keyDir[i] = dir[i];
    // The zone's day light: the game's sun and ambient while the sun is well up (over ~25 degrees), the ambient taken
    // back to noon's by the sky's shape; eased over ~10 s. A zone not yet seen by day: the default's.
    r.gameSun = m_sunLuminance;
    r.gameAmbient = std::max(m_sceneAmbient, 0.0f);
    // (The zone's references only once it and the game's ambient have been seen steadily for ~3 s: in its first frames
    // the ambient is whatever draw set it first, and a reference taken then put the game's light at 6.5 x its noon.)
    const bool ambientFresh = m_sceneAmbient >= 0.0f && m_sceneAmbientFrame != ~0ull && m_sceneAmbientFrame + 2 >= m_frameNumber;
    if (!ambientFresh || r.zone != m_skyPlayfield) m_rigSteadySince = -1.0;
    else if (m_rigSteadySince < 0.0) m_rigSteadySince = m_frameClock;
    const bool steady = m_rigSteadySince >= 0.0 && m_frameClock - m_rigSteadySince > 3.0;
    if (r.zone != m_skyPlayfield) {
        r.zone = m_skyPlayfield;
        const auto it = m_rigZones.find(r.zone);
        r.zoneMeasured = it != m_rigZones.end();
        r.zoneSun = r.zoneMeasured ? it->second.sun : 1.0f;
        r.zoneSky = r.zoneMeasured ? it->second.sky : 0.4f;
    }
    if (steady && h > 0.42f && m_sunLuminance > 0.0f && m_sunDir[1] < 0.0f) {
        const float sunNow = m_sunLuminance, skyNow = m_sceneAmbient / std::max(r.sky, 1e-3f);
        const float k = r.zoneMeasured ? 1.0f - std::exp(-m_frameDt / 10.0f) : 1.0f;
        r.zoneSun += (sunNow - r.zoneSun) * k;
        r.zoneSky += (skyNow - r.zoneSky) * k;
        r.zoneMeasured = true;
        RigZone& zone = m_rigZones[r.zone];
        zone.sun = r.zoneSun;
        zone.sky = r.zoneSky;
    }
    // The land's exposure: the target (clear air) over the game's land now against its noon's.
    {
        const float clearLand = kSunLux * (r.sunN * std::max(h, 0.0f) + r.moonN * moonUp) + skyNoonLux * r.sky;
        r.landTarget = std::pow(std::max(clearLand, 1e-6f) / noonLux, 1.0f - alpha);
        const float groundL = 0.3f * r.groundLight[0] + 0.59f * r.groundLight[1] + 0.11f * r.groundLight[2];
        const float game = (r.groundKnown ? groundL : r.gameAmbient) + r.gameAmbient;
        RigZone& z = m_rigZones[r.zone];
        if (steady && h > 0.42f && game > 0.0f) {
            const float k = z.landSeen ? 1.0f - std::exp(-m_frameDt / 10.0f) : 1.0f;
            z.land += (game / std::max(r.landTarget, 1e-3f) - z.land) * k;   // (the noon it implies)
            z.landSeen = true;
        }
        // (A zone not yet seen by day: the game's usual day - ~0.72 lightmap and ~0.37 ambient - or the ambient's
        // double without the grass's capture.)
        const float noonGame = z.landSeen ? z.land : r.groundKnown ? 1.09f : 0.74f;
        r.landGame = game > 0.0f ? game / noonGame : 1.0f;
        // (RVK_Darkness under 1 takes that much of it, in its log: 0 leaves the land at the game's own light, as nights
        // were before the rig at 0 - the physical night at 0 still took a moonless night to ~0.6 of it, which players
        // found too dark.)
        const float share = std::clamp(m_darkness, 0.0f, 1.0f);
        const float target = std::pow(std::clamp(r.landTarget / std::max(r.landGame, 1e-3f), 0.2f, 1.5f), share);
        r.landGain = m_rigGainSet ? r.landGain + (target - r.landGain) * (1.0f - std::exp(-m_frameDt / 1.5f)) : target;
        // The camera's metering on top, for everything alike (the land, the sky, the clouds, lamps): the whole view's
        // brightness over the land's (scene_luma.comp, every other frame each), against the zone's by day (the sun
        // over ~25 degrees, steady) - its RVK_Metering power (0.5: the square root) taken off. A sunset sky much
        // brighter than the land takes the exposure down - the land towards silhouettes -, a view as by day leaves it;
        // at most x0.4, eased over ~1.5 s.
        const bool fresh = m_viewLumaFrame != ~0ull && m_viewLumaFrame + 30 >= m_frameNumber && m_viewLuma > 0.0f &&
                           m_landLumaFrame != ~0ull && m_landLumaFrame + 30 >= m_frameNumber && m_landLuma > 0.0f;
        constexpr float kDayView = 0.92f;
        float meterTarget = 1.0f;
        r.metered = fresh;
        if (fresh) {
            r.view = m_viewLuma / m_landLuma;
            if (steady && h > 0.42f) {
                const float k = z.viewDay ? 1.0f - std::exp(-m_frameDt / 10.0f) : 1.0f;
                z.view += (r.view - z.view) * k;
                z.viewDay = true;
            }
            // (A zone not yet seen by day - a session started at dusk, or the zone first entered at night: a usual
            // day's view, 0.92 of the land in the demo's, until it is; never its night's own view, darker than its
            // land, every dawn read against it as too bright.) (Only down: a view darker than by day - the night's sky
            // darker than its land - lifted the nights lighter than the land's exposure set them.) The power is
            // RVK_Metering (0: off).
            r.viewRef = z.viewDay ? z.view : -1.0f;
            const float ref = z.viewDay ? z.view : kDayView;
            meterTarget = std::clamp(std::pow(std::max(r.view / std::max(ref, 1e-3f), 1e-3f), -m_metering), 0.4f, 1.0f);
        }
        r.meter = m_rigGainSet ? r.meter + (meterTarget - r.meter) * (1.0f - std::exp(-m_frameDt / 1.5f)) : meterTarget;
        m_rigGainSet = true;
    }
    // Exposed, in the zone's units.
    r.key = r.zoneSun * (sun + moon) * r.adapt;
    r.ambient = r.zoneSky * sky * r.adapt;
    const float keyUp = std::max(-r.keyDir[1], 0.0f), keyGround = r.key * keyUp;
    r.keyShare = keyGround + r.ambient > 0.0f ? keyGround / (keyGround + r.ambient) : 0.0f;
    r.sunGain = r.key / std::max(r.gameSun, 1e-3f);
    r.skyGain = r.ambient / std::max(r.gameAmbient, 1e-3f);
    if (m_frameNumber % 600 == 41)
        Log("light rig: sun up %.3f (%.1f deg); physical: sun %.3g sky %.3g moon %.3g, land %.3g lux -> adapt x%.3g "
            "(alpha %.2f); storm %.2f; zone %u (%s: sun %.2f sky %.2f); exposed: key %.3f (%s) ambient %.3f, key "
            "share %.2f; the game now: sun %.2f ambient %.2f -> gains sun %.2f sky %.2f; the ground's baked sky %.2f %.2f "
            "%.2f (%s); land exposure x%.2f (target %.2f of noon, the game's light %.2f of its noon); camera x%.2f "
            "(%s: the view %.2f x the land, by day %.2f)",
            double(h), double(std::asin(std::clamp(h, -1.0f, 1.0f)) * 57.29578f), double(r.sunN), double(r.sky),
            double(r.moonN), double(r.land), double(r.adapt), double(alpha), double(r.storm), r.zone,
            r.zoneMeasured ? "measured" : "default", double(r.zoneSun), double(r.zoneSky), double(r.key),
            r.keySun >= 0.5f ? "the sun" : "the moon", double(r.ambient), double(r.keyShare), double(r.gameSun),
            double(r.gameAmbient), double(r.sunGain), double(r.skyGain), double(r.groundSky[0]), double(r.groundSky[1]),
            double(r.groundSky[2]), r.groundKnown ? "its lightmap" : "unknown: the grass captures it",
            double(r.landGain), double(r.landTarget), double(r.landGame), double(r.meter),
            !r.metered ? "no measure yet" : r.viewRef > 0.0f ? "metered" : "metered against a usual day: the zone not seen by day",
            double(r.view), double(r.viewRef));
}

bool Device::RigGameLight(const d3d::Light& game, d3d::Light* out) const
{
    if (!m_rig.on || game.type != d3d::LIGHT_DIRECTIONAL)
        return false;
    const d3d::Vector& d = game.direction;
    float k;
    if (d.y > 0.0f) {
        k = 0.0f;                                // (from under the ground: the game's night - the rig's moon instead)
    } else {
        // (The sun's: along its way - not effects' coloured directional lights.)
        const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) *
                        std::sqrt(m_sunDir[0] * m_sunDir[0] + m_sunDir[1] * m_sunDir[1] + m_sunDir[2] * m_sunDir[2]);
        if (l <= 1e-6f || (d.x * m_sunDir[0] + d.y * m_sunDir[1] + d.z * m_sunDir[2]) / l <= 0.995f)
            return false;
        k = m_rig.sunFade;
    }
    *out = game;
    out->diffuse = {game.diffuse.r * k, game.diffuse.g * k, game.diffuse.b * k, game.diffuse.a};
    out->specular = {game.specular.r * k, game.specular.g * k, game.specular.b * k, game.specular.a};
    return true;
}

}  // namespace rvk
