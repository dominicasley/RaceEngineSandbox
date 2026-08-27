#version 450
// The volumetric fog's march: the whole integration, at half resolution (2026-08-26).
//
// The split from the apply pass (VolumetricFogFragmentShader, which now only lifts and lays) is
// the same one the occlusion chain made and for the same reason: the medium is low-frequency and
// the march was the expensive half of the frame's most expensive region. This pass reads only the
// occlusion prepass — depth in its alpha — and writes (in-scattered radiance, transmittance) as
// data, which is why its pass runs with blending OFF: the alpha here is the transmittance the
// apply multiplies the world by, not a coverage for a blend to consume.
//
// The arithmetic below is the same block the scene shaders carry and must not drift from them;
// the account of the two halves and the telescoping quadrature is theirs and
// docs/volumetric-fog-brief.md's. `Graphics/Api/VolumetricFog.cppm` is the same arithmetic in
// C++, unit-tested without a device.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// Set 0: per camera pass — the frame block, declared as far as the fog reads.
struct Light {
    vec4 position;             // xyz position
    vec4 diffuse;
    vec4 specular;
    vec4 ambientAttenuation;   // xyz ambient, w attenuation
};

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
    vec4 fogDensity;               // x extinction at the reference height, y 1/scale height,
                                   // z that reference height, w how far the medium is integrated
    vec4 fogScatter;               // xyz single-scatter albedo, w Henyey-Greenstein's asymmetry
    vec4 fogAmbient;               // xyz a tint on the ambient half, w a gain on the sun's
} frame;

// The cascades, at the slot the fullscreen layout has for the shadow set.
layout(set = SET_FULLSCREEN_SHADOW, binding = SHADOW_MAP_BINDING)
    uniform sampler2DShadow shadowMaps[SHADOW_CASCADES];

// The pass's one input: element 0 the occlusion prepass (view-space normal in rgb, view depth in
// a). The array is declared whole because the set carries it whole.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

layout(push_constant) uniform PassParameters {
    vec4 tone;      // unread here: the fog works in radiance, long before the tone map
    vec4 pass;      // x target level, y target levels, zw target size
    vec4 view;      // x tan(fovX/2), y tan(fovY/2), z near, w far
    vec4 effect;    // unread here: the medium is the scene's statement, not a pass parameter
    vec4 viewRight; // xyz the view's +x in world space, w the camera's world x
    vec4 viewUp;    // xyz the view's +y in world space, w the camera's world y
    vec4 viewBack;  // xyz the view's +z in world space (the view looks down -z), w the world z
    vec4 weather;   // unread here
} params;

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

void main()
{
    // Zero density is the scene that states no fog. The pass is only built when the scene states
    // one, so this is belt beside braces — but it is also the branch that keeps a mid-session
    // density of zero honest: no in-scatter, full transmittance, and the apply passes the world
    // through untouched.
    if (frame.fogDensity.x <= 0.0)
    {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);

        return;
    }

    // The pixel's ray, reconstructed exactly as the world-rain pass reconstructs it. The prepass's
    // alpha is the view depth of the nearest opaque surface; zero is the sky, which the scene
    // shaders fogged at whatever distance the sky box stood — always past `maximumDistance`, where
    // fogAlongRay clamps the reach — so integrating the sky at exactly that distance is the same
    // statement.
    vec2 ndc = vec2(textureCoordinates.x * 2.0 - 1.0, 1.0 - textureCoordinates.y * 2.0);
    vec3 dirView = vec3(ndc * params.view.xy, -1.0);
    vec3 dirWorld = params.viewRight.xyz * dirView.x + params.viewUp.xyz * dirView.y + params.viewBack.xyz * dirView.z;
    vec3 cameraPosition = vec3(params.viewRight.w, params.viewUp.w, params.viewBack.w);

    float viewDepth = texture(inputs[0], textureCoordinates).a;
    vec3 toFragment = viewDepth > 0.0 ? dirWorld * viewDepth : normalize(dirWorld) * frame.fogDensity.w;

    float rayLength = length(toFragment);
    if (rayLength <= 0.0)
    {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);

        return;
    }

    fragColor = fogAlongRay(toFragment / rayLength, rayLength);
}
