#version 450
// Vulkan variant of PassThroughVertexShader.glsl. Y-flip is handled by the renderer's
// negative viewport; localToScreen arrives pre-multiplied with the depth-range correction.
// MAX_JOINTS, MAX_LIGHTS, SET_* and the ATTRIBUTE_* locations are defined by the renderer
// from Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = ATTRIBUTE_POSITION) in vec3 vertexPositionModelSpace;
layout(location = ATTRIBUTE_TEXCOORD) in vec2 vertexTextureCoordinates;
layout(location = ATTRIBUTE_NORMAL) in vec3 vertexNormalModelSpace;
layout(location = ATTRIBUTE_TANGENT) in vec4 vertexTangentModelSpace;
layout(location = ATTRIBUTE_JOINT) in vec4 vertexJointIndicies;
layout(location = ATTRIBUTE_WEIGHT) in vec4 vertexJointWeights;

// Set 0: per camera pass. Set 1: per material. Set 2: per draw (dynamic offset).
struct Light {
    vec4 position;             // xyz position
    vec4 diffuse;
    vec4 specular;
    vec4 ambientAttenuation;   // xyz ambient, w attenuation
};

layout(set = SET_FRAME, binding = 0) uniform FrameData {
    mat4 viewMatrix;
    vec4 cameraPosition;
    ivec4 lightCount;          // x = lights in use, never above MAX_LIGHTS
    Light lights[MAX_LIGHTS];
} frame;

layout(set = SET_DRAW, binding = 0) uniform DrawData {
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
// One direction per declared light, locations 8..8+MAX_LIGHTS-1; elements at or past
// lightCount are never read.
layout(location = 8) out vec3 lightDirectionWorldSpace[MAX_LIGHTS];
// tangentBinormalNormalMatrix stays local: no fragment stage reads it, and an output no
// fragment shader consumes is a stage-interface mismatch once SPIR-V is optimized.

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

    mat3 tangentBinormalNormalMatrix = mat3(
        tangentInNormalSpace.x, bitangentInNormalSpace.x, normalsInNormalSpace.x,
        tangentInNormalSpace.y, bitangentInNormalSpace.y, normalsInNormalSpace.y,
        tangentInNormalSpace.z, bitangentInNormalSpace.z, normalsInNormalSpace.z
    );

    for (int lightIndex = 0; lightIndex < frame.lightCount.x; lightIndex++) {
        lightDirectionWorldSpace[lightIndex] = tangentBinormalNormalMatrix * (modelView3x3Matrix * frame.lights[lightIndex].position.xyz);
    }

    viewDirectionWorldSpace = tangentBinormalNormalMatrix * -positionInViewSpace;

    gl_Position = draw.localToScreen * boneTransform * vec4(vertexPositionModelSpace, 1.0);
}
