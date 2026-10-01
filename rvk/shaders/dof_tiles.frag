#version 450
// Depth of field: the largest circle of confusion in each 16x16 half-resolution tile (every other texel).
layout(set = 0, binding = 0) uniform sampler2D halfTex;
layout(location = 0) out vec4 outTile;

void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * 16, size = textureSize(halfTex, 0) - 1;
    float largest = 0.0;
    for (int y = 0; y < 16; y += 2)
        for (int x = 0; x < 16; x += 2)
            largest = max(largest, abs(texelFetch(halfTex, min(base + ivec2(x, y), size), 0).a));
    outTile = vec4(largest, 0.0, 0.0, 1.0);
}
