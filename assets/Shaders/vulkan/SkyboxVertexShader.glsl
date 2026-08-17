#version 450
// Vulkan variant of SkyboxVertexShader.glsl.

layout(location = 0) in vec3 vertexPositionModelSpace;
layout(location = 1) in vec2 vertexTextureCoordinates;
layout(location = 2) in vec3 vertexNormalModelSpace;
layout(location = 3) in vec4 vertexTangentModelSpace;

layout(set = 0, binding = 0) uniform FrameData {
    mat4 viewMatrix;
    vec4 cameraPosition;
    vec4 lightPosition;
    vec4 lightDiffuse;
    vec4 lightSpecular;
    vec4 lightAmbientAttenuation;
} frame;

#define MAX_JOINTS 128
layout(set = 2, binding = 0) uniform DrawData {
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
