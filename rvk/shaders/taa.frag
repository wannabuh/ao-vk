#version 450
// Temporal anti-aliasing (hdr.cpp), after the tone mapping: the scene is drawn with a different sub-pixel offset each
// frame; each pixel blends its new sample into its history - last frame's result where this surface was then (object
// motion vectors, or the camera's reprojection for the sky and anything without) - after clipping that history to
// the range of colours around the pixel now, so what moved or appeared doesn't leave ghosts.
layout(set = 0, binding = 0) uniform sampler2D current;    // tone mapped scene (jittered)
layout(set = 0, binding = 1) uniform sampler2D history;    // last frame's result
layout(set = 0, binding = 2) uniform sampler2D motion;     // screen motion since last frame, pixels (now - then)
layout(set = 0, binding = 3) uniform sampler2D depthTex;
layout(push_constant) uniform Push {
    mat4 reproject;     // this frame's clip -> last frame's clip (raw D3DMATRIX memory)
    vec4 size;          // width, height, history valid, object motion written
    vec4 params;        // new frame's weight, sharpening, unused, unused
} P;
layout(location = 0) out vec4 outColor;

vec3 ToYCoCg(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 FromYCoCg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// The history, Catmull-Rom filtered (9 taps through bilinear fetches): sharper than bilinear, which would blur
// the image a little more every frame.
vec3 SampleHistory(vec2 uv)
{
    vec2 size = P.size.xy, pos = uv * size, t1 = floor(pos - 0.5) + 0.5, f = pos - t1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2, t0 = (t1 - 1.0) / size, t3 = (t1 + 2.0) / size, t12 = (t1 + w2 / w12) / size;
    vec3 r = vec3(0.0);
    r += texture(history, vec2(t0.x, t0.y)).rgb * w0.x * w0.y;
    r += texture(history, vec2(t12.x, t0.y)).rgb * w12.x * w0.y;
    r += texture(history, vec2(t3.x, t0.y)).rgb * w3.x * w0.y;
    r += texture(history, vec2(t0.x, t12.y)).rgb * w0.x * w12.y;
    r += texture(history, vec2(t12.x, t12.y)).rgb * w12.x * w12.y;
    r += texture(history, vec2(t3.x, t12.y)).rgb * w3.x * w12.y;
    r += texture(history, vec2(t0.x, t3.y)).rgb * w0.x * w3.y;
    r += texture(history, vec2(t12.x, t3.y)).rgb * w12.x * w3.y;
    r += texture(history, vec2(t3.x, t3.y)).rgb * w3.x * w3.y;
    return max(r, vec3(0.0));
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy), size = ivec2(P.size.xy);
    vec3 c = texelFetch(current, p, 0).rgb;
    // The colours around this pixel now (YCoCg mean and spread), and the nearest surface among them, whose motion
    // the pixel takes - so edges move with the object in front.
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    float nearest = 2.0;
    ivec2 nearestPix = p;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            ivec2 q = clamp(p + ivec2(x, y), ivec2(0), size - 1);
            vec3 s = ToYCoCg(texelFetch(current, q, 0).rgb);
            m1 += s;
            m2 += s * s;
            float d = texelFetch(depthTex, q, 0).r;
            if (d < nearest) { nearest = d; nearestPix = q; }
        }
    vec3 mean = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    if (P.size.z < 0.5) { outColor = vec4(c, 1.0); return; }

    vec2 move = P.size.w > 0.5 ? texelFetch(motion, nearestPix, 0).xy : vec2(0.0);
    if (move == vec2(0.0)) {
        // No object motion: where the camera saw this point last frame.
        float d = min(texelFetch(depthTex, nearestPix, 0).r, 1.0);
        vec2 ndc = vec2((float(p.x) + 0.5) / P.size.x * 2.0 - 1.0, 1.0 - (float(p.y) + 0.5) / P.size.y * 2.0);
        vec4 prev = P.reproject * vec4(ndc, d, 1.0);
        vec2 prevNdc = prev.xy / prev.w;
        vec2 prevPix = vec2((prevNdc.x + 1.0) * 0.5 * P.size.x, (1.0 - prevNdc.y) * 0.5 * P.size.y);
        move = gl_FragCoord.xy - prevPix;
    }
    vec2 uv = (gl_FragCoord.xy - move) / P.size.xy;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) { outColor = vec4(c, 1.0); return; }

    // Clip the history towards the mean into the box of colours seen now.
    vec3 h = ToYCoCg(SampleHistory(uv));
    vec3 lo = mean - 1.25 * sigma, hi = mean + 1.25 * sigma;
    vec3 centre = 0.5 * (lo + hi), extent = max(0.5 * (hi - lo), vec3(1e-4));
    vec3 offset = h - centre, units = abs(offset / extent);
    float far = max(units.x, max(units.y, units.z));
    if (far > 1.0) h = centre + offset / far;
    h = FromYCoCg(h);
    // Blend, weighting by inverse brightness (a bright sample doesn't dominate: less flicker); a little more of the
    // new frame while moving fast, where the history is blurred by resampling.
    float weight = clamp(P.params.x + length(move) * 0.002, P.params.x, 0.4);
    float wc = weight / (1.0 + dot(c, vec3(0.3, 0.59, 0.11))), wh = (1.0 - weight) / (1.0 + dot(h, vec3(0.3, 0.59, 0.11)));
    outColor = vec4((c * wc + h * wh) / (wc + wh), 1.0);
}
