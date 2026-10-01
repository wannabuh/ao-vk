#version 450
// Camera motion blur (hdr.cpp), on the tone mapped scene before the interface: each pixel's motion since the last
// frame from its depth and the two frames' cameras, scaled to a fixed exposure time (the same blur at any frame
// rate). Pixels near the camera - the player's character, which the camera follows - aren't blurred, and aren't
// smeared over the background either.
layout(set = 0, binding = 0) uniform sampler2D colour;
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(push_constant) uniform Push {
    mat4 reproject;     // current clip -> previous clip (raw D3D matrix: GLSL M * v == D3D v * M)
    vec4 params;        // motion scale (exposure / frame time), max length (pixels), focus near, focus far
    vec4 proj;          // D3D projection m[2][2], m[3][2]; target width, height
} P;
layout(location = 0) out vec4 outColor;

float ViewZ(float d) { return d >= 1.0 ? 1e6 : P.proj.y / (d - P.proj.x); }

void main()
{
    ivec2 pix = ivec2(gl_FragCoord.xy);
    vec2 size = P.proj.zw;
    float d = texelFetch(depthTex, pix, 0).r;
    vec2 ndc = vec2(gl_FragCoord.x / size.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / size.y * 2.0);
    vec4 prev = P.reproject * vec4(ndc, d, 1.0);
    vec4 centre = texelFetch(colour, pix, 0);
    if (prev.w <= 1e-5) { outColor = centre; return; }
    vec2 motion = (ndc - prev.xy / prev.w) * 0.5 * size * vec2(1.0, -1.0) * P.params.x;   // pixels
    float z = ViewZ(d);
    motion *= smoothstep(P.params.z, P.params.w, z);               // near the camera: no blur
    float len = length(motion);
    if (len > P.params.y) motion *= P.params.y / len;
    if (len < 0.5) { outColor = centre; return; }

    const int kSamples = 12;
    vec3 sum = centre.rgb;
    float weight = 1.0;
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    for (int i = 0; i < kSamples; ++i) {
        float t = (float(i) + jitter) / float(kSamples) - 0.5;
        ivec2 q = clamp(ivec2(gl_FragCoord.xy + motion * t), ivec2(0), ivec2(size) - 1);
        if (ViewZ(texelFetch(depthTex, q, 0).r) < P.params.z) continue;   // the near character: not smeared out
        sum += texelFetch(colour, q, 0).rgb;
        weight += 1.0;
    }
    outColor = vec4(sum / weight, centre.a);
}
