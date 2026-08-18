#version 450
// Vulkan variant of SkyboxVertexShader.glsl.

// Position only: the renderer feeds exactly the locations this shader declares, so
// declaring the unread attributes would cost a vertex binding per primitive for nothing.
layout(location = ATTRIBUTE_POSITION) in vec3 vertexPositionModelSpace;

// Declared so the set-0 block matches the ABI even though nothing here reads it.
struct Light {
    vec4 position;
    vec4 diffuse;
    vec4 specular;
    vec4 ambientAttenuation;
};

layout(set = SET_FRAME, binding = 0) uniform FrameData {
    mat4 viewMatrix;
    vec4 cameraPosition;
    ivec4 lightCount;
    Light lights[MAX_LIGHTS];
} frame;

layout(set = SET_DRAW, binding = 0) uniform DrawData {
    mat4 localToWorld;
    mat4 localToView;
    mat4 localToScreen;
    mat4 normalMatrix;
    ivec4 animated;
    mat4 jointTransforms[MAX_JOINTS];
} draw;

layout(location = 0) out vec3 textureCoordinates;

void main()
{
    textureCoordinates = vertexPositionModelSpace;
    gl_Position = draw.localToScreen * vec4(vertexPositionModelSpace, 1.0);
}
