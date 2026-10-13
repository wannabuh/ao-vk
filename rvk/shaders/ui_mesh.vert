#version 450
// The interface back end's meshes (interface.cpp, docs/ui-dll.md phase 9): RmlUi's triangles - positions in pixels,
// premultiplied colours, texture coordinates - moved by the mesh's translation onto the target.
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(push_constant) uniform Push {
    vec4 target;        // 2 / width, 2 / height, translation x, y (pixels)
    vec4 params;        // depth, opacity
} P;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUv;

void main()
{
    vec2 p = (inPosition + P.target.zw) * P.target.xy - 1.0;
    gl_Position = vec4(p, P.params.x, 1.0);
    outColor = inColor * P.params.y;
    outUv = inUv;
}
