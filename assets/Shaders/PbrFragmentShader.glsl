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
    // The weather, appended under the same prefix rule as the air above it. Only y — the rain
    // intensity, Scene::rain — is read here: x is the clock and zw the airstream, which are the
    // windscreen's business. Zero rain is the dry scene, and the wet term is one branch on it.
    vec4 timeRain;
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


// ---------------------------------------------------------------------------------------------
// Volumetric fog, and the god rays that fall out of the same integral.
//
// Single scattering through a medium whose density falls off exponentially with height. It is in
// two halves and the split is what makes it affordable:
//
//  - **Extinction, and the light everything-but-the-sun scatters into the ray, are closed form.**
//    The optical depth of an exponential profile along a ray has an analytic value (Inigo Quilez,
//    https://iquilezles.org/articles/fog/), and because the density is the derivative of that
//    optical depth, the in-scattering integral of anything the shadow map does not touch is exactly
//    `1 - transmittance`. Neither is marched, so the haze is right out to the sky box for the cost
//    of an exponential.
//  - **The sun's half is marched**, because the shadow map is inside its integral. That is the god
//    rays: the medium is lit where the cascades say the sun reaches it.
//
// The quadrature is the load-bearing detail. Each step is weighted by the *difference* in
// transmittance across it — which is the analytic value of the in-scattering integral over that
// step — so with nothing shadowing anything the sum telescopes to exactly the `1 - transmittance`
// above. The marched half therefore cannot exceed the analytic total at any step count, and
// FOG_MARCH_STEPS is a quality knob rather than a correctness one.
//
// **Nothing here states a colour.** The shafts are the colour of the light casting them and the haze
// is the mean of what the scene's global probe photographed, so dusk fogs as dusk and midnight fogs
// as almost nothing, with no curve anywhere that somebody had to tune for either. A light probe
// capture uploads no fog at all — VulkanRenderer's probe path says why — which is what stops the
// second of those feeding itself.
//
// **This block is the same in PbrFragmentShader, BlinnPhongFragmentShader and — since the fog
// moved to the fullscreen pass for everything opaque (2026-08-25) — VolumetricFogFragmentShader,
// which fogs what these shaders' blended draws are composited over. It must not drift between
// them**: this engine compiles each shader from one source string and
// has no include resolver to share code through, and two surfaces of one scene fogging by different
// arithmetic would part company along the seam between them. `Graphics/Api/VolumetricFog.cppm` is
// the same arithmetic once more in C++, where it is unit-tested without a device;
// docs/volumetric-fog-brief.md is the account.
// ---------------------------------------------------------------------------------------------

// How far the density profile's exponent may run either side of the reference height before it is
// held. Past this the medium is either so thin that nothing survives rounding or so thick that
// nothing survives the medium, and exp() of it stops being representable.
const float fogMaximumExponent = 80.0;

// The optical depth of the medium along a ray, in closed form. `directionY` is the y component of a
// normalised direction and `rayLength` how far along it to integrate, in world units.
float fogOpticalDepth(float originHeight, float directionY, float rayLength)
{
    float falloff = frame.fogDensity.y;

    // The profile's exponent at each end of the ray, in scale heights above the reference, and the
    // climb between them. Stated in the two *ends* rather than in one end and a climb, and that is a
    // correction rather than a preference: clamping the climb alone reads as no fog at all on
    // exactly the ray it was meant to protect. A camera high above a shallow layer looking steeply
    // down has a tiny density at the eye and an enormous one at the far end, and the product of an
    // underflow and a clamped overflow is zero — a ray that should be completely opaque coming back
    // completely clear. Clamping each end instead bounds the *medium*, which is a statement about
    // the fog rather than about the ray's geometry.
    float nearExponent = (originHeight - frame.fogDensity.z) * falloff;
    float farExponent = nearExponent + rayLength * directionY * falloff;
    float climb = farExponent - nearExponent;

    float nearDensity = exp(-clamp(nearExponent, -fogMaximumExponent, fogMaximumExponent));
    float farDensity = exp(-clamp(farExponent, -fogMaximumExponent, fogMaximumExponent));

    // The integral is `density * rayLength * (1 - exp(-climb)) / climb`, which is the difference of
    // the two densities over the climb. At climb = 0 that is 0/0 with a limit of 1, and near zero it
    // is the difference of two nearly equal numbers over a small one — most of a float's mantissa.
    // A level ray is exactly zero and a level ray is what a driver spends the lap looking along, so
    // the series is the common case rather than an edge one.
    if (abs(climb) > 1.0e-2)
    {
        return max(frame.fogDensity.x * rayLength * (nearDensity - farDensity) / climb, 0.0);
    }

    return max(frame.fogDensity.x * rayLength * nearDensity
                   * (1.0 - climb * 0.5 + climb * climb * (1.0 / 6.0)),
               0.0);
}

// Henyey-Greenstein, normalised so that isotropic scattering is exactly 1 rather than 1/4pi. The
// factor is carried where a directional source's irradiance becomes a radiance instead, which is the
// one place it belongs. `cosTheta` is dot(ray, towards the light), so a camera looking straight at
// the sun reads 1 and gets the forward lobe — and that asymmetry *is* the effect: at zero there is
// no shaft, only a wash.
float fogPhase(float cosTheta, float anisotropy)
{
    float g = clamp(anisotropy, -0.95, 0.95);
    float gg = g * g;
    float denominator = 1.0 + gg - 2.0 * g * clamp(cosTheta, -1.0, 1.0);

    return (1.0 - gg) / max(pow(max(denominator, 0.0), 1.5), 1.0e-4);
}

// The colour of the light the medium scatters in from everywhere that is not the sun: the global
// probe's band-0 spherical-harmonic coefficient, which is the mean of the sphere it photographed and
// so is the isotropic in-scattering term by definition rather than by approximation. A scene with no
// global probe has no answer to this and fogs from the sun alone, which is dark and is honest.
vec3 fogAmbientRadiance()
{
    for (int index = 0; index < frame.probeParams.x && index < MAX_IBL_PROBES; index++)
    {
        if (frame.probes[index].boxMax.w == 0.0)
        {
            continue;
        }

        return max(frame.probes[index].irradiance[0].rgb * 0.282095, vec3(0.0));
    }

    return vec3(0.0);
}

// How much of the shadow-casting light reaches a point in the *air*.
//
// Not `shadowFactor`, and the differences are the point: that one offsets its sample along a surface
// normal and biases by the surface's slope, and a point in a medium has neither. What it keeps is
// the cascade choice and the rule at the far end — past the last cascade every sample is lit, and
// the last SHADOW_FADE_PERCENT of the distance fades to lit rather than stopping dead, or the shafts
// would end on a sphere ruled through the air at a fixed distance from the camera.
float fogVisibility(vec3 worldPosition, float viewDepth)
{
    if (frame.shadowParams.x <= 0)
    {
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

    vec4 lightSpace = frame.shadowMatrices[cascade] * vec4(worldPosition, 1.0);
    vec3 coordinate = lightSpace.xyz / lightSpace.w;

    // Past the cascade's far plane nothing was stored to compare against, exactly as on a surface.
    if (coordinate.z >= 1.0)
    {
        return 1.0;
    }

    // The constant part of the surface path's bias budget and none of its slope part, in the same
    // texel units: there is no surface here to acne against, but the last steps of a march stand
    // close enough to one to fight with it.
    coordinate.z -= frame.shadowTexelWorldSize[cascade] * float(SHADOW_CONSTANT_BIAS_TEXELS)
        * frame.shadowDepthScale[cascade];

    float visibility = 1.0;

    // One tap, and no percentage-closer filter: the march already averages FOG_MARCH_STEPS lookups
    // along the ray and the dither is what turns the boundary between them into a gradient. An array
    // of samplers may only be indexed by a dynamically uniform expression, which is what the loop is
    // for — `shadowInCascade` does the same dance for the same reason.
    for (int index = 0; index < SHADOW_CASCADES; index++)
    {
        if (index != cascade)
        {
            continue;
        }

        visibility = texture(shadowMaps[index], coordinate);
    }

    float fadeStart = lastSplit * (1.0 - float(SHADOW_FADE_PERCENT) / 100.0);

    return mix(visibility, 1.0, clamp((viewDepth - fadeStart) / max(lastSplit - fadeStart, 0.0001), 0.0, 1.0));
}

// Interleaved gradient noise — a function of the pixel and of nothing else, so two captures of one
// frame stay identical and the parity gate still means something. The same generator the occlusion
// gather rotates its slices by. Here it moves each step's sample inside the step it stands for:
// without it sixteen steps land on sixteen surfaces spread across the screen, and a surface in a
// medium reads as a band.
float fogDither(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// The medium between the eye and a point `rayLength` away along `rayDirection`: rgb the radiance it
// scatters into the ray, a the fraction of whatever is behind it that survives.
vec4 fogAlongRay(vec3 rayDirection, float rayLength)
{
    float originHeight = frame.cameraPosition.y;
    float reach = min(rayLength, frame.fogDensity.w);
    float transmittance = exp(-fogOpticalDepth(originHeight, rayDirection.y, reach));

    int shadowLight = clamp(frame.shadowParams.y, 0, MAX_LIGHTS - 1);
    vec3 towardsLight = normalize(frame.lights[shadowLight].position.xyz);

    // The march reaches only as far as the cascades do. Past the last split the shadow code shades
    // every fragment lit — there is no map to ask — so the rest of the ray is unshadowed by that
    // same statement rather than by a second one, and it is the closed form's to carry.
    float shadowReach = frame.shadowParams.x > 0 ? frame.shadowSplits[frame.shadowParams.x - 1] : 0.0;
    float marchReach = min(reach, shadowReach);

    // How far the ray leans off the view axis, which is what turns a distance along it into the depth
    // the cascade splits are stated in. Once, because a straight ray's is constant.
    float viewAxis = -(mat3(frame.viewMatrix) * rayDirection).z;

    float sunIntegral = 0.0;
    float previous = 1.0;

    if (marchReach > 0.0)
    {
        float jitter = fogDither(gl_FragCoord.xy);

        for (int index = 1; index <= FOG_MARCH_STEPS; index++)
        {
            float here = marchReach * float(index) / float(FOG_MARCH_STEPS);
            float survives = exp(-fogOpticalDepth(originHeight, rayDirection.y, here));

            float sampleAt = marchReach * (float(index) - jitter) / float(FOG_MARCH_STEPS);
            float visibility = fogVisibility(frame.cameraPosition.xyz + rayDirection * sampleAt,
                                             sampleAt * viewAxis);

            // Weighted by the transmittance the step spans rather than by the length of it: that is
            // the analytic in-scattering over the step, and it is what makes this sum telescope.
            sunIntegral += visibility * (previous - survives);
            previous = survives;
        }
    }

    // Whatever is left of the ray beyond the cascades, at visibility one.
    sunIntegral += previous - transmittance;

    // A directional source of irradiance E scattered isotropically gives a radiance of E/4pi in every
    // direction, and the phase function above is stated relative to that isotropic case — so this is
    // where the 4pi the published phase function carries actually lives. Both halves below are
    // radiances and can be added.
    const float fogInverseSphere = 0.0795774715;

    vec3 sunRadiance = frame.lights[shadowLight].diffuse.rgb * fogInverseSphere * frame.fogAmbient.w
        * fogPhase(dot(rayDirection, towardsLight), frame.fogScatter.w);
    vec3 ambientRadiance = fogAmbientRadiance() * frame.fogAmbient.xyz;

    // The single-scatter albedo is the fraction of extinction that scatters rather than absorbs, and
    // it is the one coloured coefficient here: this is Quilez's separate extinction and in-scattering
    // said once, which keeps the transmittance and every marched weight a single number.
    vec3 inScattered = frame.fogScatter.rgb
        * (ambientRadiance * (1.0 - transmittance) + sunRadiance * sunIntegral);

    return vec4(inScattered, transmittance);
}

// The medium applied to a shaded surface.
//
// `coverage` is the surface's own alpha. The scene pass blends premultiplied, so what the fog adds
// has to be scaled by coverage exactly as the diffuse term is: a windscreen at alpha 0.3 that added
// a whole helping of haze would lay it over a road that has already been fogged over a longer ray
// behind it. At alpha 1 this is the plain statement, so every opaque surface reads the obvious thing.
vec3 applyFog(vec3 colour, vec3 worldPosition, float coverage)
{
    // Zero density is the scene that states no fog, and it is the only branch: the block is
    // value-initialised, so this is also every frame drawn before the feature existed.
    if (frame.fogDensity.x <= 0.0)
    {
        return colour;
    }

    vec3 toFragment = worldPosition - frame.cameraPosition.xyz;
    float rayLength = length(toFragment);
    if (rayLength <= 0.0)
    {
        return colour;
    }

    vec4 fog = fogAlongRay(toFragment / rayLength, rayLength);

    return colour * fog.a + fog.rgb * coverage;
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

void main()
{
    vec2 transformedTextureCoordinates = (mat3(material.textureTransform) * vec3(textureCoordinates, 1.0)).xy;

    // glTF's factors **multiply** the texture; they are not the value it stands in for.
    // baseColor = baseColorFactor x texture, roughness = roughnessFactor x texture.g,
    // metallic = metallicFactor x texture.b. Written as a ternary — factor *or* texture — a
    // material carrying both silently discards the factor, and the factor is where an exporter
    // puts the part of the answer the shared texture cannot say: this scene's car paint is one
    // metallicRoughness atlas with `roughnessFactor 0, metallicFactor 0` on the paint slot, so
    // the body panels were shaded as rough metal off a table meant to be scaled to nothing.
    //
    // A material with no texture reads 1 here and gets its factor alone, which is exactly what
    // the ternary produced — so the untextured case is unchanged and only the discarding stops.
    vec4 sampledBaseColour =
        (material.useTextures.x != 0) ? texture(diffuseTexture, transformedTextureCoordinates) : vec4(1.0);
    vec4 sampledMetallicRoughness =
        (material.useTextures.z != 0) ? texture(specularTexture, transformedTextureCoordinates) : vec4(1.0);

    vec4 albedo = material.baseColour * sampledBaseColour;
    albedo.rgb *= detailModulation(transformedTextureCoordinates);

    // glTF MASK: under the cutoff there is no surface here at all. Discarded before any lighting
    // is spent on it, and *not* blended — a cut-out is opaque geometry with holes, which is what
    // lets a tree card write depth and sort like the trunk it stands in for.
    if (material.roughMetal.z > 0.0 && albedo.a < material.roughMetal.z) {
        discard;
    }

    // **glTF MASK is binary, and this is where that has to be enforced.** The spec is explicit: a
    // masked material renders "either fully opaque or fully transparent" according to the cutoff.
    // The discard above delivers the transparent half; without the line below the opaque half was
    // never delivered, because the coverage that survived the test was then written out *as* the
    // fragment's alpha — and every scene draw in this engine blends premultiplied, so a texel at 0.6
    // against a 0.4 cutoff passed the test and then composited at sixty percent over whatever had
    // already been drawn. On a tree card that reads as a soft edge and nobody looks twice; on the
    // ground cover, the sand edges and the kerb decals it reads as **seeing other parts of the track
    // through the ground**, which is what it was reported as.
    //
    // A cut-out is opaque geometry with holes. Above the cutoff there is surface here, and surface
    // here is opaque.
    if (material.roughMetal.z > 0.0)
    {
        albedo.a = 1.0;
    }


    vec3 normalMap = (material.useTextures.y != 0) ? normalize(texture(normalTexture, transformedTextureCoordinates).xyz * 2.0 - 1.0) : normalize(vec3(0.0, 0.0, 1.0));

    // The other half of a double-sided material. A fragment seen from behind is the surface's other
    // side, and every consumer of its normal has to agree: flipped here, the direct term stops
    // lighting the wrong hemisphere and the probe lookup reflects the world the *viewer's* side of
    // the pane actually faces. This is what a windscreen looked wrong without — from the seat the
    // camera sees the glass's inner surface, whose unflipped normal still pointed at the sky, so the
    // probe handed the driver a bright sky sheen off a pane that should be reflecting the dark
    // cabin's side of the world.
    if (!gl_FrontFacing) {
        normalMap = vec3(normalMap.xy, -normalMap.z);
    }
    // r stays the sampled channel: it is read as occlusion and glTF states no factor for it.
    vec4 specularMap = vec4(sampledMetallicRoughness.r,
                            material.roughMetal.x * sampledMetallicRoughness.g,
                            material.roughMetal.y * sampledMetallicRoughness.b,
                            1.0);

    // The rain's wetness — the metalness-roughness half of the statement BlinnPhongFragmentShader
    // makes, so a wet prop beside the wet circuit reads as standing in the same weather. A film of
    // water darkens the body and drops the roughness hard; the indirect specular this model already
    // gathers then does the rest, which is the sky arriving in every wet surface for free. No
    // puddles here: this path shades props and the car, and standing water belongs to the ground.
    // One branch on the scene's own rain, so a dry frame is bit-for-bit the shader without it.
    // Opaque surfaces only, for the reason BlinnPhongFragmentShader states at its own wet block:
    // coverage scales the body and not the highlight, so a blended surface wetted here would add
    // its wet reflection at full strength over partial coverage. Glass already is water.
    if (frame.timeRain.y > 0.0 && material.useTextures2.y != 0)
    {
        float wetness = clamp(frame.timeRain.y, 0.0, 1.0);
        albedo.rgb *= 1.0 - 0.35 * wetness;
        specularMap.g = mix(specularMap.g, min(specularMap.g, 0.12), wetness);
    }

    vec3 colour = ads(albedo, specularMap, normalMap);

    // Opaque fog belongs to the fullscreen fog pass now — the reason is at
    // BlinnPhongFragmentShader's own gate: opaque surfaces are in the occlusion prepass the pass
    // reads its depths from, blended ones are not, and probe captures upload no fog either way.
    if (material.useTextures2.y == 0)
    {
        colour = applyFog(colour, positionInWorldSpace, albedo.a);
    }

    fragColor = vec4(colour, albedo.a);
}
