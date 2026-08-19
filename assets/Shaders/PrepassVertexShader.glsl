#version 450
// The occlusion prepass: the view drawn once for its shape rather than its colour. What comes out
// is a surface normal and a view depth per pixel, which is the geometry the gather searches.
//
// A pair of its own rather than the shading pair for the reason the depth-only pair is one: this
// stage writes two varyings where PassThroughVertexShader writes ten, and a fragment stage that
// declared a subset of those would leave the vertex stage writing outputs nothing consumes — which
// is what validation reports on every pipeline built from a mismatched pair.
//
// MAX_JOINTS, SET_DRAW and the ATTRIBUTE_* locations are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = ATTRIBUTE_POSITION) in vec3 vertexPositionModelSpace;
layout(location = ATTRIBUTE_NORMAL) in vec3 vertexNormalModelSpace;
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

layout(location = 0) out vec3 normalInViewSpace;
layout(location = 1) out vec3 positionInViewSpace;

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

    // `normalMatrix` is already the inverse transpose of localToView — the *view*-space normal
    // matrix, which is what PassThroughVertexShader builds its tangent basis from too. It is the one
    // field of DrawData whose name does not say which space it lands in, and rotating its result by
    // the view matrix a second time is a wrong normal that still looks like a normal: the gather
    // then reads every surface as tilted, and answers with large smooth patches of shading that
    // follow the mesh rather than the light.
    normalInViewSpace = mat3(draw.normalMatrix) * mat3(boneTransform) * vertexNormalModelSpace;
    positionInViewSpace = vec3(draw.localToView * boneTransform * vec4(vertexPositionModelSpace, 1.0));

    gl_Position = draw.localToScreen * boneTransform * vec4(vertexPositionModelSpace, 1.0);
}
