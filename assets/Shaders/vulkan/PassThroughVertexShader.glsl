#version 450
// Vulkan variant of PassThroughVertexShader.glsl. Y-flip is handled by the renderer's
// negative viewport; localToScreen arrives pre-multiplied with the depth-range correction.

layout(location = 0) in vec3 vertexPositionModelSpace;
layout(location = 1) in vec2 vertexTextureCoordinates;
layout(location = 2) in vec3 vertexNormalModelSpace;
layout(location = 3) in vec4 vertexTangentModelSpace;
layout(location = 4) in vec4 vertexJointIndicies;
layout(location = 5) in vec4 vertexJointWeights;

// Set 0: per camera pass. Set 1: per material. Set 2: per draw (dynamic offset).
layout(set = 0, binding = 0) uniform FrameData {
    mat4 viewMatrix;
    vec4 cameraPosition;
    vec4 lightPosition;
    vec4 lightDiffuse;
    vec4 lightSpecular;
    vec4 lightAmbientAttenuation; // xyz ambient, w attenuation
} frame;
#define MAX_JOINTS 128
layout(set = 2, binding = 0) uniform DrawData {
    mat4 localToWorld;
    mat4 localToView;
    mat4 localToScreen; // clip-corrected for Vulkan depth 0..1 by the renderer
    mat4 normalMatrix;  // upper 3x3 meaningful
    ivec4 animated;     // x != 0 when skinned
    mat4 jointTransforms[MAX_JOINTS];
} draw;

layout(location = 0) out vec2 textureCoordinates;
layout(location = 1) out vec3 positionInWorldSpace;
layout(location = 2) out vec3 positionInViewSpace;
layout(location = 3) out vec3 normalsInNormalSpace;
layout(location = 4) out vec3 tangentInNormalSpace;
layout(location = 5) out vec3 bitangentInNormalSpace;
layout(location = 6) out vec3 normalsInWorldSpace;
layout(location = 7) out vec3 viewDirectionWorldSpace;
layout(location = 8) out vec3 lightDirectionWorldSpace;
layout(location = 9) out mat3 tangentBinormalNormalMatrix;

void main()
{
    mat4 boneTransform = mat4(1.0f);
    vec4 jointWeights = vertexJointWeights;

    if (draw.animated.x != 0) {
        boneTransform = jointWeights.x * draw.jointTransforms[int(vertexJointIndicies.x)] +
        jointWeights.y * draw.jointTransforms[int(vertexJointIndicies.y)] +
        jointWeights.z * draw.jointTransforms[int(vertexJointIndicies.z)] +
        jointWeights.w * draw.jointTransforms[int(vertexJointIndicies.w)];
    }

    mat3 normalMatrix3 = mat3(draw.normalMatrix);
    mat3 modelView3x3Matrix = mat3(frame.viewMatrix);

    textureCoordinates = vertexTextureCoordinates;
    positionInWorldSpace = vec3(draw.localToWorld * boneTransform * vec4(vertexPositionModelSpace, 1.0));
    positionInViewSpace = vec3(draw.localToView * boneTransform * vec4(vertexPositionModelSpace, 1.0));

    vec3 bitangent = cross(vertexNormalModelSpace, vertexTangentModelSpace.xyz) * vertexTangentModelSpace.w;

    normalsInWorldSpace = normalize(draw.localToWorld * boneTransform * vec4(vertexNormalModelSpace.xyz, 0.0)).xyz;
    normalsInNormalSpace = normalize(normalMatrix3 * mat3(boneTransform) * vertexNormalModelSpace);
    tangentInNormalSpace = normalize(normalMatrix3 * mat3(boneTransform) * vec3(vertexTangentModelSpace));
    bitangentInNormalSpace = normalize(normalMatrix3 * mat3(boneTransform) * bitangent);

    tangentBinormalNormalMatrix = mat3(
        tangentInNormalSpace.x, bitangentInNormalSpace.x, normalsInNormalSpace.x,
        tangentInNormalSpace.y, bitangentInNormalSpace.y, normalsInNormalSpace.y,
        tangentInNormalSpace.z, bitangentInNormalSpace.z, normalsInNormalSpace.z
    );

    lightDirectionWorldSpace = tangentBinormalNormalMatrix * (modelView3x3Matrix * frame.lightPosition.xyz);
    viewDirectionWorldSpace = tangentBinormalNormalMatrix * -positionInViewSpace;

    gl_Position = draw.localToScreen * boneTransform * vec4(vertexPositionModelSpace, 1.0);
}
