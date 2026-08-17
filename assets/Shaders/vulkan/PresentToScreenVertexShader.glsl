#version 450
// Vulkan fullscreen pass: a single oversized triangle from gl_VertexIndex; no vertex buffers.

layout(location = 0) out vec2 textureCoordinates;

void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    textureCoordinates = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
