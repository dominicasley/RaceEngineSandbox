#version 450
// The classic reflectance model — ambient, diffuse and specular coefficients with a specular
// exponent — for content authored against it. MAX_LIGHTS, SET_* and the TEXTURE_* bindings are
// defined by the renderer from Graphics/Api/RenderContract.cppm; this file must not spell one of
// those numbers.
//
// Why this exists beside PbrFragmentShader rather than instead of it: an imported track's materials
// were tuned by somebody looking at this formula, and metalness-roughness has two numbers this
// model does not carry. Roughness can be derived from the exponent; metalness cannot be derived
// from anything, so converting means inferring it, and an inferred metal loses its diffuse colour
// entirely and reads as black. A surface authored as "0.3 of the ambient, 0.2 of the sun, a faint
// highlight at exponent 16" is fully described by those four numbers and is drawn here from them.
//
// **What this is not**: a re-implementation of the source engine's frame. The material's response
// is the imported one; the light it responds to is this renderer's — this scene's sun, this scene's
// cascades and this scene's light probes, in scene-linear radiance with the exposure meter and the
// tone curve downstream exactly as for every other surface in the frame.
//
// **Which is exactly why the ambient and diffuse coefficients arrive relative rather than
// absolute.** In the source model they multiply that engine's ambient constant and that engine's
// sun, and neither of those is in the asset — the track states the sun's *angles* and nothing about
// its magnitude, because the weather supplies both. Used raw they assert that two rigs' lighting
// agrees in absolute terms. It does not: measured on this circuit, using them raw put the ambient
// at 0.129 of the direct term where the metalness-roughness path beside it sits at 0.269, so the
// circuit's own shade came out half as bright as the car's standing in it, and the whole surface
// 0.84 stops down — which the exposure meter then paid for by opening and taking the sky with it.
//
// So the importer divides both by the asset's own *ordinary* material (its median), and 1.0 here
// means "an ordinary surface taking the ordinary amount of light". This shader places that against
// what this renderer measures: the light probes for ambient, and Lambert's `1/pi` for diffuse. A
// material at 1.0 then reflects exactly what a white dielectric does in PbrFragmentShader — the two
// models stand next to each other in one frame without either dragging the meter — and every
// material that departs from ordinary keeps the departure it was authored with, in both channels.
// Bathurst's ordinary material is ambient 0.3 / diffuse 0.2, which is 75.6% and 67.3% of its
// triangles respectively, so this is anchored to three quarters of the circuit rather than to a
// number somebody liked.
//
// The **specular** coefficient is *not* anchored and stays the asset's own: there is no ordinary
// specular to be relative to, 64 of these 188 materials carrying no highlight at all. Nor is the
// highlight energy-normalised, because the source model does not normalise it either and the
// coefficients were chosen against that.
//
// The shadow lookup and the spherical-harmonic evaluation below are **the same functions as
// PbrFragmentShader's**, copied because this renderer compiles each shader from one source string
// and has no include resolver to share them through. They must not drift: a difference in the
// shadow bias would put the track's shadows a different distance from their casters than the car's,
// and a difference in the basis would be a rotation applied to this surface's indirect light alone.

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
    vec4 ambientAttenuation;   // xyz ambient, w attenuation
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
    mat4 shadowMatrices[SHADOW_CASCADES];
    vec4 shadowSplits;             // where each cascade ends, along the view axis
    vec4 shadowTexelWorldSize;     // world units one texel of each cascade covers
    vec4 shadowDepthScale;         // normalised depth per world unit along the light, per cascade
    ivec4 shadowParams;            // x = cascades in use (0 = shade lit), y = the light they follow
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
    // windscreen's business and not a road's. Zero rain is the dry scene, and everything wet in
    // this shader is one branch on it.
    vec4 timeRain;
} frame;

// Declared because the block has one std140 layout both stages must agree on. This model reflects
// the environment through no term, so nothing here reads it.
layout(set = SET_FRAME, binding = PROBE_SPECULAR_BINDING) uniform samplerCubeArray probeSpecular;

// textureTransform is a 3x3 UV transform in a mat4 slot: std140 pads a mat3's columns to 16
// bytes each, which the C++ glm::mat3 does not, so the ABI carries it as a mat4.
layout(set = SET_MATERIAL, binding = 0) uniform MaterialData {
    vec4 baseColour;
    vec4 roughMetal;       // x roughness, y metalness, z alpha cutoff (0 = no test)
    ivec4 useTextures;     // x diffuse, y normal, z specular, w emissive
    ivec4 useTextures2;    // x occlusion, y opaque, z this material states the block below
    mat4 textureTransform; // KHR_texture_transform, upper 3x3
    // x ambient, y diffuse, z specular, w specular exponent. Filled for every material: a material
    // that states none gets the neutral matte default, which is why nothing below branches on
    // whether it was stated.
    //
    // x and y are **relative** — 1.0 is "an ordinary surface taking the ordinary amount of light",
    // the importer having divided the asset's own coefficients by the asset's own ordinary
    // material. So this shader places them against what this renderer measures rather than against
    // what some other engine's lamps were set to. z is absolute; see Material.cppm.
    vec4 blinnPhong;
    // The blended-material feature: xyzw are the four detail layers' tiling, in repeats per unit of
    // the model's own space.
    vec4 detailTiling;
    // x the blend strength, y non-zero when this material states detail layers. Read y rather than
    // testing the samplers: every slot is written whatever the material carries, and an unstated
    // layer holds the 1x1 white dummy, which would blend as a layer of pure white.
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

// What the detail layers do to the base colour. The same function as PbrFragmentShader's, and it
// must not drift for the same reason the shadow lookup below must not: two surfaces of one blended
// material drawn through different shaders would otherwise stop matching at the seam between them.
//
// A weighted sum, each layer by its own channel of the mask, scaled by the blend strength, and the
// caller *multiplies* the result into the base colour — so the base carries the large-scale colour
// and the layers carry the texture, and a layer at 1.0 leaves the base alone. Colour only: a detail
// layer says what a surface is made of, not whether it is there, so coverage is left to the base
// map's alpha and the cutout it feeds.
//
// The mask is sampled in UV because *where* a surface changes belongs to that surface's own
// parametrisation; the layers are sampled on model-space xz because *what* it changes to must be
// continuous across meshes that share no UV layout at all — which is every road ever exported as
// thirty separate objects.
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

// Set 3: one comparison sampler per cascade, and beside them the occlusion this view gathered from
// its own geometry. A view with neither binds the renderer's fallbacks — an opaque white border and
// a 1x1 white image — so both are read unconditionally.
layout(set = SET_SHADOW, binding = SHADOW_MAP_BINDING) uniform sampler2DShadow shadowMaps[SHADOW_CASCADES];
layout(set = SET_SHADOW, binding = AO_MAP_BINDING) uniform sampler2D ambientOcclusionMap;

// The Lambertian normalisation, spelled the same way PbrFragmentShader spells it. Not a contract
// number — it is pi, and both files are entitled to say so.
const float M_PI = 3.141592653;
const float INVERSE_PI = 1.0 / M_PI;

// One cascade's percentage-closer average.
float shadowInCascade(int cascade, vec3 coordinate)
{
    float result = 1.0;

    // An array of samplers may only be indexed by a dynamically uniform expression, and a cascade
    // chosen from a fragment's own view depth is not one — fragments of a single triangle straddle
    // a split. The loop counter is dynamically uniform because its bounds are compile-time
    // constants, so this unrolls into a chain of constant-index branches.
    for (int index = 0; index < SHADOW_CASCADES; index++)
    {
        if (index != cascade)
        {
            continue;
        }

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
    // a slope without detaching the contact shadow.
    vec4 lightSpace = frame.shadowMatrices[cascade]
        * vec4(worldPosition + worldNormal * (texelWorld * float(SHADOW_NORMAL_OFFSET_TEXELS) * sinTheta), 1.0);
    vec3 coordinate = lightSpace.xyz / lightSpace.w;

    // Past the cascade's far plane nothing was stored to compare against. Outside it laterally needs
    // no test: the sampler clamps to an opaque white border, which compares as lit.
    if (coordinate.z >= 1.0)
    {
        return 1.0;
    }

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

// The basis, evaluated. The same nine functions in the same order as the engine's C++ projection
// (Graphics/Api/SphericalHarmonics.cppm) and as PbrFragmentShader's copy.
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
    // zero. Negative light is not a thing.
    return max(result, vec3(0.0));
}

// How much of this fragment belongs to probe `index`. The ramp is per axis and the three are
// multiplied, so a fragment near a corner falls off in both directions at once.
float probeWeight(int index, vec3 worldPosition)
{
    if (frame.probes[index].boxMax.w != 0.0)
    {
        // The global probe has no bound. Its weight is what the local probes left over.
        return 1.0;
    }

    vec3 insideDistance = min(worldPosition - frame.probes[index].boxMin.xyz,
                              frame.probes[index].boxMax.xyz - worldPosition);
    vec3 ramp = clamp(insideDistance / max(frame.probes[index].boxMin.w, 0.0001), 0.0, 1.0);

    return ramp.x * ramp.y * ramp.z;
}

// The indirect diffuse light arriving here, for unit albedo: the scene's probes blended by how far
// inside each one's volume this fragment is, with whatever they do not account for falling to the
// global probe. The specular half of image-based lighting has no term in this model and is not
// gathered — an exponent is not a roughness, so there is no level of a prefiltered chain to read.
vec3 indirectDiffuse(vec3 worldNormal)
{
    vec4 blended[SH_COEFFICIENTS];
    for (int coefficient = 0; coefficient < SH_COEFFICIENTS; coefficient++)
    {
        blended[coefficient] = vec4(0.0);
    }

    float localWeight = 0.0;

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
            blended[coefficient] += frame.probes[index].irradiance[coefficient] * weight;
        }
    }

    // Where volumes overlap the weights can exceed one; scaling back down is what makes an overlap
    // a cross-fade rather than a bright seam.
    if (localWeight > 1.0)
    {
        for (int coefficient = 0; coefficient < SH_COEFFICIENTS; coefficient++)
        {
            blended[coefficient] /= localWeight;
        }
        localWeight = 1.0;
    }

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
                blended[coefficient] += frame.probes[index].irradiance[coefficient] * remainingWeight;
            }

            break;
        }
    }

    return evaluateIrradiance(blended, worldNormal);
}

// -- Wet surfaces ---------------------------------------------------------------------------------
//
// Stage one of rain in the world (docs/world-rain-brief.md): a wet road is darker, glossier and
// mirror-like at grazing angles, and that single change does more for "it is raining" than any
// falling streak, because it is what the driver is looking at. The model is a thin film of water
// over the authored material — the body darkens, a water lobe with its own Fresnel joins the
// highlight, and the sky arrives along the reflected ray — with standing water only where the
// surface is nearly level, masked by world-position noise so the puddles are places and not a
// coat. Everything below is one branch on the scene's own rain, so a dry frame is bit-for-bit the
// shader that had no rain in it.

// An integer hash, the house rule for anything procedural: a function of the cell alone, never of
// a wall clock and never fract(sin(...)). Same shape as the windscreen's rainHash.
float wetHash(ivec2 cell)
{
    uint hashed = uint(cell.x) * 374761393u + uint(cell.y) * 668265263u;
    hashed = (hashed ^ (hashed >> 13u)) * 1274126177u;
    hashed = hashed ^ (hashed >> 16u);

    return float(hashed & 0x00FFFFFFu) / 16777215.0;
}

// Smoothed value noise over world-space cells. World space rather than UV because where water
// stands is a fact about the ground, and this track's UV layout restarts every few metres.
float wetNoise(vec2 position)
{
    vec2 cell = floor(position);
    vec2 f = position - cell;
    f = f * f * (3.0 - 2.0 * f);

    ivec2 corner = ivec2(cell);
    float a = wetHash(corner);
    float b = wetHash(corner + ivec2(1, 0));
    float c = wetHash(corner + ivec2(0, 1));
    float d = wetHash(corner + ivec2(1, 1));

    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// How much standing water this fragment carries, 0 to 1. Two gates multiplied: flatness — water
// runs off a cambered road, and Bathurst is cambered nearly everywhere, so puddles live on the few
// near-level patches — and a two-octave noise in world metres, thresholded lower as it rains
// harder, so light rain damps the road and heavy rain stands in it. A world unit is a tenth of a
// metre, so the 0.03 puts the noise's features at three-odd metres.
float puddleMask(vec3 worldPosition, vec3 worldUp, float wetness)
{
    float upness = clamp(worldUp.y, 0.0, 1.0);
    float level = smoothstep(0.995, 0.9995, upness);
    if (level <= 0.0)
    {
        return 0.0;
    }

    vec2 p = worldPosition.xz * 0.03;
    float shape = wetNoise(p) * 0.65 + wetNoise(p * 3.1) * 0.35;
    float threshold = mix(0.78, 0.55, wetness);

    return level * smoothstep(threshold, threshold + 0.10, shape);
}

void main()
{
    vec2 transformedTextureCoordinates = (mat3(material.textureTransform) * vec3(textureCoordinates, 1.0)).xy;

    vec4 sampledBaseColour =
        (material.useTextures.x != 0) ? texture(diffuseTexture, transformedTextureCoordinates) : vec4(1.0);
    vec4 albedo = material.baseColour * sampledBaseColour;
    albedo.rgb *= detailModulation(transformedTextureCoordinates);

    // glTF MASK: under the cutoff there is no surface here at all. Discarded before any lighting is
    // spent on it, and *not* blended — a cut-out is opaque geometry with holes, which is what lets a
    // tree card write depth and sort like the trunk it stands in for. The same test the depth-only
    // and prepass stages make, so a leaf's hole is a hole in its shadow and in its occlusion too.
    if (material.roughMetal.z > 0.0 && albedo.a < material.roughMetal.z)
    {
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


    float ambientLevel = material.blinnPhong.x;
    float diffuseLevel = material.blinnPhong.y;
    float specularLevel = material.blinnPhong.z;
    float specularExponent = material.blinnPhong.w;

    // The specular map, and it is read only where the material states this model. In that case the
    // slot holds a map authored for it — red the specular multiplier, green the exponent's — which
    // is not what the same slot means to the metalness-roughness path, where green is roughness and
    // red is occlusion. Reading it unconditionally would take a converted ORM texture and multiply
    // this surface's highlight by its occlusion channel, which is a plausible-looking picture of the
    // wrong thing. A material with no map reads the identity and is left exactly as authored.
    if (material.useTextures2.z != 0 && material.useTextures.z != 0)
    {
        vec4 maps = texture(specularTexture, transformedTextureCoordinates);
        specularLevel *= maps.r;
        specularExponent = max(1.0, specularExponent * maps.g);
    }

    // The rain's wetness, one branch on the scene's own statement so a dry frame is bit-for-bit
    // the shader that had no rain in it. The albedo darkens by nearly half at full wetness — a wet
    // porous surface loses roughly that much diffuse reflectance to internal reflection under the
    // film — and a puddle darkens further still, because what it shows instead is the sky, added as
    // reflection below rather than as body.
    //
    // Opaque surfaces only, and the tyre marks are why. The scene pass blends premultiplied and
    // coverage scales the body, not the highlight — so a blended decal at thirty percent coverage
    // was adding the water lobe and the sky reflection at *full* strength and glowing. A blended
    // surface is a film over ground that is already wet underneath; the wet tarmac shows through
    // it, and that is the whole of what it should say about the rain. Mask cutouts are opaque and
    // stay wet.
    float wetness = 0.0;
    float puddle = 0.0;
    if (frame.timeRain.y > 0.0 && material.useTextures2.y != 0)
    {
        wetness = clamp(frame.timeRain.y, 0.0, 1.0);
        puddle = puddleMask(positionInWorldSpace, normalize(normalsInWorldSpace), wetness);
        albedo.rgb *= (1.0 - 0.45 * wetness) * (1.0 - 0.35 * puddle);
    }

    // Tangent space, for the direct term: the vertex stage hands the light and view directions over
    // already rotated into it, so the normal map is used raw and no basis is rebuilt here.
    vec3 V = normalize(viewDirectionWorldSpace);
    vec3 N = (material.useTextures.y != 0)
        ? normalize(texture(normalTexture, transformedTextureCoordinates).xyz * 2.0 - 1.0)
        : vec3(0.0, 0.0, 1.0);

    // Standing water is its own surface: level, so the texture's normal gives way to the pane of
    // the water as the puddle deepens. The geometric camber stays — the basis below still tilts
    // with the road — which is why the flatness gate above only admits near-level ground.
    if (puddle > 0.0)
    {
        N = normalize(mix(N, vec3(0.0, 0.0, 1.0), puddle));
    }

    // The other half of a double-sided material. A fragment seen from behind is the surface's other
    // side, and every consumer of its normal has to agree — a foliage card lit through its back
    // otherwise takes the sun on whichever side the exporter happened to wind.
    if (!gl_FrontFacing)
    {
        N = vec3(N.xy, -N.z);
    }

    // Once, outside the loop: one light casts the cascades. The geometric world normal, not the
    // normal-mapped one — the bias is a property of the surface the depth map recorded, not of its
    // texture. Lights carry the direction *towards* the light in `position`.
    int shadowLight = clamp(frame.shadowParams.y, 0, MAX_LIGHTS - 1);
    vec3 geometricNormal = normalize(gl_FrontFacing ? normalsInWorldSpace : -normalsInWorldSpace);
    float shadow = shadowFactor(positionInWorldSpace, geometricNormal,
                                normalize(frame.lights[shadowLight].position.xyz), -positionInViewSpace.z);

    vec3 diffuseLight = vec3(0.0);
    vec3 specularLight = vec3(0.0);

    for (int lightIndex = 0; lightIndex < frame.lightCount.x; lightIndex++)
    {
        vec3 L = normalize(lightDirectionWorldSpace[lightIndex]);
        vec3 H = normalize(L + V);

        float NdL = max(0.0, dot(N, L));
        float NdH = max(0.0, dot(N, H));

        float occlusionFromShadow = (lightIndex == shadowLight) ? shadow : 1.0;
        float attenuation = frame.lights[lightIndex].ambientAttenuation.w * occlusionFromShadow;

        // Lambert, scaled by how far this material departs from an ordinary one. The 1/pi is what
        // makes "ordinary" mean the same thing here as it does in PbrFragmentShader, so a surface
        // at 1.0 reflects exactly what a white dielectric there does and the two models can stand
        // next to each other in one frame without either dragging the exposure meter.
        diffuseLight += frame.lights[lightIndex].diffuse.rgb * (diffuseLevel * INVERSE_PI * NdL * attenuation);

        // Gated on NdL rather than on NdH alone: the half-vector still rises towards one as a light
        // slides behind a surface, so an ungated highlight lights the dark side of every kerb. The
        // light's own specular colour, which is the one thing in this frame that reads it — the
        // metalness-roughness path takes its highlight from the diffuse colour and a Fresnel term.
        if (NdL > 0.0)
        {
            specularLight += frame.lights[lightIndex].specular.rgb
                * (specularLevel * pow(NdH, specularExponent) * attenuation);
        }

        // The water film's own lobe, on top of the material's: water is a dielectric with its own
        // Fresnel, so the glint is faint face-on and fierce at grazing — which is the long streak of
        // sun a wet road throws at a low morning light. Normalised, (n+8)/8pi, so the puddle's much
        // tighter exponent brightens the peak instead of starving it. One branch on the scene's
        // rain; a dry frame never reaches it.
        if (wetness > 0.0 && NdL > 0.0)
        {
            float VdH = max(dot(V, H), 0.0);
            float fresnel = 0.02 + 0.98 * pow(1.0 - VdH, 5.0);
            float wetExponent = mix(160.0, 1800.0, puddle);
            float wetStrength = wetness * mix(0.30, 1.0, puddle);

            specularLight += frame.lights[lightIndex].specular.rgb
                * (wetStrength * fresnel * (wetExponent + 8.0) * 0.125 * INVERSE_PI * pow(NdH, wetExponent)
                   * attenuation);
        }
    }

    // World space, for the indirect term: a probe is a fact about a place, so everything read from
    // one has to be asked for in the coordinates the place is described in. The view rotation is
    // orthonormal, so its transpose is its inverse and no matrix is inverted per fragment.
    mat3 viewToWorld = transpose(mat3(frame.viewMatrix));
    vec3 shadingNormalViewSpace = normalize((tangentInNormalSpace * N.x) +
                                            (bitangentInNormalSpace * N.y) +
                                            (normalsInNormalSpace * N.z));
    vec3 worldNormal = normalize(viewToWorld * shadingNormalViewSpace);

    // The screen-space occlusion, on the indirect term alone. It stays out of the direct one on
    // purpose: the sun either reaches a surface or is stopped by something the shadow map already
    // knows about, and darkening it here would be the same occluder counted twice. A transparent
    // surface reads none of it — the buffer holds one opaque surface per pixel and that surface is
    // whatever is behind this glass.
    float screenOcclusion = material.useTextures2.y != 0
        ? texture(ambientOcclusionMap, gl_FragCoord.xy / vec2(textureSize(ambientOcclusionMap, 0))).r
        : 1.0;

    vec3 ambientLight = indirectDiffuse(worldNormal) * (ambientLevel * screenOcclusion);

    // The sky in the road, which is most of what "wet" looks like: the probes' irradiance read
    // along the reflected ray instead of the normal — a blurry sky, which is exactly what rough wet
    // asphalt reflects — weighted by water's Fresnel, so it lives at grazing angles and vanishes
    // looking straight down. A puddle reflects at full strength; damp tarmac at a third. The
    // screen-space occlusion rides along so a reflection cannot appear under a car that blocks the
    // sky it would be reflecting. Sharper mirror puddles want the prefiltered specular chain this
    // model deliberately does not gather; if the seat asks for them, that is the next reach.
    if (wetness > 0.0)
    {
        vec3 incident = normalize(positionInWorldSpace - frame.cameraPosition.xyz);
        vec3 reflected = reflect(incident, worldNormal);
        float NdV = max(dot(worldNormal, -incident), 0.0);
        float fresnelSky = 0.02 + 0.98 * pow(1.0 - NdV, 5.0);
        float wetMirror = wetness * mix(0.35, 1.0, puddle);

        specularLight += indirectDiffuse(reflected) * (fresnelSky * wetMirror * screenOcclusion);
    }

    // Premultiplied, and the split is the whole point of it: coverage scales the body of the surface
    // and not the highlight. A pane of glass transmits most of what is behind it and still catches
    // the sun at full strength — the highlight is light the surface adds, not light it fails to
    // block. At alpha 1 this is the plain sum. The blend state is (ONE, ONE_MINUS_SRC_ALPHA) to
    // match, which is the same contract PbrFragmentShader writes under.
    vec3 body = albedo.rgb * (ambientLight + diffuseLight);

    vec3 shaded = body * albedo.a + specularLight;

    // The medium moved to the fullscreen fog pass for everything opaque (2026-08-25): an opaque
    // surface is in the occlusion prepass, so the pass places it exactly and integrates the air
    // once per pixel instead of once per fragment with overdraw. A blended surface is not in that
    // prepass — there is no single depth for a pane you can see through — so the blended draws
    // keep the in-shader statement over their own exact positions, composited over a world the
    // pass has already fogged behind them. Probe captures upload no fog at all, so this gate
    // changes nothing a probe photographs.
    if (material.useTextures2.y == 0)
    {
        shaded = applyFog(shaded, positionInWorldSpace, albedo.a);
    }

    fragColor = vec4(shaded, albedo.a);
}
