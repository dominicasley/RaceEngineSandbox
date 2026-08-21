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

// Declared for the same reason the shadow tail is: this stage reads none of it, and the block has
// one std140 layout that both stages have to agree on. Read where it is used (PbrFragmentShader).
struct Probe {
    vec4 irradiance[SH_COEFFICIENTS];
    vec4 boxMin;
    vec4 boxMax;
    vec4 position;
};

// Declared whole even though this stage reads only the first three members: it is one block with
// one std140 layout, and a stage that declared a prefix of it would be a second statement of the
// ABI to keep in step. The shadow tail is documented where it is read (PbrFragmentShader).
layout(set = SET_FRAME, binding = 0) uniform FrameData {
    mat4 viewMatrix;
    vec4 cameraPosition;
    ivec4 lightCount;          // x = lights in use, never above MAX_LIGHTS
    Light lights[MAX_LIGHTS];
    mat4 shadowMatrices[SHADOW_CASCADES];
    vec4 shadowSplits;
    vec4 shadowTexelWorldSize;
    vec4 shadowDepthScale;
    ivec4 shadowParams;
    ivec4 probeParams;
    Probe probes[MAX_IBL_PROBES];
} frame;

layout(set = SET_DRAW, binding = 0) uniform DrawData {
    mat4 localToWorld;
    mat4 localToView;
    mat4 localToScreen; // clip-corrected for Vulkan depth 0..1 by the renderer
    mat4 normalMatrix;  // upper 3x3 meaningful
    ivec4 animated;     // x != 0 when skinned
} draw;

// The skinning palette, at a binding of its own on a ring of its own: it is MAX_JOINTS mat4s
// against the 272 bytes above, and only a draw with animated.x != 0 has one. Unskinned draws bind
// its zeroed first slot, which nothing here reads.
layout(set = SET_DRAW, binding = JOINT_DATA_BINDING) uniform JointData {
    mat4 jointTransforms[MAX_JOINTS];
} skin;

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
        boneTransform = jointWeights.x * skin.jointTransforms[int(vertexJointIndicies.x)] +
        jointWeights.y * skin.jointTransforms[int(vertexJointIndicies.y)] +
        jointWeights.z * skin.jointTransforms[int(vertexJointIndicies.z)] +
        jointWeights.w * skin.jointTransforms[int(vertexJointIndicies.w)];
    }

    mat3 normalMatrix3 = mat3(draw.normalMatrix);
    mat3 modelView3x3Matrix = mat3(frame.viewMatrix);

    textureCoordinates = vertexTextureCoordinates;
    positionInWorldSpace = vec3(draw.localToWorld * boneTransform * vec4(vertexPositionModelSpace, 1.0));
    positionInViewSpace = vec3(draw.localToView * boneTransform * vec4(vertexPositionModelSpace, 1.0));

    vec3 bitangent = cross(vertexNormalModelSpace, vertexTangentModelSpace.xyz) * vertexTangentModelSpace.w;

    normalsInWorldSpace = normalize(draw.localToWorld * boneTransform * vec4(vertexNormalModelSpace.xyz, 0.0)).xyz;
    normalsInNormalSpace = normalize(normalMatrix3 * mat3(boneTransform) * vertexNormalModelSpace);

    // Guarded, because a primitive with no TANGENT attribute reads the pipeline's dummy — a zero
    // vector — and normalize(0) is NaN on this driver. The NaN rides the varying into the fragment
    // stage where 0 * NaN is still NaN, so the whole tangent frame poisons the shading normal even
    // though the normal map's x and y are zero: every direct term dies (a sunlit road rendering
    // near-black) and the specular direction goes undefined per pixel, which is visible as
    // lighting flicker in motion. The fallback is a *synthesised* frame about the normal, not a
    // zero one: the whole of the direct term is computed in this frame, and a degenerate frame
    // collapses the light direction onto its normal axis — a cosine of one at every sun angle. Any
    // in-plane orientation does, because the only normal map that reaches it is flat.
    vec3 tangentRotated = normalMatrix3 * mat3(boneTransform) * vec3(vertexTangentModelSpace);
    vec3 bitangentRotated = normalMatrix3 * mat3(boneTransform) * bitangent;
    if (dot(tangentRotated, tangentRotated) < 1e-12) {
        vec3 helper = abs(normalsInNormalSpace.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        tangentRotated = cross(helper, normalsInNormalSpace);
        bitangentRotated = cross(normalsInNormalSpace, tangentRotated);
    }
    tangentInNormalSpace = normalize(tangentRotated);
    bitangentInNormalSpace = normalize(bitangentRotated);

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
