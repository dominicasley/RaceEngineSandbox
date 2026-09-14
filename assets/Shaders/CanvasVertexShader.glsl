#version 450

// The canvas pass's vertex stage (IFrameRecorder::recordCanvas, docs/instrument-cluster-brief.md §6):
// no vertex input at all. Each instance is one quad of the frame's ring — position and size in canvas
// pixels, y down from the top-left; the image rectangle it samples; a tint — and the six vertices of
// the instance are the quad's two triangles, picked by gl_VertexIndex. A positive viewport puts
// canvas row 0 at the top of the target, which is where an uploaded texture's row 0 is, so the
// canvas samples the right way up through a surface's own UVs.

struct Quad
{
    vec2 position;
    vec2 size;
    vec2 uvMin;
    vec2 uvMax;
    vec4 colour;
};

layout(std430, set = SET_CANVAS_QUADS, binding = CANVAS_QUAD_BINDING) readonly buffer Quads
{
    Quad quads[];
};

layout(push_constant) uniform Canvas
{
    vec2 canvasSize;
    vec2 unitRange;
    uint firstQuad;
    uint kind;
    uint encode;
    uint reserved;
} canvas;

layout(location = 0) out vec2 imageCoordinates;
layout(location = 1) out vec4 tint;

void main()
{
    Quad quad = quads[canvas.firstQuad + gl_InstanceIndex];
    const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                                    vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
    vec2 corner = corners[gl_VertexIndex];
    vec2 pixel = quad.position + corner * quad.size;
    gl_Position = vec4(pixel / canvas.canvasSize * 2.0 - 1.0, 0.0, 1.0);
    imageCoordinates = mix(quad.uvMin, quad.uvMax, corner);
    tint = quad.colour;
}
