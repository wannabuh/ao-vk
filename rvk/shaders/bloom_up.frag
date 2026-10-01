#version 450
// Bloom (hdr.cpp), upsampling: a 3x3 tent over the smaller level, added (blending ONE/ONE) to this level's
// downsampled light.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push {
    vec4 params;        // source texel size xy
} P;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 ts = P.params.xy, uv = gl_FragCoord.xy * 0.5 * ts;  // this pixel in the (2x smaller) source's coordinates
    vec3 s = texture(src, uv).rgb * 4.0;
    s += (texture(src, uv + ts * vec2(-1, 0)).rgb + texture(src, uv + ts * vec2(1, 0)).rgb +
          texture(src, uv + ts * vec2(0, -1)).rgb + texture(src, uv + ts * vec2(0, 1)).rgb) * 2.0;
    s += texture(src, uv + ts * vec2(-1, -1)).rgb + texture(src, uv + ts * vec2(1, -1)).rgb +
         texture(src, uv + ts * vec2(-1, 1)).rgb + texture(src, uv + ts * vec2(1, 1)).rgb;
    outColor = vec4(s / 16.0, 1.0);
}
