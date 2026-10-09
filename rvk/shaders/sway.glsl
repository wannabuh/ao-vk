// Plants swaying in the wind (an enhancement), shared by the scene (ffp.vert) and the shadow passes (shadow.vert) so
// a plant and its shadow move together. Needs the plant's texture 0 as `swayTex`.

// The texture's mean alpha: its smallest mip, or without mips a grid of taps. Low for leaves and grass, 1 for walls.
float SwayMeanAlpha()
{
    int levels = textureQueryLevels(swayTex);
    if (levels > 4) return textureLod(swayTex, vec2(0.5), float(levels - 1)).a;
    float sum = 0.0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            sum += textureLod(swayTex, (vec2(x, y) + 0.5) / 4.0, 0.0).a;
    return sum / 16.0;
}

// How much a texture's holes let it sway (1 = fully): leaves and grass are mostly holes (mean alpha ~0.1 - 0.45 in a
// survey of the game's plants); signs, lamps and trims with a cut-out edge are mostly not (0.6 and up).
float SwayHoles(float meanAlpha) { return smoothstep(0.8, 0.6, meanAlpha); }

// How high up the plant a vertex is (0 at the base, 1 at the top), along the model axis that points up in the world
// (sway.w - 1: not always y; sway.y is negative when the axis points down).
float SwayHeight(vec3 modelPos, vec4 sway)
{
    int axis = clamp(int(sway.w + 0.5) - 1, 0, 2);
    return clamp((modelPos[axis] - sway.x) * sway.y, 0.0, 1.0);
}

// How far (world units, along the wind) a vertex moves: the plant bends with the square of the height above its base
// - two waves and a slow gust, their phase travelling across the world (from the object's position and the vertex's)
// so neighbouring plants move a little apart. sway: model base along the up axis, 1 / model height (signed), tip
// sway, 1 + up axis (0 = off).
// ... with the texture's holes factor already known (SwayHoles(SwayMeanAlpha())).
float SwayDistanceHoles(vec3 modelPos, vec4 sway, vec2 originXZ, float time, float holes)
{
    float h = SwayHeight(modelPos, sway);
    if (h <= 0.0 || holes <= 0.0) return 0.0;
    float phase = dot(originXZ + modelPos.xz, vec2(0.31, 0.23));
    float wave = 0.6 * sin(time * 1.9 + phase) + 0.25 * sin(time * 3.7 + phase * 1.7) +
                 0.35 * (0.5 + 0.5 * sin(time * 0.37 + phase * 0.1));
    return wave * sway.z * h * h * holes;
}

float SwayDistance(vec3 modelPos, vec4 sway, vec2 originXZ, float time)
{
    return SwayHeight(modelPos, sway) > 0.0
               ? SwayDistanceHoles(modelPos, sway, originXZ, time, SwayHoles(SwayMeanAlpha())) : 0.0;
}
