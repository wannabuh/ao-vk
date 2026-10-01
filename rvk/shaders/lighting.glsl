// Direct3D 7 lighting equation, shared by the per-vertex (ffp.vert) and per-pixel (ffp.frag) paths.
// Requires constants.glsl. Accumulates the light terms; the caller combines them with the material colours.
void AccumulateLight(Light l, vec3 posW, vec3 normalW, vec3 toEye, float sunScale, inout vec3 ambient, inout vec3 diff,
                     inout vec3 spec)
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
        att = denom > 0.0 ? 1.0 / denom : 1.0;
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
    diff += att * nl * l.diffuse.rgb;
    if ((C.flags.x & F_SPECULAR) != 0u && nl > 0.0) {
        float nh = max(dot(normalW, normalize(L + toEye)), 0.0);
        spec += att * pow(nh, C.misc.x) * l.specular.rgb;
    }
}

void AccumulateLights(vec3 posW, vec3 normalW, float sunScale, inout vec3 ambient, inout vec3 diff, inout vec3 spec)
{
    vec3 toEye = (C.flags.x & F_LOCALVIEWER) != 0u ? normalize(C.eyePos.xyz - posW) : -C.eyeDir.xyz;
    for (uint i = 0u; i < C.lightInfo.x; ++i)
        AccumulateLight(C.lights[i], posW, normalW, toEye, sunScale, ambient, diff, spec);
    if ((C.flags.x & F_LIGHTOVERRIDE) != 0u)
        for (uint i = 0u; i < FL.info.x; ++i)
            AccumulateLight(FL.lights[i], posW, normalW, toEye, sunScale, ambient, diff, spec);
}
