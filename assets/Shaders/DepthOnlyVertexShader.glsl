#version 450
// Vulkan variant of DepthOnlyVertexShader.glsl. localToScreen arrives pre-multiplied with the
// depth-range correction, so the value this writes is the one the D32_SFLOAT attachment stores and
// the one shadowLookupCorrection's reference is compared against.
//
// MAX_JOINTS, SET_DRAW and the ATTRIBUTE_* locations are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = ATTRIBUTE_POSITION) in vec3 vertexPositionModelSpace;
layout(location = ATTRIBUTE_JOINT) in vec4 vertexJointIndicies;
layout(location = ATTRIBUTE_WEIGHT) in vec4 vertexJointWeights;

layout(set = SET_DRAW, binding = 0) uniform DrawData {
    mat4 localToWorld;
    mat4 localToView;
    mat4 localToScreen; // clip-corrected for Vulkan depth 0..1 by the renderer
    mat4 normalMatrix;  // upper 3x3 meaningful
    ivec4 animated;     // x != 0 when skinned
    mat4 jointTransforms[MAX_JOINTS];
} draw;

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

    gl_Position = draw.localToScreen * boneTransform * vec4(vertexPositionModelSpace, 1.0);
}
