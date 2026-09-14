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
    vec4 boxMax;               // w the fade in [0,1], negative for the global probe
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
    // The prefix runs to the block's last field now: the clouds ride cloudParams, and reaching it
    // means declaring everything in front of it, field for field against FrameDataUbo. Nothing
    // between here and there is read by this shader.
    vec4 timeRain;
    vec4 rainWind;
    vec4 wiperArcA;
    vec4 wiperArcB;
    vec4 wiperSweep;
    vec4 wiperTiming;
    vec4 wiperPane;
    vec4 rainBody;
    vec4 cloudParams;          // x effective coverage, y stratus-to-cumulus type,
                               // z the eye's sky gain (one in a probe capture), w reserved
    // Three of the mirror's, declared only to reach what stands after them: a block may be a prefix
    // of its buffer and never a subset of it, so the sky's own fields cannot be read without them.
    vec4 mirrorParams;
    vec4 mirrorAxis;
    vec4 mirrorUp;
    vec4 skyParams;            // x the integral's solar intensity scale — ONE for the sun, 1/398107
                               // for the full moon; y the irradiance of a magnitude-zero star, zero
                               // by day and in every probe capture; z and w reserved
} frame;

// The cloud dome map, beside the cascades on the scene shadow set: rgb the clouds' in-scattered
// radiance, a their transmittance, written by the dome pass at the end of the world camera's
// chain and read here one frame later. A scene with no clouds binds the 1x1 white dummy, which is
// why every read below sits behind the coverage branch. Probe faces bind the same map, which is
// how the clouded sky reaches the captures and becomes the world's ambient light.
layout(set = SET_SHADOW, binding = CLOUD_MAP_BINDING) uniform sampler2D cloudMap;

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


// The volumetric fog left this shader on 2026-08-25: the sky is opaque, so the fullscreen fog
// pass places it — a prepass depth of zero reads as the medium's own maximum reach, which is
// exactly the distance the block that stood here integrated — and a probe capture uploads no
// fog at all, so nothing a probe photographs changed. The solar disc is still fogged with
// everything else, one pass later. The block lives on in BlinnPhongFragmentShader,
// PbrFragmentShader (for their blended draws) and VolumetricFogFragmentShader (for everything
// opaque), and those three must not drift.

// Where a world direction lands on the cloud dome map. u is azimuth over the FULL 360 degrees —
// zero looking along +z, the game's own yaw convention — and v is elevation from -10 to +90
// degrees; the u seam wraps (the map's sampler repeats in u for exactly this line) and v clamps at
// the horizon rows, where the dome pass writes no cloud anyway.
//
// **This is the EXACT inverse of cloudDomeDirection in CloudDomeFragmentShader.glsl and the two
// MUST NOT DRIFT** — a disagreement moves every cloud in the sky by the difference, which reads as
// a projection bug nobody can localise.
vec2 cloudDomeUv(vec3 direction)
{
    float azimuth = atan(direction.x, direction.z);
    float elevation = asin(clamp(direction.y, -1.0, 1.0));
    float u = fract(azimuth * 0.15915494309);
    float v = clamp((degrees(elevation) + 10.0) / 100.0, 0.0, 1.0);

    return vec2(u, v);
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

// A grid cell to three numbers in [0,1): Hoskins' hash33, integer-free on purpose. The stars have
// to stand in the same place on every machine and in every capture, exactly as the cloud map does,
// and a hash that went through an integer bit pattern would be the one thing in this sky a driver
// could disagree about.
vec3 starHash(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yzz) * p.zyx);
}

// The stars, as radiance added to whatever the atmosphere left.
//
// **A star is a point source, so a pixel does not show its radiance** — that is a surface brightness
// no display has ever needed — **but its irradiance spread over the pixel's own solid angle**, which
// is why this takes the pixel from the ray's screen derivatives instead of drawing a dot of some
// chosen size. Drawn as a Gaussian about a pixel and a half across with its integral held at the
// star's irradiance, so the field neither crawls as the head turns nor changes brightness with the
// window: at twice the resolution each star lands on a quarter the solid angle and four times the
// radiance, which is the same star.
//
// `zeroMagnitude` is the irradiance of a magnitude-zero star in the engine's own units, derived in
// RenderRig.cppm against the sun's own apparent magnitude. Everything here is dimensionless beside
// it, which is what keeps the sky's one anchor on the CPU where the sun's is.
vec3 starField(vec3 ray, float zeroMagnitude)
{
    // The pixel, as a solid angle on the unit sphere: the cross product of the ray's two screen
    // derivatives is exactly the area one pixel covers there. **Taken before the density branch
    // below**, because a derivative inside non-uniform control flow is undefined and the branch is
    // per-pixel.
    float pixelSolidAngle = max(length(cross(dFdx(ray), dFdy(ray))), 1e-12);

    // The sphere cut into cells about a fifth of a degree across, a star in some of them. One cell
    // is enough rather than the usual twenty-seven: the star is jittered into the middle 60% of its
    // own cell, which leaves a margin some ten pixels wide against a spot that is one, so no star
    // ever reaches out of the cell that owns it.
    const float starCells = 180.0;
    // What fraction of cells hold one, for roughly six thousand over the whole sphere — the naked
    // eye's limit under a dark sky, which is also where the magnitude distribution below stops.
    const float starDensity = 0.0090;
    const float faintestMagnitude = 6.0;

    vec3 cell = floor(ray * starCells);
    vec3 h = starHash(cell);
    if (h.x >= starDensity)
    {
        return vec3(0.0);
    }

    vec3 jitter = starHash(cell + 17.0) - 0.5;
    vec3 starDirection = normalize(cell + 0.5 + jitter * 0.6);

    // The magnitude. Star counts grow about three-fold per magnitude, so a hash uniform in [0,1)
    // becomes a magnitude through a log base three: one star in a thousand comes out brighter than
    // zero, which is about what the real sky holds above the horizon.
    float u = max(h.x / starDensity, 1e-4);
    float magnitude = faintestMagnitude + log(u) / log(3.0);
    float irradiance = zeroMagnitude * pow(10.0, -0.4 * magnitude);

    float sigma = 0.62 * sqrt(pixelSolidAngle);
    float separation = acos(clamp(dot(ray, starDirection), -1.0, 1.0));
    float spot = exp(-0.5 * separation * separation / (sigma * sigma));

    // Colour: the main sequence runs from the blue-white of an A star to the orange of a K, and the
    // third hash lane picks a place along it. Both ends are normalised to the same luminance, so a
    // star's temperature moves its hue and never the magnitude just stated.
    const vec3 warmStar = vec3(1.232, 0.960, 0.717);
    const vec3 coolStar = vec3(0.894, 1.000, 1.319);

    return mix(warmStar, coolStar, h.z) * (irradiance * spot / (6.2831853 * sigma * sigma));
}

vec3 atmosphere(vec3 r, vec3 r0, vec3 pSun, float iSun, float rPlanet, float rAtmos, vec3 kRlh, float kMie, float shRlh, float shMie, float g, vec3 kOzn, float hOzn, float wOzn) {
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
    // Ozone absorbs and does not scatter, so its depth enters the attenuation and nothing else.
    float iOdOzn = 0.0;

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
        // The ozone layer is a tent: full density at its centre, zero a half-width above and below.
        float odStepOzn = max(1.0 - abs(iHeight - hOzn) / wOzn, 0.0) * iStepSize;

        // Accumulate optical depth.
        iOdRlh += odStepRlh;
        iOdMie += odStepMie;
        iOdOzn += odStepOzn;

        // **Whether this sample can see the body at all**, which is the whole difference between a
        // sky and a night sky. The secondary ray below integrates the air between the sample and
        // the sun and never asked whether the planet was in the way: with the sun under the
        // horizon every sample went on receiving a full beam straight through the earth, so this
        // model had no night in it — only a dimmer day, and no elevation could make it dark. `rsi`
        // against `rPlanet` is the missing question. Roots that lie ahead of the sample mean the
        // ground stands between it and the body, and the sample then contributes nothing but the
        // optical depth it has already added to the primary ray.
        //
        // **Inert above about one degree of elevation, geometrically rather than by hope.** The ray
        // origin stands 1 km up, so its own horizon is 1.02 degrees down, and the furthest a
        // primary ray can put a sample on the ground is the tangent — about 113 km, which tilts
        // that sample's local vertical by 1.02 degrees. The two are the same number because they
        // are the same tangent, so a body more than about 1.1 degrees up is above the horizon of
        // every sample the integral can take, and the un-shadowed path below stays the old
        // arithmetic in the old order, token for token. Both goldens stand at nineteen degrees.
        vec2 planetHit = rsi(iPos, pSun, rPlanet);
        if (planetHit.x <= planetHit.y && planetHit.y > 0.0)
        {
            iTime += iStepSize;
            continue;
        }

        // Calculate the step size of the secondary ray.
        float jStepSize = rsi(iPos, pSun, rAtmos).y / float(jSteps);

        // Initialize the secondary ray time.
        float jTime = 0.0;

        // Initialize optical depth accumulators for the secondary ray.
        float jOdRlh = 0.0;
        float jOdMie = 0.0;
        float jOdOzn = 0.0;

        // Sample the secondary ray.
        for (int j = 0; j < jSteps; j++) {

            // Calculate the secondary ray sample position.
            vec3 jPos = iPos + pSun * (jTime + jStepSize * 0.5);

            // Calculate the height of the sample.
            float jHeight = length(jPos) - rPlanet;

            // Accumulate the optical depth.
            jOdRlh += exp(-jHeight / shRlh) * jStepSize;
            jOdMie += exp(-jHeight / shMie) * jStepSize;
            jOdOzn += max(1.0 - abs(jHeight - hOzn) / wOzn, 0.0) * jStepSize;

            // Increment the secondary ray time.
            jTime += jStepSize;
        }

        // Calculate attenuation.
        vec3 attn = exp(-(kMie * (iOdMie + jOdMie) + kRlh * (iOdRlh + jOdRlh) + kOzn * (iOdOzn + jOdOzn)));

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
        // The body's intensity, not the sun's: one integral draws both, because a moonlit sky IS
        // this integral with a source 14 magnitudes down. The scale is exactly one whenever the sun
        // is the body, and a multiply by one is exact, so the daylight sky is the sky it was.
        22.0 * frame.skyParams.x,       // intensity of the body the sky follows
        6371e3,                         // radius of the planet in meters
        6471e3,                         // radius of the atmosphere in meters
        // The Rayleigh blue and the Mie coefficient are the measured clear-sky values (Bruneton
        // 2008; Hillaire 2020: Rayleigh 33.1e-6 in the blue, Mie scatter 4e-6), placed 2026-09-11
        // for a deeper blue. They replaced the integral's stock 22.4e-6 and 21e-6: the stock Mie
        // is five times a clean sky's and lays a grey veil over the whole sky, and the stock blue
        // under-scatters at the zenith. `atmosphericSunTransmittance` in RenderRig.cppm restates
        // these and the ozone below and MUST NOT DRIFT from them.
        vec3(5.5e-6, 13.0e-6, 33.1e-6), // Rayleigh scattering coefficient
        4e-6,                           // Mie scattering coefficient
        8e3,                            // Rayleigh scale height
        1.2e3,                          // Mie scale height
        0.758,                          // Mie preferred scattering direction
        // Ozone (2026-09-12, Hillaire 2020): the Chappuis band absorbs green most, red next and
        // blue almost not at all, over a layer that peaks at 25 km and is gone by 10 and 40. A
        // vertical column is 0.028 of depth in the green. It is what keeps the zenith blue under
        // a low sun and the horizon band blue instead of yellow: on a long slant path the green and
        // red are taken and the blue survives. Absorption only, so it never scatters light in.
        vec3(0.650e-6, 1.881e-6, 0.085e-6), // ozone absorption coefficient
        25e3,                           // ozone layer centre
        15e3                            // ozone layer half-width
    );

    // The clouds, composited over the integral and BEFORE the disc: `sky * a + rgb`, radiance
    // over radiance in the layered frame's own premultiplied form. One branch on coverage — the
    // `timeRain.y` pattern — so at zero this shader is bit-identical to the cloudless one and the
    // engine's 1x1 dummy at the binding is never read. The transmittance is kept: the disc and
    // both aureole arms below multiply by it, so overcast hides the sun by occlusion with its hue
    // intact rather than through any new cap logic.
    vec3 ray = normalize(textureCoordinates);
    float cloudTransmittance = 1.0;
    if (frame.cloudParams.x > 0.0)
    {
        // A half-texel four-tap tent over the map, not one sample: the dome march is one crisp
        // sample per texel, so the density field's own iso-lines bake into the map as faint
        // contours, and this read-time tent is what smooths them off the sky (seat, 2026-08-26)
        // — four taps on sky pixels only, against a whole extra blur pass.
        vec2 uv = cloudDomeUv(ray);
        vec2 half_ = 0.5 / vec2(textureSize(cloudMap, 0));
        vec4 cloud = 0.25
            * (texture(cloudMap, uv + vec2(half_.x, half_.y)) + texture(cloudMap, uv + vec2(-half_.x, half_.y))
               + texture(cloudMap, uv + vec2(half_.x, -half_.y)) + texture(cloudMap, uv + vec2(-half_.x, -half_.y)));
        color = color * cloud.a + cloud.rgb;
        cloudTransmittance = cloud.a;
    }

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

    // **The body under the horizon, which the disc has never been asked about.** `angle` above is
    // measured against a direction, and a direction has no horizon in it, so a set sun went on being
    // drawn under the ground and photographed into every probe — the scattering integral's own
    // blindness, one term further on. The earth's limb *cuts* the disc rather than switching it off,
    // which is what a sunset is: the fraction of the disc still above the horizon, over the disc's
    // own angular radius, so the last of the sun goes at the rate its own diameter says. Applied to
    // `sunRadiance` rather than to the arms below, so the aureole — forward-scattered light out of a
    // beam the observer can no longer see — goes with it, and so that a body fully up runs every arm
    // below on exactly the bits it ran on before.
    //
    // The observer stands 1 km up in the model above, so the horizon it sets against is 1.02 degrees
    // down: the same tangent the integral's occlusion test turns on, stated here as its sine.
    const float horizonDip = -0.01772;
    float bodyVisibility = clamp((sunDirection.y - horizonDip) / sunAngularRadius + 0.5, 0.0, 1.0);
    if (bodyVisibility < 1.0)
    {
        sunRadiance *= bodyVisibility;
    }

    // The one view the disc must not appear in. A light probe records the world so that a surface
    // can be given the light it cannot see directly; the sun is not that light, it is the direct
    // term, and a disc in the cube would deliver it a second time — about 9% of the sun's
    // irradiance in the mean, and anywhere between none of it and three times that in a given
    // capture, since the disc is smaller than one texel of a 128-pixel face. The aureole stays:
    // it is scattered light, which is exactly what a probe is for.
    const bool probeCapture = frame.shadowParams.z != 0;

    // Under cloud, every arm multiplies by the map's transmittance, so an overcast sun fades
    // behind cloud the way the sky behind it already has. The dry arms are restated whole rather
    // than multiplied by a transmittance that is exactly 1.0, and the duplication is the point:
    // the gates demand the clear sky bit-identical, and a spelled-out multiply is not that — a
    // driver is free to contract `a * b + c` into a fused multiply-add, and `(a * b) * t + c`
    // rounds differently even at t = 1.0. The dry arms below are the pre-cloud statements, token
    // for token.
    if (frame.cloudParams.x > 0.0)
    {
        if (angle >= sunAngularRadius)
        {
            const float falloff = sunAngularRadius / angle;

            color += sunRadiance * sunAureoleAtEdge * falloff * falloff * cloudTransmittance;
        }
        else if (probeCapture)
        {
            // Continued flat across the disc at its edge value rather than left as a hole, so
            // that nothing in the cube depends on where the sun fell in the texel grid.
            color += sunRadiance * sunAureoleAtEdge * cloudTransmittance;
        }
        else
        {
            // Limb darkening: the disc is not uniform, because a ray leaving its edge travels
            // further through the photosphere than one leaving its centre. The edge lands about a
            // third down, which is what stops the disc reading as a sticker.
            float edge = angle / sunAngularRadius;
            float limb = 1.0 - sunLimbDarkening * (1.0 - sqrt(max(1.0 - edge * edge, 0.0)));

            color += sunRadiance * limb * cloudTransmittance;
        }
    }
    else if (angle >= sunAngularRadius)
    {
        const float falloff = sunAngularRadius / angle;

        color += sunRadiance * sunAureoleAtEdge * falloff * falloff;
    }
    else if (probeCapture)
    {
        color += sunRadiance * sunAureoleAtEdge;
    }
    else
    {
        float edge = angle / sunAngularRadius;
        float limb = 1.0 - sunLimbDarkening * (1.0 - sqrt(max(1.0 - edge * edge, 0.0)));

        color += sunRadiance * limb;
    }

    // The stars, after the body and before the eye's stop, because they are sky like everything else
    // above: the graduated filter is a filter over the picture and the stars are in the picture. Zero
    // is written by day and in every capture, and the branch is where both goldens stay exactly what
    // they were — the day sky out-scatters a bright star by four orders of magnitude, so nothing is
    // being hidden by the gate, only kept off the arithmetic.
    if (frame.skyParams.y > 0.0)
    {
        color += starField(ray, frame.skyParams.y);
    }

    // No tone map here. This shader used to end with `1 - exp(-color)`, which is a display transfer
    // applied inside the sky: it clamped the whole atmosphere to one, so the sun's peak, the
    // horizon and a mid-sky blue all arrived at the post chain within a factor of two of each
    // other. Everything downstream — the exposure meter, the bloom threshold, the light probes —
    // reads scene-referred radiance, and the sky is the brightest thing in the scene.
    // The medium, last: everything above it is light arriving from beyond the fog, and this is
    // the kilometre of air it arrives through.
    // Unfogged on purpose: the fullscreen fog pass integrates the sky's ray one pass later.
    //
    // The eye's sky stop, last of all (2026-09-12): the landscape photographer's graduated filter.
    // The meter exposes the street and the outdoor dial opens 1.5 stops over it, which lands this
    // sky — physically bright, 0.7 of a sunlit grey at the zenith — past the tone curve's ceiling,
    // where the per-channel curve prints it white. The engine writes the gain as ONE in a probe
    // capture, so the probes photograph the true sky and the world's ambient light does not move
    // with it; a gain of exactly one is bit-for-bit the sky before the stop existed. Applied to the
    // whole picture, clouds and disc included: the disc is capped four orders over the ceiling and
    // does not notice, and the clouds are part of the sky the eye is being shown.
    color *= frame.cloudParams.z;

    fragColor = vec4(color, 1.0f);
}