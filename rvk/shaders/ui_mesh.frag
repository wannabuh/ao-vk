#version 450
// The interface back end's meshes (ui_mesh.vert): the vertex colour times the texture, both premultiplied, over what
// is there (ONE, INV_SRC_ALPHA). Untextured meshes come with a white texture.
layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = inColor * texture(image, inUv);
}
