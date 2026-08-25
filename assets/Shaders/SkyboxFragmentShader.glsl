#version 450

#define PI 3.141592
#define iSteps 16
#define jSteps 8

layout(location = 0) out vec4 fragColor;

layout(location = 0) in vec3 textureCoordinates;

struct Light {
    vec4 position;             // xyz the direction *towards* the light
    vec4 diffuse;
    vec4 specular;
    vec4 ambientAttenuation;
};

// A prefix of the frame block: everything up to the field this shader reads, laid out as the ABI
// states it. What it needs is which light casts the cascades and where that light is, because the
// sun in the sky and the sun the world is lit by have to be the same sun — they were not, and the
// scene rendered a dusk sky over midday lighting.
//
// This is also what makes a light probe's "captured at runtime to follow the time of day" mean
// anything: move the light and the sky moves with it, so a re-captured probe records a genuinely
// different environment rather than the same one under a different direct term.
// One light probe, declared here because the fog reads the global one's mean radiance for the light
// it scatters in from everywhere that is not the sun. Same layout as the scene shaders' copy.
struct Probe {
    vec4 irradiance[SH_COEFFICIENTS];
    vec4 boxMin;
    vec4 boxMax;               // w non-zero for the scene's global probe
    vec4 position;
};

layout(set = SET_FRAME, binding = 0) uniform FrameData {
    mat4 viewMatrix;
    vec4 cameraPosition;
    ivec4 lightCount;
    Light lights[MAX_LIGHTS];
    mat4 shadowMatrices[SHADOW_CASCADES];
    vec4 shadowSplits;
    vec4 shadowTexelWorldSize;
    vec4 shadowDepthScale;
    ivec4 shadowParams;
    // The block goes this far now rather than stopping at shadowParams: the fog fields are appended
    // *after* the probes, and a block may be a prefix of its buffer but not a subset of it, so
    // reaching the fog means declaring what stands in front of it. Nothing between here and there is
    // read by this shader except probes[].irradiance[0], which is the haze's colour.
    ivec4 probeParams;
    Probe probes[MAX_IBL_PROBES];
    vec4 fogDensity;           // x extinction at the reference height, y 1/scale height,
                               // z that reference height, w how far the medium is integrated
    vec4 fogScatter;           // xyz single-scatter albedo, w Henyey-Greenstein's asymmetry
    vec4 fogAmbient;           // xyz a tint on the ambient half, w a gain on the sun's
} frame;

// The cascades, for the marched half of the fog. The sky is where a shaft is most visible — there is
// nothing else out there for the eye to measure it against — so this shader marches the medium
// exactly as the two surface shaders do. Every scene pipeline shares one layout, so this set is
// already bound for the pass; a view with no cascades binds the renderer's fallback and
// frame.shadowParams.x is what says not to read it.
layout(set = SET_SHADOW, binding = SHADOW_MAP_BINDING) uniform sampler2DShadow shadowMaps[SHADOW_CASCADES];

// The sun as it is actually seen: a disc a little over half a degree across, darkening towards its
// edge, subtending the solid angle that turns the scene light's irradiance into a radiance.
const float sunAngularRadius = 0.00465;   // radians; 0.266 degrees
const float sunSolidAngle = 6.794e-5;     // steradians; 2*pi*(1 - cos(radius))
const float sunLimbDarkening = 0.6;
// The aureole: the bright ring of forward-scattered light immediately around the disc, which is a
// real and very steep feature of a hazy sky and which the sixteen-step integral above cannot
// resolve — its Mie term is a glow tens of degrees wide. Stated as a fraction of the disc's own
// radiance at the disc's edge, falling as the inverse square of the angle from there, so it is a
// hundredth of that a decade out and gone into the sky by five degrees.
//
// It is also what stops the sun reading as a *square*. Bloom is the only thing spreading a disc
// twelve pixels across, its chain starts at half resolution, and a point source that lives in one
// level of a chain comes back out shaped like that level's kernel. The aureole gives the spill a
// radial, resolution-independent core to start from and leaves bloom the wide halo it is good at.
const float sunAureoleAtEdge = 0.02;
// Half floats stop at 65504 and the bloom chain has to sum this without reaching there.
const float sunMaximumRadiance = 4000.0;


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
// **This block is the same in PbrFragmentShader, BlinnPhongFragmentShader and SkyboxFragmentShader
// and must not drift between them**: this engine compiles each shader from one source string and
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

// The medium applied to the sky.
//
// The sky is not at a distance — it is the direction there is no surface in — so it is fogged as
// though it stood at the medium's own maximum reach. That is what the number is for: the height
// profile already thins a ray that climbs, so what this bounds is the horizon, where a level ray
// would otherwise integrate to fully opaque against a sky box thirty thousand units out.
//
// The solar disc is fogged with everything else, and deliberately: a sun seen through a kilometre of
// haze is dimmed and reddened by it, which is most of what a low sun looks like.
vec3 applyFog(vec3 colour, vec3 rayDirection)
{
    if (frame.fogDensity.x <= 0.0)
    {
        return colour;
    }

    vec4 fog = fogAlongRay(rayDirection, frame.fogDensity.w);

    return colour * fog.a + fog.rgb;
}

vec2 rsi(vec3 r0, vec3 rd, float sr) {
    // ray-sphere intersection that assumes
    // the sphere is centered at the origin.
    // No intersection when result.x > result.y
    float a = dot(rd, rd);
    float b = 2.0 * dot(rd, r0);
    float c = dot(r0, r0) - (sr * sr);
    float d = (b*b) - 4.0*a*c;
    if (d < 0.0) return vec2(1e5,-1e5);
    return vec2(
        (-b - sqrt(d))/(2.0*a),
        (-b + sqrt(d))/(2.0*a)
    );
}

vec3 atmosphere(vec3 r, vec3 r0, vec3 pSun, float iSun, float rPlanet, float rAtmos, vec3 kRlh, float kMie, float shRlh, float shMie, float g) {
    // Normalize the sun and view directions.
    pSun = normalize(pSun);
    r = normalize(r);

    // Calculate the step size of the primary ray.
    vec2 p = rsi(r0, r, rAtmos);
    if (p.x > p.y) return vec3(0,0,0);
    p.y = min(p.y, rsi(r0, r, rPlanet).x);
    float iStepSize = (p.y - p.x) / float(iSteps);

    // Initialize the primary ray time.
    float iTime = 0.0;

    // Initialize accumulators for Rayleigh and Mie scattering.
    vec3 totalRlh = vec3(0,0,0);
    vec3 totalMie = vec3(0,0,0);

    // Initialize optical depth accumulators for the primary ray.
    float iOdRlh = 0.0;
    float iOdMie = 0.0;

    // Calculate the Rayleigh and Mie phases.
    float mu = dot(r, pSun);
    float mumu = mu * mu;
    float gg = g * g;
    float pRlh = 3.0 / (16.0 * PI) * (1.0 + mumu);
    float pMie = 3.0 / (8.0 * PI) * ((1.0 - gg) * (mumu + 1.0)) / (pow(1.0 + gg - 2.0 * mu * g, 1.5) * (2.0 + gg));

    // Sample the primary ray.
    for (int i = 0; i < iSteps; i++) {

        // Calculate the primary ray sample position.
        vec3 iPos = r0 + r * (iTime + iStepSize * 0.5);

        // Calculate the height of the sample.
        float iHeight = length(iPos) - rPlanet;

        // Calculate the optical depth of the Rayleigh and Mie scattering for this step.
        float odStepRlh = exp(-iHeight / shRlh) * iStepSize;
        float odStepMie = exp(-iHeight / shMie) * iStepSize;

        // Accumulate optical depth.
        iOdRlh += odStepRlh;
        iOdMie += odStepMie;

        // Calculate the step size of the secondary ray.
        float jStepSize = rsi(iPos, pSun, rAtmos).y / float(jSteps);

        // Initialize the secondary ray time.
        float jTime = 0.0;

        // Initialize optical depth accumulators for the secondary ray.
        float jOdRlh = 0.0;
        float jOdMie = 0.0;

        // Sample the secondary ray.
        for (int j = 0; j < jSteps; j++) {

            // Calculate the secondary ray sample position.
            vec3 jPos = iPos + pSun * (jTime + jStepSize * 0.5);

            // Calculate the height of the sample.
            float jHeight = length(jPos) - rPlanet;

            // Accumulate the optical depth.
            jOdRlh += exp(-jHeight / shRlh) * jStepSize;
            jOdMie += exp(-jHeight / shMie) * jStepSize;

            // Increment the secondary ray time.
            jTime += jStepSize;
        }

        // Calculate attenuation.
        vec3 attn = exp(-(kMie * (iOdMie + jOdMie) + kRlh * (iOdRlh + jOdRlh)));

        // Accumulate scattering.
        totalRlh += odStepRlh * attn;
        totalMie += odStepMie * attn;

        // Increment the primary ray time.
        iTime += iStepSize;

    }

    // Calculate and return the final color.
    return iSun * (pRlh * kRlh * totalRlh + pMie * kMie * totalMie);
}

void main()
{
    // The ray is the skybox cube's own model-space position, and nothing rotates that node, so it
    // is already the world direction this fragment looks along. It used to have its y negated,
    // which rendered the whole atmosphere upside down: the scattering model's "up" is +y, so a
    // flipped ray put the horizon glow below the horizon and left no sun direction that could
    // light the sky and agree with the scene's sun at the same time. Unflipped, the two are the
    // same vector and the sky follows the light.
    int shadowLight = clamp(frame.shadowParams.y, 0, MAX_LIGHTS - 1);
    vec3 sunDirection = normalize(frame.lights[shadowLight].position.xyz);

    vec3 color = atmosphere(
        normalize(textureCoordinates),  // normalized ray direction
        vec3(0, 6372e3, 0),             // ray origin
        sunDirection,                   // position of the sun
        22.0,                           // intensity of the sun
        6371e3,                         // radius of the planet in meters
        6471e3,                         // radius of the atmosphere in meters
        vec3(5.5e-6, 13.0e-6, 22.4e-6), // Rayleigh scattering coefficient
        21e-6,                          // Mie scattering coefficient
        8e3,                            // Rayleigh scale height
        1.2e3,                          // Mie scale height
        0.758                           // Mie preferred scattering direction
    );

    // The solar disc, which the scattering integral above does not draw: what it produces around
    // the sun is the *glow*, the Mie phase function peaking as the ray turns towards it, and the
    // disc itself is a hole in that model. Without one there is nothing in the sky brighter than
    // the sky, which is what makes a bright afternoon read as an overcast one.
    //
    // Its radiance is the one the scene's own sun implies rather than a number chosen to look
    // right: a directional light of irradiance E arriving from a disc of solid angle
    // `sunSolidAngle` is a disc of radiance E / solid angle, so the sun you see and the sun the
    // world is lit by are the same sun stated twice. Capped, because the frame is stored in half
    // floats and the physical figure has four more digits than that leaves room for; what the cap
    // costs is the ratio between the disc and the sky, which no display can show anyway.
    vec3 ray = normalize(textureCoordinates);
    float angle = acos(clamp(dot(ray, sunDirection), -1.0, 1.0));

    // **Capped by scaling rather than by clamping, so the cap cannot change the sun's colour.**
    // `min()` per channel is a hue shift wearing a range limit: every channel over the ceiling comes
    // back at exactly the ceiling, so any sun bright enough to clip prints pure white whatever colour
    // it actually is. That was invisible while the sun was midday white and became the whole problem
    // at sunrise — a deeply reddened sun, four orders of magnitude over the ceiling in every channel,
    // drawn as a white disc against a red sky. Scaling the vector to bring its brightest channel to
    // the ceiling keeps the ratios, so what is lost is the disc's absolute radiance — which no
    // display can show anyway — and not its colour.
    vec3 sunRadiance = frame.lights[shadowLight].diffuse.rgb / sunSolidAngle;
    float sunPeak = max(max(sunRadiance.r, sunRadiance.g), sunRadiance.b);
    if (sunPeak > sunMaximumRadiance)
    {
        sunRadiance *= sunMaximumRadiance / sunPeak;
    }

    // The one view the disc must not appear in. A light probe records the world so that a surface
    // can be given the light it cannot see directly; the sun is not that light, it is the direct
    // term, and a disc in the cube would deliver it a second time — about 9% of the sun's
    // irradiance in the mean, and anywhere between none of it and three times that in a given
    // capture, since the disc is smaller than one texel of a 128-pixel face. The aureole stays:
    // it is scattered light, which is exactly what a probe is for.
    const bool probeCapture = frame.shadowParams.z != 0;

    if (angle >= sunAngularRadius)
    {
        const float falloff = sunAngularRadius / angle;

        color += sunRadiance * sunAureoleAtEdge * falloff * falloff;
    }
    else if (probeCapture)
    {
        // Continued flat across the disc at its edge value rather than left as a hole, so that
        // nothing in the cube depends on where the sun fell in the texel grid.
        color += sunRadiance * sunAureoleAtEdge;
    }
    else
    {
        // Limb darkening: the disc is not uniform, because a ray leaving its edge travels further
        // through the photosphere than one leaving its centre. The edge lands about a third down,
        // which is what stops the disc reading as a sticker.
        float edge = angle / sunAngularRadius;
        float limb = 1.0 - sunLimbDarkening * (1.0 - sqrt(max(1.0 - edge * edge, 0.0)));

        color += sunRadiance * limb;
    }

    // No tone map here. This shader used to end with `1 - exp(-color)`, which is a display transfer
    // applied inside the sky: it clamped the whole atmosphere to one, so the sun's peak, the
    // horizon and a mid-sky blue all arrived at the post chain within a factor of two of each
    // other. Everything downstream — the exposure meter, the bloom threshold, the light probes —
    // reads scene-referred radiance, and the sky is the brightest thing in the scene.
    // The medium, last: everything above it is light arriving from beyond the fog, and this is
    // the kilometre of air it arrives through.
    fragColor = vec4(applyFog(color, ray), 1.0f);
}