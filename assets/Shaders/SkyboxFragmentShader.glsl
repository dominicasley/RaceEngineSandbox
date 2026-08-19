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
} frame;

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

    vec3 sunRadiance = min(frame.lights[shadowLight].diffuse.rgb / sunSolidAngle, vec3(sunMaximumRadiance));

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
    fragColor = vec4(color, 1.0f);
}