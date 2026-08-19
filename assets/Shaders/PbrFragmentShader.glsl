#version 450
// MAX_LIGHTS, SET_* and the TEXTURE_* bindings are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = 0) out vec4 fragColor;

layout(location = 0) in vec2 textureCoordinates;
layout(location = 1) in vec3 positionInWorldSpace;
layout(location = 2) in vec3 positionInViewSpace;
layout(location = 3) in vec3 normalsInNormalSpace;
layout(location = 4) in vec3 tangentInNormalSpace;
layout(location = 5) in vec3 bitangentInNormalSpace;
layout(location = 6) in vec3 normalsInWorldSpace;
layout(location = 7) in vec3 viewDirectionWorldSpace;
layout(location = 8) in vec3 lightDirectionWorldSpace[MAX_LIGHTS];

// Set 0: per camera pass. Set 1: per material. Set 2: per draw (dynamic offset).
struct Light {
    vec4 position;             // xyz position
    vec4 diffuse;
    vec4 specular;
    vec4 ambientAttenuation;   // xyz ambient, w attenuation (ambient is no longer read; see ads)
};

// One light probe. `irradiance` is already convolved with the clamped-cosine lobe and divided by
// pi by the engine's projection (Graphics/Api/SphericalHarmonics.cppm), so evaluating the basis
// gives outgoing diffuse radiance for unit albedo and this shader multiplies by albedo alone.
struct Probe {
    vec4 irradiance[SH_COEFFICIENTS];
    vec4 boxMin;               // xyz world minimum of the influence box, w the blend band's width
    vec4 boxMax;               // xyz world maximum, w non-zero for the scene's global probe
    vec4 position;             // xyz where it was captured, w its slice of probeSpecular
};

layout(set = SET_FRAME, binding = 0) uniform FrameData {
    mat4 viewMatrix;
    vec4 cameraPosition;
    ivec4 lightCount;              // x = lights in use, never above MAX_LIGHTS
    Light lights[MAX_LIGHTS];
    // The cascaded shadow map. shadowMatrices take world space straight to a shadow-map lookup —
    // the depth convention and the negative-viewport y flip are folded in by the renderer's
    // shadowLookupCorrection — so what follows is the same arithmetic as the GL dialect.
    mat4 shadowMatrices[SHADOW_CASCADES];
    vec4 shadowSplits;             // where each cascade ends, along the view axis
    vec4 shadowTexelWorldSize;     // world units one texel of each cascade covers
    vec4 shadowDepthScale;         // normalised depth per world unit along the light, per cascade
    ivec4 shadowParams;            // x = cascades in use (0 = shade lit), y = the light they follow,
                                   // z = non-zero in a light probe capture (read by the sky, not here)
    // The image-based lighting graph. Each probe is an environment recorded from one point, the
    // box that recording is a good enough answer for, and the band inside that box over which it
    // hands over to whatever else covers the same space.
    ivec4 probeParams;             // x = probes in use (0 = no image-based lighting at all)
    Probe probes[MAX_IBL_PROBES];
} frame;

// Every probe's prefiltered radiance, as one cube array: layers 6i..6i+5 are probe i, and the mip
// chain is the roughness axis. One image rather than one per probe is what lets the loop below
// pick a probe with a dynamic index.
layout(set = SET_FRAME, binding = PROBE_SPECULAR_BINDING) uniform samplerCubeArray probeSpecular;

// textureTransform is a 3x3 UV transform in a mat4 slot: std140 pads a mat3's columns to 16
// bytes each, which the C++ glm::mat3 does not, so the ABI carries it as a mat4.
layout(set = SET_MATERIAL, binding = 0) uniform MaterialData {
    vec4 baseColour;
    vec4 roughMetal;       // x roughness, y metalness
    ivec4 useTextures;     // x diffuse, y normal, z specular, w emissive
    ivec4 useTextures2;    // x occlusion
    mat4 textureTransform; // KHR_texture_transform, upper 3x3
} material;
layout(set = SET_MATERIAL, binding = TEXTURE_DIFFUSE) uniform sampler2D diffuseTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_NORMAL) uniform sampler2D normalTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_SPECULAR) uniform sampler2D specularTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_EMISSIVE) uniform sampler2D emissiveTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_OCCLUSION) uniform sampler2D occlusionTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_ENVIRONMENT) uniform samplerCube environmentMap;

// Set 3: one comparison sampler per cascade. A set of its own because the cascades are per frame,
// not per material. The pipeline uses these statically, so a view with no cascades still binds a
// set — the renderer's fallback — and frame.shadowParams.x is what says not to read it.
layout(set = SET_SHADOW, binding = SHADOW_MAP_BINDING) uniform sampler2DShadow shadowMaps[SHADOW_CASCADES];

// Beside them, and for the same reason: the occlusion this view gathered from its own geometry
// before it shaded, one visibility term per pixel of the screen. A view that gathers none binds a
// 1x1 white image, so this is read unconditionally and reads as one — nothing is in the way.
layout(set = SET_SHADOW, binding = AO_MAP_BINDING) uniform sampler2D ambientOcclusionMap;

const float M_PI = 3.141592653;

// One cascade's percentage-closer average.
float shadowInCascade(int cascade, vec3 coordinate)
{
    float result = 1.0;

    // An array of samplers may only be indexed by a dynamically uniform expression, and a cascade
    // chosen from a fragment's own view depth is not one — fragments of a single triangle straddle
    // a split. The loop counter is dynamically uniform because its bounds are compile-time
    // constants, so this unrolls into a chain of constant-index branches: the form that is defined
    // on Vulkan without shaderSampledImageArrayNonUniformIndexing and on desktop GL without
    // ARB_gpu_shader5, neither of which this engine asks for.
    for (int index = 0; index < SHADOW_CASCADES; index++)
    {
        if (index != cascade)
        {
            continue;
        }

        // Each tap is already a 2x2 percentage-closer average, because the comparison sampler
        // filters linearly; a (2r+1)^2 grid one texel apart therefore covers a (2r+2)^2
        // neighbourhood with tent weighting.
        float texel = 1.0 / float(textureSize(shadowMaps[index], 0).x);
        float total = 0.0;

        for (int y = -SHADOW_PCF_RADIUS; y <= SHADOW_PCF_RADIUS; y++)
        {
            for (int x = -SHADOW_PCF_RADIUS; x <= SHADOW_PCF_RADIUS; x++)
            {
                total += texture(shadowMaps[index], vec3(coordinate.xy + vec2(x, y) * texel, coordinate.z));
            }
        }

        result = total / float((2 * SHADOW_PCF_RADIUS + 1) * (2 * SHADOW_PCF_RADIUS + 1));
    }

    return result;
}

// How much of the shadow-casting light reaches this fragment, according to one cascade.
float shadowSample(int cascade, vec3 worldPosition, vec3 worldNormal, float NdL)
{
    float texelWorld = frame.shadowTexelWorldSize[cascade];
    float sinTheta = sqrt(max(0.0, 1.0 - NdL * NdL));
    float tanTheta = min(sinTheta / max(NdL, 1.0 / float(SHADOW_MAX_SLOPE)), float(SHADOW_MAX_SLOPE));

    // Normal offset first. Moving the sample point off the surface sideways is what clears acne on
    // a slope without detaching the contact shadow, which a depth bias large enough to do the same
    // job alone would.
    vec4 lightSpace = frame.shadowMatrices[cascade]
        * vec4(worldPosition + worldNormal * (texelWorld * float(SHADOW_NORMAL_OFFSET_TEXELS) * sinTheta), 1.0);
    vec3 coordinate = lightSpace.xyz / lightSpace.w;

    // Past the cascade's far plane nothing was stored to compare against, and the comparison would
    // read "occluded" for every fragment behind the map. Outside it laterally needs no test: the
    // sampler clamps to an opaque white border, which compares as lit.
    if (coordinate.z >= 1.0)
    {
        return 1.0;
    }

    // What the offset does not cover: the depth a surface at this slope crosses over the texels the
    // filter spans, expressed in the cascade's own normalised depth.
    coordinate.z -= texelWorld * (float(SHADOW_CONSTANT_BIAS_TEXELS) + float(SHADOW_SLOPE_BIAS_TEXELS) * tanTheta)
        * frame.shadowDepthScale[cascade];

    return shadowInCascade(cascade, coordinate);
}

float shadowFactor(vec3 worldPosition, vec3 worldNormal, vec3 lightDirection, float viewDepth)
{
    if (frame.shadowParams.x <= 0)
    {
        return 1.0;
    }

    float NdL = dot(worldNormal, lightDirection);
    if (NdL <= 0.0)
    {
        // Facing away from the light. The diffuse term is already zero here, and testing would
        // sample the far side of this very surface and shadow it a second time.
        return 1.0;
    }

    float lastSplit = frame.shadowSplits[frame.shadowParams.x - 1];
    if (viewDepth >= lastSplit)
    {
        return 1.0;
    }

    int cascade = frame.shadowParams.x - 1;
    for (int index = 0; index < SHADOW_CASCADES; index++)
    {
        if (index < frame.shadowParams.x && viewDepth < frame.shadowSplits[index])
        {
            cascade = index;
            break;
        }
    }

    float shadow = shadowSample(cascade, worldPosition, worldNormal, NdL);

    // The seam. Filter width and bias both change at a split, which reads as a line ruled across
    // the ground; the last SHADOW_BLEND_PERCENT of a cascade cross-fades into the next.
    float split = frame.shadowSplits[cascade];
    float blendStart = split * (1.0 - float(SHADOW_BLEND_PERCENT) / 100.0);
    if (cascade + 1 < frame.shadowParams.x && viewDepth > blendStart)
    {
        float blend = clamp((viewDepth - blendStart) / max(split - blendStart, 0.0001), 0.0, 1.0);
        shadow = mix(shadow, shadowSample(cascade + 1, worldPosition, worldNormal, NdL), blend);
    }

    // And the far end, for the same reason: past the last cascade every fragment is lit, so the
    // last SHADOW_FADE_PERCENT of the shadow distance fades to lit rather than stopping dead.
    float fadeStart = lastSplit * (1.0 - float(SHADOW_FADE_PERCENT) / 100.0);

    return mix(shadow, 1.0, clamp((viewDepth - fadeStart) / max(lastSplit - fadeStart, 0.0001), 0.0, 1.0));
}

float phong_diffuse()
{
    return (1.0 / M_PI);
}

float D_GGX(in float roughness, in float NdH)
{
    float m = roughness * roughness;
    float m2 = m * m;
    float d = (NdH * m2 - NdH) * NdH + 1.0;
    return m2 / (M_PI * d * d);
}

vec3 fresnel_factor(in vec3 f0, in float product)
{
    return mix(f0, vec3(1.0), pow(1.01 - product, 5.0));
}

float G_schlick(in float roughness, in float NdV, in float NdL)
{
    float k = roughness * roughness * 0.5;
    float V = NdV * (1.0 - k) + k;
    float L = NdL * (1.0 - k) + k;
    return 0.25 / (V * L);
}

vec3 cooktorrance_specular(in float NdL, in float NdV, in float NdH, in vec3 specular, in float roughness)
{
    float D = D_GGX(roughness, NdH);
    float G = G_schlick(roughness, NdV, NdL);
    float rim = mix(1.0 - roughness * 0.6 * 0.9, 1.0, NdV);
    return (1.0 / rim) * specular * G * D;
}

// ---------------------------------------------------------------------------------------------
// Image-based lighting, from the scene's light probes.
//
// What this replaces: two samples of the sky cube map, one along the reflection vector at a fixed
// mip standing in for irradiance, one at a roughness-scaled mip standing in for a prefiltered
// environment, neither occluded by anything. The sky is visible from everywhere, so every surface
// got the same indirect light and the same specular highlight whether it stood in the open or
// against a wall in shadow — which is what put a rim of sky-coloured specular on geometry the
// shadow map had just darkened.
//
// A probe answers that by construction rather than by correction: it is a photograph of the world
// from a point, with the geometry and the shadows already in it. A probe standing in the
// building's shadow records the building, so what it hands back is dark.
// ---------------------------------------------------------------------------------------------

// The basis, evaluated. The same nine functions in the same order as the engine's C++ projection
// (Graphics/Api/SphericalHarmonics.cppm) — the two are one function written twice, and a
// difference between them would be a rotation applied to every probe's diffuse light.
vec3 evaluateIrradiance(vec4 coefficients[SH_COEFFICIENTS], vec3 direction)
{
    float x = direction.x;
    float y = direction.y;
    float z = direction.z;

    vec3 result = coefficients[0].rgb * 0.282095;
    result += coefficients[1].rgb * (0.488603 * y);
    result += coefficients[2].rgb * (0.488603 * z);
    result += coefficients[3].rgb * (0.488603 * x);
    result += coefficients[4].rgb * (1.092548 * x * y);
    result += coefficients[5].rgb * (1.092548 * y * z);
    result += coefficients[6].rgb * (0.315392 * (3.0 * z * z - 1.0));
    result += coefficients[7].rgb * (1.092548 * x * z);
    result += coefficients[8].rgb * (0.546274 * (x * x - y * y));

    // An order-2 reconstruction of anything with an edge in it rings, and the trough goes below
    // zero. Negative light is not a thing; clamping here is what stops it subtracting from the
    // direct term on the shadowed side of a surface.
    return max(result, vec3(0.0));
}

// How much of this fragment belongs to probe `index`.
//
// The ramp is per axis and the three are multiplied, so a fragment near a corner falls off in both
// directions at once rather than in whichever one happened to be tested. Outside the box some axis
// is negative and clamps to zero, which is the whole of the containment test — there is no
// separate inside/outside branch to disagree with the falloff.
float probeWeight(int index, vec3 worldPosition)
{
    if (frame.probes[index].boxMax.w != 0.0)
    {
        // The global probe has no bound. Its weight is decided by what the local probes left over,
        // not by where the fragment is.
        return 1.0;
    }

    vec3 insideDistance = min(worldPosition - frame.probes[index].boxMin.xyz,
                              frame.probes[index].boxMax.xyz - worldPosition);
    vec3 ramp = clamp(insideDistance / max(frame.probes[index].boxMin.w, 0.0001), 0.0, 1.0);

    return ramp.x * ramp.y * ramp.z;
}

// Re-aims the reflection vector at the point it actually leaves the influence box.
//
// A cube map is a record of the world as seen from one point, so reading it along the raw
// reflection vector places every reflected feature at infinity: a wall two metres away reflects as
// though it were on the horizon, and the reflection slides across a surface as the camera moves
// instead of staying pinned to the wall. Intersecting the box and re-aiming from the capture point
// is the standard correction, and the box is the one the probe already carries.
//
// A reflection vector with a zero component divides to infinity here, which the min/max chain
// discards on its own — the axis that cannot be crossed is the axis that never bounds the ray.
vec3 parallaxCorrect(int index, vec3 worldPosition, vec3 reflection)
{
    if (frame.probes[index].boxMax.w != 0.0)
    {
        // The global probe's environment is the sky, which genuinely is at infinity.
        return reflection;
    }

    vec3 inverseDirection = 1.0 / reflection;
    vec3 toMaximum = (frame.probes[index].boxMax.xyz - worldPosition) * inverseDirection;
    vec3 toMinimum = (frame.probes[index].boxMin.xyz - worldPosition) * inverseDirection;
    vec3 furthest = max(toMaximum, toMinimum);
    float distance = min(min(furthest.x, furthest.y), furthest.z);

    return normalize((worldPosition + reflection * distance) - frame.probes[index].position.xyz);
}

// The split-sum approximation's second half — the environment BRDF — as Lazarov's analytic fit
// rather than a lookup table.
//
// The table is the textbook answer and it is a 2D texture, a generating pass and a descriptor for
// something whose whole content is a smooth function of two numbers. The fit is within about a
// percent of it across the domain, which is far inside what the 128-pixel probe cube can resolve
// in the first place. Returns the scale and bias to apply to F0.
vec2 environmentBrdf(float roughness, float NdV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);

    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdV)) * r.x + r.y;

    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// How much of the environment a fragment's *specular* lobe can actually see, from how much of the
// hemisphere its diffuse one can.
//
// Ambient occlusion is a diffuse quantity: it integrates the whole hemisphere, where a specular
// lobe is narrow and pointed somewhere particular. Applying the diffuse figure to a mirror would
// darken a reflection that is looking straight out of the crevice; ignoring it leaves a crevice
// with a full-strength reflection of a sky it cannot see. Lagarde's form interpolates between the
// two by roughness, which is exactly the axis along which the lobe stops being a mirror.
float specularOcclusion(float NdV, float occlusion, float roughness)
{
    return clamp(pow(NdV + occlusion, exp2(-16.0 * roughness - 1.0)) - 1.0 + occlusion, 0.0, 1.0);
}

vec3 ads(vec4 albedo, vec4 metallicRoughness, vec3 normalMap)
{
    // Tangent space, for the direct term. The vertex stage hands over the light and view
    // directions already rotated into it — the names say world and the arithmetic says otherwise —
    // so the normal map is used raw and no basis is rebuilt here.
    vec3 V = normalize(viewDirectionWorldSpace);
    vec3 N = normalMap;

    float occlusion = metallicRoughness.r;
    float roughness = metallicRoughness.g;
    float metallic = metallicRoughness.b;

    // The material's own occlusion and the view's, combined into the term the indirect light is
    // scaled by. The screen-space half is sampled at this fragment's own pixel — the buffer is the
    // view's resolution, so the coordinate is the fragment's position in it and nothing has to be
    // scaled. It stays out of the direct term below on purpose: the sun either reaches a surface or
    // is stopped by something the shadow map already knows about, and darkening it here would be
    // the same occluder counted twice.
    float indirectOcclusion =
        occlusion * texture(ambientOcclusionMap, gl_FragCoord.xy / vec2(textureSize(ambientOcclusionMap, 0))).r;

    vec3 specular = mix(vec3(0.04), albedo.rgb, metallic);

    float NdV = max(0.001, dot(N, V));

    vec3 reflected_light = vec3(0);
    vec3 diffuse_light = vec3(0);

    // Once, outside the loop: one light casts the cascades, and the test is the same wherever in
    // the loop that light turns up. The geometric world normal, not the normal-mapped one — the
    // bias is a property of the surface the depth map recorded, not of its texture. Lights carry
    // the direction *towards* the light in `position`, which is what the loop below reads too.
    int shadowLight = clamp(frame.shadowParams.y, 0, MAX_LIGHTS - 1);
    float shadow = shadowFactor(positionInWorldSpace, normalize(normalsInWorldSpace),
                                normalize(frame.lights[shadowLight].position.xyz), -positionInViewSpace.z);

    for (int lightIndex = 0; lightIndex < frame.lightCount.x; lightIndex++)
    {
        vec3 L = normalize(lightDirectionWorldSpace[lightIndex]);
        vec3 H = normalize(L + V);

        float NdL = max(0.001, dot(N, L));
        float NdH = max(0.001, dot(N, H));
        float HdV = max(0.001, dot(H, V));

        vec3 specfresnel = fresnel_factor(specular, HdV);
        vec3 specref = cooktorrance_specular(NdL, NdV, NdH, specfresnel, roughness);

        specref *= vec3(NdL);

        vec3 diffref = (vec3(1.0) - specfresnel) * phong_diffuse() * NdL;

        float occlusion_from_shadow = (lightIndex == shadowLight) ? shadow : 1.0;

        vec3 light_color = frame.lights[lightIndex].diffuse.xyz * frame.lights[lightIndex].ambientAttenuation.w
            * occlusion_from_shadow;
        reflected_light += specref * light_color;
        diffuse_light += diffref * light_color;
    }

    // World space, for the indirect term: a probe is a fact about a place, so everything read from
    // one has to be asked for in the coordinates the place is described in. The view rotation is
    // orthonormal, so its transpose is its inverse and no matrix is inverted per fragment.
    mat3 viewToWorld = transpose(mat3(frame.viewMatrix));
    vec3 shadingNormalViewSpace = normalize((tangentInNormalSpace * normalMap.x) +
                                            (bitangentInNormalSpace * normalMap.y) +
                                            (normalsInNormalSpace * normalMap.z));
    vec3 worldNormal = normalize(viewToWorld * shadingNormalViewSpace);
    vec3 worldView = normalize(frame.cameraPosition.xyz - positionInWorldSpace);
    vec3 worldReflection = reflect(-worldView, worldNormal);
    float worldNdV = max(dot(worldNormal, worldView), 0.001);

    // Roughness picks the level of the prefiltered chain, inverting exactly the mapping the
    // prefilter pass used when it wrote it.
    float specularLevel = roughness * float(PROBE_SPECULAR_MIPS - 1);

    vec4 blendedIrradiance[SH_COEFFICIENTS];
    for (int coefficient = 0; coefficient < SH_COEFFICIENTS; coefficient++)
    {
        blendedIrradiance[coefficient] = vec4(0.0);
    }
    vec3 blendedRadiance = vec3(0.0);
    float localWeight = 0.0;

    // The local probes first, each weighted by how far inside its box this fragment is. Weights
    // are summed rather than normalised here: what they do not add up to is what the global probe
    // is for, and normalising now would give a fragment at the very edge of one small volume the
    // same confidence as one in the middle of it.
    for (int index = 0; index < frame.probeParams.x && index < MAX_IBL_PROBES; index++)
    {
        if (frame.probes[index].boxMax.w != 0.0)
        {
            continue;
        }

        float weight = probeWeight(index, positionInWorldSpace);
        if (weight <= 0.0)
        {
            continue;
        }

        localWeight += weight;

        for (int coefficient = 0; coefficient < SH_COEFFICIENTS; coefficient++)
        {
            blendedIrradiance[coefficient] += frame.probes[index].irradiance[coefficient] * weight;
        }

        vec3 direction = parallaxCorrect(index, positionInWorldSpace, worldReflection);
        blendedRadiance += textureLod(probeSpecular, vec4(direction, frame.probes[index].position.w),
                                      specularLevel).rgb * weight;
    }

    // Where volumes overlap the weights can exceed one; scaling back down is what makes an overlap
    // a cross-fade rather than a bright seam.
    if (localWeight > 1.0)
    {
        float normalisation = 1.0 / localWeight;
        for (int coefficient = 0; coefficient < SH_COEFFICIENTS; coefficient++)
        {
            blendedIrradiance[coefficient] *= normalisation;
        }
        blendedRadiance *= normalisation;
        localWeight = 1.0;
    }

    // Whatever the local probes did not account for falls to the global one, which is the scene's
    // view of the sky from somewhere it can see it. Without one, a fragment outside every local
    // volume has no indirect light at all — which is a legitimate scene and a very dark one.
    float remainingWeight = 1.0 - localWeight;
    if (remainingWeight > 0.0)
    {
        for (int index = 0; index < frame.probeParams.x && index < MAX_IBL_PROBES; index++)
        {
            if (frame.probes[index].boxMax.w == 0.0)
            {
                continue;
            }

            for (int coefficient = 0; coefficient < SH_COEFFICIENTS; coefficient++)
            {
                blendedIrradiance[coefficient] += frame.probes[index].irradiance[coefficient] * remainingWeight;
            }

            blendedRadiance += textureLod(probeSpecular, vec4(worldReflection, frame.probes[index].position.w),
                                          specularLevel).rgb * remainingWeight;
            break;
        }
    }

    vec3 indirectDiffuse = evaluateIrradiance(blendedIrradiance, worldNormal) * indirectOcclusion;

    // The other half of the split sum, and the reason the specular term no longer survives a
    // shadow: it is the probe's own radiance — dark where the probe stood in shadow — scaled by
    // what the surface's Fresnel and roughness actually reflect, and by how much of the
    // environment a lobe this rough can see from inside whatever crevice it is in.
    vec2 environment = environmentBrdf(roughness, worldNdV);
    vec3 indirectSpecular = blendedRadiance * (specular * environment.x + environment.y) *
                            specularOcclusion(worldNdV, indirectOcclusion, roughness);

    vec3 direct = occlusion * diffuse_light * mix(albedo.rgb, vec3(0.0), metallic) + max(reflected_light, 0.0);

    return direct + indirectDiffuse * mix(albedo.rgb, vec3(0.0), metallic) + indirectSpecular;
}

void main()
{
    vec2 transformedTextureCoordinates = (mat3(material.textureTransform) * vec3(textureCoordinates, 1.0)).xy;

    vec4 albedo = (material.useTextures.x != 0) ? texture(diffuseTexture, transformedTextureCoordinates) : material.baseColour;
    vec3 normalMap = (material.useTextures.y != 0) ? normalize(texture(normalTexture, transformedTextureCoordinates).xyz * 2.0 - 1.0) : normalize(vec3(0.0, 0.0, 1.0));
    vec4 specularMap = (material.useTextures.z != 0) ? texture(specularTexture, transformedTextureCoordinates) : vec4(1.0, material.roughMetal.x, material.roughMetal.y, 1.0);
    vec3 colour = ads(albedo, specularMap, normalMap);

    fragColor = vec4(colour, albedo.a);
}
