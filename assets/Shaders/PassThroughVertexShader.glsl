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
    // Declared past the shading fields for one reason: `rainBody`. The two body axes are the one
    // part of the rain model that must be resolved per *primitive* rather than per fragment, and
    // this is the only stage holding the matrix that resolves them. Everything between is here to
    // reach past, and is documented where it is read.
    vec4 fogDensity;
    vec4 fogScatter;
    vec4 fogAmbient;
    vec4 timeRain;
    vec4 rainWind;             // xyz the car's forward direction in world space
    vec4 wiperArcA;
    vec4 wiperArcB;
    vec4 wiperSweep;
    vec4 wiperTiming;
    vec4 wiperPane;
    vec4 rainBody;             // xyz the car's up direction in world space
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
// The vertex in the model's own coordinate system, for materials whose detail layers tile in space
// rather than in UV (Material.cppm, `DetailLayer`). Located past the light array by arithmetic
// rather than by a literal, so that changing MAX_LIGHTS cannot silently overlap it.
//
// The *unskinned* position, deliberately: a detail layer tiled on a skinned position would swim
// across the surface as the mesh deforms, where the point of tiling in space is that it does not
// move. It is also the position before any node transform, so a level that places or scales the
// model does not thereby change how coarse its detail looks.
//
// Fragment stages that do not blend never declare it. That is already the norm here — the whole of
// ColourFragmentShader declares none of these twelve — and is why an unread output is safe.
layout(location = 8 + MAX_LIGHTS) out vec3 positionInModelSpace;
// The car's own two axes, expressed in the model space of *this* primitive, and the primitive's own
// normal in that same space. Together with positionInModelSpace they are a complete, body-fixed,
// metric description of the surface, which is what the rain on a windscreen is built on.
//
// **They are resolved here because only this stage holds `localToWorld`.** A glTF node transform is
// baked into a per-mesh matrix rather than into the vertex data, so each mesh has a model space of
// its own and "which way is up in it" is a different answer per primitive — but it is the *same*
// answer for every vertex of one, and it does not change as the car drives. Resolving it per
// fragment would mean recovering a basis from screen-space derivatives, which is the mechanism that
// has produced four separate rain defects: the derivatives jitter, and anything that multiplies an
// accumulating displacement has to be exact rather than merely close.
//
// The inverse rotation is `transpose(mat3(localToWorld))`, which is the true inverse only up to a
// scale for a matrix that rotates and scales uniformly — the fragment stage normalises, so the
// scale falls out. A non-uniformly scaled node would skew these; no car body carries one.
layout(location = 9 + MAX_LIGHTS) out vec3 bodyUpInModelSpace;
layout(location = 10 + MAX_LIGHTS) out vec3 bodyForwardInModelSpace;
layout(location = 11 + MAX_LIGHTS) out vec3 normalsInModelSpace;
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
    positionInModelSpace = vertexPositionModelSpace;
    normalsInModelSpace = vertexNormalModelSpace;

    // A scene that has stated no car — every scene but the one with a windscreen in it — leaves
    // both axes zero, and a zero direction normalised in the fragment stage is a NaN that would
    // reach the shading of every pane. The world's own axes stand in, which is what a pane with no
    // car behind it should assume anyway.
    mat3 worldToModelRotation = transpose(mat3(draw.localToWorld));
    vec3 bodyUpWorld = dot(frame.rainBody.xyz, frame.rainBody.xyz) > 1e-8 ? frame.rainBody.xyz : vec3(0.0, 1.0, 0.0);
    vec3 bodyForwardWorld = dot(frame.rainWind.xyz, frame.rainWind.xyz) > 1e-8 ? frame.rainWind.xyz : vec3(0.0, 0.0, 1.0);
    bodyUpInModelSpace = worldToModelRotation * bodyUpWorld;
    bodyForwardInModelSpace = worldToModelRotation * bodyForwardWorld;
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
