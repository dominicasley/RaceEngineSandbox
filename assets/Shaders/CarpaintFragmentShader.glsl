#version 450
// Car paint: a PBR base coat with metallic flake suspended in it, a clearcoat over the top, and the
// orange peel a sprayed lacquer never quite loses.
//
// MAX_LIGHTS, SET_* and the TEXTURE_* bindings are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.
//
// **What is a car and what is a colour are two different scopes here, and that is the design.** The
// maps — flake pattern, scratch, dirt — are the *asset* and live on the material. The numbers —
// which colour, how much flake, how dirty — are the *car* and arrive through PAINT_DATA_BINDING, a
// dynamic-offset block in the per-draw set that carries the renderable's own `Paint`. So a game
// recolours a car by writing a field on the car it holds, two cars built from one model are two
// colours, and nothing has to go looking inside a model for a material it did not author.
//
// The base coat is shaded by **`ads`, lifted verbatim from PbrFragmentShader**, so a body panel sits
// against the rest of the scene exactly as any other PBR surface does. Everything below it is
// added on top rather than replacing it. The shadow stack, the BRDF and the image-based lighting are
// that file's too and **must not drift from it**.

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
layout(location = 8 + MAX_LIGHTS) in vec3 positionInModelSpace;

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
    // The air. Appended to the block for the reason the material block's fifth vec4 was: a uniform
    // block may be a prefix of the buffer backing it, so a stage that declares only as far as what
    // it reads keeps matching, and a field inserted above would silently move every probe under
    // every one of them. Zero density is a scene that states no fog. See Fog, and applyFog below.
    vec4 fogDensity;               // x extinction at the reference height, y 1/scale height,
                                   // z that reference height, w how far the medium is integrated
    vec4 fogScatter;               // xyz single-scatter albedo, w Henyey-Greenstein's asymmetry
    vec4 fogAmbient;               // xyz a tint on the ambient half, w a gain on the sun's
} frame;

// Every probe's prefiltered radiance, as one cube array: layers 6i..6i+5 are probe i, and the mip
// chain is the roughness axis. One image rather than one per probe is what lets the loop below
// pick a probe with a dynamic index.
layout(set = SET_FRAME, binding = PROBE_SPECULAR_BINDING) uniform samplerCubeArray probeSpecular;

// textureTransform is a 3x3 UV transform in a mat4 slot: std140 pads a mat3's columns to 16
// bytes each, which the C++ glm::mat3 does not, so the ABI carries it as a mat4.
layout(set = SET_MATERIAL, binding = 0) uniform MaterialData {
    vec4 baseColour;
    vec4 roughMetal;       // x roughness, y metalness, z alpha cutoff (0 = no test)
    ivec4 useTextures;     // x diffuse, y normal, z specular, w emissive
    ivec4 useTextures2;    // x occlusion
    mat4 textureTransform; // KHR_texture_transform, upper 3x3
    vec4 blinnPhong;       // the classic reflectance model's coefficients; read by BlinnPhongFragmentShader
    // The blended-material feature: xyzw are the four detail layers' tiling, in repeats per unit of
    // the model's own space, and blend.x is their strength with blend.y non-zero when the material
    // states any. Declared here because the block has one std140 layout every stage must agree on.
    vec4 detailTiling;
    vec4 blend;
} material;
layout(set = SET_MATERIAL, binding = TEXTURE_DIFFUSE) uniform sampler2D diffuseTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_NORMAL) uniform sampler2D normalTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_SPECULAR) uniform sampler2D specularTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_EMISSIVE) uniform sampler2D emissiveTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_OCCLUSION) uniform sampler2D occlusionTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_ENVIRONMENT) uniform samplerCube environmentMap;
layout(set = SET_MATERIAL, binding = TEXTURE_BLEND_MASK) uniform sampler2D blendMaskTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_DETAIL_R) uniform sampler2D detailRTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_DETAIL_G) uniform sampler2D detailGTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_DETAIL_B) uniform sampler2D detailBTexture;
layout(set = SET_MATERIAL, binding = TEXTURE_DETAIL_A) uniform sampler2D detailATexture;

// What the detail layers do to the base colour — the same function as BlinnPhongFragmentShader's,
// and it must not drift from it: two surfaces of one blended material drawn through different
// shaders would otherwise stop matching at the seam between them. Full account there and in
// Material.cppm. A material stating no layers returns 1 and is left exactly as it was, which is
// what makes this inert for every asset that predates the feature.
vec3 detailModulation(vec2 uv)
{
    if (material.blend.y == 0.0)
    {
        return vec3(1.0);
    }

    vec4 mask = texture(blendMaskTexture, uv);
    vec2 p = positionInModelSpace.xz;

    vec3 combined = texture(detailRTexture, p * material.detailTiling.x).rgb * mask.r
                  + texture(detailGTexture, p * material.detailTiling.y).rgb * mask.g
                  + texture(detailBTexture, p * material.detailTiling.z).rgb * mask.b
                  + texture(detailATexture, p * material.detailTiling.w).rgb * mask.a;

    return combined * material.blend.x;
}

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
    // A floor, because zero is now reachable: glTF's roughnessFactor multiplies the texture and
    // this scene's car paint states 0, where before the factor was discarded and nothing here ever
    // saw it. At exactly zero D_GGX is 0/0 for a fragment whose half-vector lands on its normal —
    // a NaN, which the bloom chain would then spread across the frame — and everywhere else it is
    // simply 0, so a mirror-smooth surface gets no sun glint at all, which is the wrong end of
    // "perfectly polished". 0.03 sits under every authored value in these assets (the next
    // smallest is the building's reflective glass at 0.0398), so it changes nothing that is not
    // the degenerate case itself.
    float roughness = max(metallicRoughness.g, 0.03);
    float metallic = metallicRoughness.b;

    // The material's own occlusion and the view's, combined into the term the indirect light is
    // scaled by. The screen-space half is sampled at this fragment's own pixel — the buffer is the
    // view's resolution, so the coordinate is the fragment's position in it and nothing has to be
    // scaled. It stays out of the direct term below on purpose: the sun either reaches a surface or
    // is stopped by something the shadow map already knows about, and darkening it here would be
    // the same occluder counted twice.
    //
    // A transparent surface reads none of it. The buffer holds one opaque surface per pixel and
    // that surface is whatever is *behind* this glass, so sampling it here paints the interior's
    // occlusion onto the window. The other half of the rule is in the engine, which keeps
    // transparent geometry out of the prepass entirely — where two coplanar panes would otherwise
    // fight and the gather would print the fight back onto the glass as hard-edged wedges.
    float screenOcclusion = material.useTextures2.y != 0
        ? texture(ambientOcclusionMap, gl_FragCoord.xy / vec2(textureSize(ambientOcclusionMap, 0))).r
        : 1.0;
    float indirectOcclusion = occlusion * screenOcclusion;

    vec3 specular = mix(vec3(0.04), albedo.rgb, metallic);

    float NdV = max(0.001, dot(N, V));

    vec3 reflected_light = vec3(0);
    vec3 diffuse_light = vec3(0);

    // Once, outside the loop: one light casts the cascades, and the test is the same wherever in
    // the loop that light turns up. The geometric world normal, not the normal-mapped one — the
    // bias is a property of the surface the depth map recorded, not of its texture. Lights carry
    // the direction *towards* the light in `position`, which is what the loop below reads too.
    int shadowLight = clamp(frame.shadowParams.y, 0, MAX_LIGHTS - 1);
    vec3 geometricNormal = normalize(gl_FrontFacing ? normalsInWorldSpace : -normalsInWorldSpace);
    float shadow = shadowFactor(positionInWorldSpace, geometricNormal,
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

    // Premultiplied, and the split is the whole point of it: **coverage scales the diffuse term and
    // not the reflection**. A pane of glass transmits most of what is behind it and still reflects
    // the sky at full strength — the reflection is light the surface adds, not light it fails to
    // block — so a windscreen at alpha 0.3 shaded the naive way loses 70% of the one cue that says
    // "glass" and reads as a faint grey film over the interior, which is what an absent windscreen
    // also looks like. Scaled here rather than by the blend state so the frame carries the sum
    // rather than the ingredients, and the blend is (ONE, ONE_MINUS_SRC_ALPHA) to match.
    //
    // At alpha 1 this is exactly the expression it replaced, so every opaque surface is unmoved.
    vec3 diffuseAlbedo = mix(albedo.rgb, vec3(0.0), metallic);
    vec3 diffuseTerm = occlusion * diffuse_light * diffuseAlbedo + indirectDiffuse * diffuseAlbedo;
    vec3 specularTerm = max(reflected_light, 0.0) + indirectSpecular;

    return diffuseTerm * albedo.a + specularTerm;
}


// ---------------------------------------------------------------------------------------------
// The paint itself.
// ---------------------------------------------------------------------------------------------

// The renderable's own paint, from the per-draw set. Every draw binds this because the layout
// declares it; a draw whose renderable states no paint binds offset zero and reads a zeroed block,
// which is why nothing here may treat zero as a meaningful setting.
layout(set = SET_DRAW, binding = PAINT_DATA_BINDING) uniform PaintData {
    vec4 colour;      // xyz the base coat, w how much of the surface is flake
    vec4 flake;       // xyz the flake's own colour, w how fine it is
    vec4 clearcoat;   // x strength, y roughness, z orange peel amount, w orange peel scale
    vec4 wear;        // x scratch, y dirt, z non-zero when this renderable states paint at all
} paint;

// The two maps this shader adds to the material, both in slots the renderer binds and nothing else
// reads. Scratch rides `occlusion` because that slot is sampled as **linear** and a scratch mask is
// an amount rather than a colour; dirt rides `emissive` because that one is a colour slot and grime
// has a colour. A material stating neither reads white and is simply clean.
#define scratchTexture occlusionTexture
#define dirtTexture emissiveTexture

// A cheap 3-vector hash (Dave Hoskins). Deterministic in the model's own space, which is what keeps
// the flake pinned to the panel instead of swimming across it as the car moves.
vec3 hash33(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);

    return fract((p.xxy + p.yxx) * p.zyx);
}

// One flake facet's normal. The paint is a suspension of tiny mirrors at random angles, so the
// sparkle is not a texture — it is what happens when a few of those mirrors happen to point the sun
// at your eye, and it resolves into individual glints as the camera closes. Cells in model space,
// one random tilt each.
vec3 flakeNormal(vec3 surfaceNormal, float scale)
{
    vec3 cell = floor(positionInModelSpace * scale);
    vec3 random = hash33(cell) * 2.0 - 1.0;

    // Tilted off the surface rather than pointed anywhere: a flake lying flat in the lacquer only
    // ever departs from the panel by a few degrees, and a fully random hemisphere would read as
    // glitter glued on top rather than metal suspended in the paint.
    return normalize(surfaceNormal + random * 0.35);
}

// Smoothly interpolated value noise: the cell hash above, trilinearly blended between the eight
// corners of the cell a point falls in, with a smoothstep on the weights so the derivative is
// continuous across cell boundaries too.
//
// **The interpolation is the whole point and its absence is visible.** `hash33(floor(p))` on its own
// is *cell* noise — one constant value per cube, with a hard edge at every boundary — which is
// exactly what a flake facet wants and exactly what a ripple must not be. Used raw for orange peel
// it quilts a car's panels into centimetre squares of flat shading, which reads as faceted geometry
// and sends you looking at tangents and normals rather than at the noise that caused it.
vec3 smoothHash33(vec3 p)
{
    vec3 cell = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    vec3 n000 = hash33(cell + vec3(0.0, 0.0, 0.0));
    vec3 n100 = hash33(cell + vec3(1.0, 0.0, 0.0));
    vec3 n010 = hash33(cell + vec3(0.0, 1.0, 0.0));
    vec3 n110 = hash33(cell + vec3(1.0, 1.0, 0.0));
    vec3 n001 = hash33(cell + vec3(0.0, 0.0, 1.0));
    vec3 n101 = hash33(cell + vec3(1.0, 0.0, 1.0));
    vec3 n011 = hash33(cell + vec3(0.0, 1.0, 1.0));
    vec3 n111 = hash33(cell + vec3(1.0, 1.0, 1.0));

    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

// Low-frequency ripple in the lacquer. It bends the clearcoat's normal and nothing else, so what it
// moves is the *reflection* — which is exactly how orange peel reads on a real car: the panel's
// colour is untouched and the world reflected in it goes wavy.
//
// Two octaves, because one is too regular to read as a sprayed finish: real peel has a dominant
// wavelength with a finer structure riding on it.
vec3 orangePeelNormal(vec3 surfaceNormal, float amount, float scale)
{
    if (amount <= 0.0)
    {
        return surfaceNormal;
    }

    vec3 ripple = (smoothHash33(positionInModelSpace * scale) * 2.0 - 1.0)
                + (smoothHash33(positionInModelSpace * scale * 2.7) * 2.0 - 1.0) * 0.5;

    return normalize(surfaceNormal + ripple * amount * 0.05);
}

// A single GGX lobe, for the two this shader adds that `ads` does not have: the clearcoat and the
// flake. Same D and G as the base coat uses, so the three agree about what a highlight is.
vec3 specularLobe(vec3 normal, vec3 viewDirection, vec3 lightDirection, float roughness, vec3 f0)
{
    vec3 halfVector = normalize(lightDirection + viewDirection);
    float NdL = max(dot(normal, lightDirection), 0.001);
    float NdV = max(dot(normal, viewDirection), 0.001);
    float NdH = max(dot(normal, halfVector), 0.001);
    float HdV = max(dot(halfVector, viewDirection), 0.001);

    return fresnel_factor(f0, HdV) * D_GGX(roughness, NdH) * G_schlick(roughness, NdV, NdL) * NdL;
}

void main()
{
    vec2 uv = (mat3(material.textureTransform) * vec3(textureCoordinates, 1.0)).xy;

    // **A painted car ignores the material's base map, and that is deliberate.** On AC content that
    // map is the *skin* — this Golf ships `05_Ruben_Black`, whose `carpaint.dds` is simply black —
    // so multiplying the paint into it gives black paint whatever colour was asked for, which is
    // exactly what it did the first time. Painting a car replaces its skin; a car that wants its
    // skin does not state paint.
    vec4 sampledBase = (material.useTextures.x != 0) ? texture(diffuseTexture, uv) : vec4(1.0);
    vec4 sampledMetallicRoughness = (material.useTextures.z != 0) ? texture(specularTexture, uv) : vec4(1.0);

    // Scratch and dirt, each scaled by how much of it *this car* has.
    float scratch = paint.wear.x * (1.0 - texture(scratchTexture, uv).r);
    vec4 dirt = texture(dirtTexture, uv);
    float dirtAmount = clamp(paint.wear.y * dirt.a, 0.0, 1.0);

    // **Whether this renderable is painted at all**, which is a different question from what colour
    // it is. Every draw binds this block and an unpainted one binds offset zero, so without the flag
    // a car nobody painted would be black paint with no lacquer rather than a car drawn as its own
    // textures describe it.
    bool painted = paint.wear.z != 0.0;

    vec3 baseColour = painted ? paint.colour.rgb : sampledBase.rgb * material.baseColour.rgb;
    baseColour = mix(baseColour, dirt.rgb, dirtAmount);

    vec3 normalMap = (material.useTextures.y != 0)
        ? normalize(texture(normalTexture, uv).xyz * 2.0 - 1.0)
        : normalize(vec3(0.0, 0.0, 1.0));
    if (!gl_FrontFacing) {
        normalMap = vec3(normalMap.xy, -normalMap.z);
    }

    // The base coat, shaded exactly as any other PBR surface is. Roughened where the paint is
    // scratched or filthy, because both are the lacquer stopping being smooth.
    float baseRoughness = clamp(material.roughMetal.x * sampledMetallicRoughness.g
                                + scratch * 0.4 + dirtAmount * 0.3, 0.03, 1.0);
    vec4 specularMap = vec4(sampledMetallicRoughness.r, baseRoughness,
                            material.roughMetal.y * sampledMetallicRoughness.b, 1.0);

    vec3 colour = ads(vec4(baseColour, 1.0), specularMap, normalMap);

    // World space for the two lobes below: they reflect the world, so they are asked for in the
    // coordinates the world is described in.
    mat3 viewToWorld = transpose(mat3(frame.viewMatrix));
    vec3 shadingNormalViewSpace = normalize((tangentInNormalSpace * normalMap.x) +
                                            (bitangentInNormalSpace * normalMap.y) +
                                            (normalsInNormalSpace * normalMap.z));
    vec3 worldNormal = normalize(viewToWorld * shadingNormalViewSpace);
    vec3 worldView = normalize(frame.cameraPosition.xyz - positionInWorldSpace);

    int shadowLight = clamp(frame.shadowParams.y, 0, MAX_LIGHTS - 1);
    vec3 worldLight = normalize(frame.lights[shadowLight].position.xyz);
    vec3 geometricNormal = normalize(gl_FrontFacing ? normalsInWorldSpace : -normalsInWorldSpace);
    float shadow = shadowFactor(positionInWorldSpace, geometricNormal, worldLight, -positionInViewSpace.z);
    vec3 sunlight = frame.lights[shadowLight].diffuse.xyz * shadow;

    // **Flake.** A second, very sharp lobe about a randomly tilted facet, coloured by the flake and
    // weighted by how much of the surface is flake rather than base. Scratched paint has lost its
    // flake along with its lacquer.
    if (painted && paint.colour.w > 0.0)
    {
        vec3 facet = flakeNormal(worldNormal, max(paint.flake.w, 1.0));
        vec3 sparkle = specularLobe(facet, worldView, worldLight, 0.06, paint.flake.rgb);

        colour += sparkle * sunlight * paint.colour.w * (1.0 - scratch);
    }

    // **Clearcoat.** The lacquer over the top: a sharp bright lobe on a Fresnel of 0.04, riding the
    // broader one the base coat already gave, which is the difference between paint and plastic. Its
    // normal is the orange-peel one, so the ripple shows in the reflection and nowhere else.
    float clearcoat = painted ? paint.clearcoat.x * (1.0 - scratch * 0.7) : 0.0;
    if (clearcoat > 0.0)
    {
        vec3 coatNormal = orangePeelNormal(worldNormal, paint.clearcoat.z, max(paint.clearcoat.w, 1.0));
        float coatRoughness = clamp(paint.clearcoat.y + scratch * 0.5 + dirtAmount * 0.3, 0.02, 1.0);

        vec3 coat = specularLobe(coatNormal, worldView, worldLight, coatRoughness, vec3(0.04));

        // What the lacquer reflects of the sky, which is most of what makes a parked car read as
        // painted at all. The probe's own prefiltered chain, at the coat's roughness.
        vec3 coatReflection = vec3(0.0);
        vec3 coatBounce = reflect(-worldView, coatNormal);
        for (int index = 0; index < frame.probeParams.x && index < MAX_IBL_PROBES; index++)
        {
            if (frame.probes[index].boxMax.w == 0.0)
            {
                continue;
            }

            coatReflection = textureLod(probeSpecular, vec4(coatBounce, frame.probes[index].position.w),
                                        coatRoughness * float(PROBE_SPECULAR_MIPS - 1)).rgb;
            break;
        }

        float coatFresnel = 0.04 + 0.96 * pow(1.0 - max(dot(coatNormal, worldView), 0.0), 5.0);

        // Energy: what the lacquer reflects, the base coat underneath never receives.
        colour *= (1.0 - coatFresnel * clearcoat);
        colour += (coat * sunlight + coatReflection * coatFresnel) * clearcoat;
    }

    fragColor = vec4(colour, 1.0);
}
