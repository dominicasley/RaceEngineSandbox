#version 450
// The cloud dome (docs/cloud-rendering-brief.md): the sky hemisphere's clouds, marched once per
// frame into a lat-long map instead of once per sky pixel per view. It rides the world camera's
// chain, writes rgb = in-scattered cloud radiance and a = transmittance, and the skybox composites
// it next frame — `sky * a + rgb`, the disc and aureole then multiplied by a — so every consumer,
// chase camera, cockpit and all six probe faces alike, samples one map and the march cost is paid
// at map resolution rather than frame resolution.
//
// Nothing about its colour is authored, which is the chain the fog established: the sun term is
// the scene light the cascades follow, the ambient is the global probe's band-0, and both move
// with `OSR_SUN` without a number changing here. The density model is Nubis (Schneider, SIGGRAPH
// 2015/2017): a Perlin-Worley base volume carved by a Worley FBM, gated by coverage through a
// hashed weather field, shaped by a height profile, eroded by a detail volume. The lighting is a
// short march toward the sun for Beer-Lambert, a two-lobe Henyey-Greenstein phase, Hillaire's
// three-octave multiple-scattering approximation and a powder term for the dark-edge look.
//
// Determinism is the house rule: the clock is the simulated instant the push constant carries and
// every random number is an integer hash of a cell — never a wall reading, never fract(sin()).
// The constants below carry the look and this file is their record, per the wet-surfaces
// convention; every distance is in world units, which are TENTHS of a metre.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// Set 0: per camera pass — the frame block, declared as far as the clouds read, which is the whole
// of it: cloudParams is the last field.
struct Light {
    vec4 position;             // xyz the direction *towards* the light
    vec4 diffuse;
    vec4 specular;
    vec4 ambientAttenuation;   // xyz ambient, w attenuation
};

struct Probe {
    vec4 irradiance[SH_COEFFICIENTS];
    vec4 boxMin;               // xyz world minimum of the influence box, w the blend band's width
    vec4 boxMax;               // xyz world maximum, w non-zero for the scene's global probe
    vec4 position;             // xyz where it was captured, w its slice of probeSpecular (-1: none, see below)
};

layout(set = SET_FRAME, binding = 0) uniform FrameData {
    mat4 viewMatrix;
    vec4 cameraPosition;
    ivec4 lightCount;              // x = lights in use, never above MAX_LIGHTS
    Light lights[MAX_LIGHTS];
    mat4 shadowMatrices[SHADOW_CASCADES];
    vec4 shadowSplits;
    vec4 shadowTexelWorldSize;
    vec4 shadowDepthScale;
    ivec4 shadowParams;            // y = the light the cascades follow, which is the sun here
    ivec4 probeParams;             // x = probes in use (0 = no image-based lighting at all)
    Probe probes[MAX_IBL_PROBES];
    vec4 fogDensity;
    vec4 fogScatter;
    vec4 fogAmbient;
    vec4 timeRain;
    vec4 rainWind;
    vec4 wiperArcA;
    vec4 wiperArcB;
    vec4 wiperSweep;
    vec4 wiperTiming;
    vec4 wiperPane;
    vec4 rainBody;
    vec4 cloudParams;              // x effective coverage, y stratus-to-cumulus type, zw reserved
} frame;

// The baked noise, at the fullscreen set's two volume slots. Both repeat — the tiles are the whole
// point of baking them.
layout(set = SET_POST_PROCESS, binding = CLOUD_BASE_NOISE_BINDING) uniform sampler3D cloudBaseNoise;
layout(set = SET_POST_PROCESS, binding = CLOUD_DETAIL_NOISE_BINDING) uniform sampler3D cloudDetailNoise;

layout(push_constant) uniform PassParameters {
    vec4 tone;      // unread here: the clouds are radiance, long before the tone map
    vec4 pass;      // x target level, y target levels, zw target size
    vec4 view;      // unread here: the map is a hemisphere, not this camera's frustum
    vec4 effect;    // unread here: the weather is the scene's statement, not a pass parameter
    vec4 viewRight; // xyz the view's +x in world space, w the camera's world x
    vec4 viewUp;    // xyz the view's +y in world space, w the camera's world y
    vec4 viewBack;  // xyz the view's +z in world space (the view looks down -z), w the world z
    vec4 weather;   // x the simulated instant in seconds — the one clock this shader may read
} params;

// The shell: where the clouds live, as world-unit altitudes. Base 15,000 units is a 1,500 m cloud
// base, top 33,000 is 3,300 m — 1.8 km of vertical development, heap-cloud country: a 900 m slab
// could only draw pancakes, and the seat asked for cumulus (2026-08-26). Individual clouds do not
// reach the ceiling — each weather cell grows its own tower height, below. The rig's sun-dimming
// Beer-Lambert states its path through this same 18,000-unit thickness and the two must agree, or
// the disc dims for a shell the map is not drawing.
const float cloudShellBase = 15000.0;
const float cloudShellTop = 33000.0;

// Extinction at density 1, per world unit: 0.006 is 0.06 per metre, the middle of a real cloud's
// 0.02-0.1 — raised from the low end when the seat asked for more pronounced lighting, because
// extinction is what the self-shadow gradient is made of: a denser cloud grades from lit top to
// dark base harder for the same sun march.
const float cloudExtinction = 0.006;
// The single-scatter albedo. Clouds are the whitest natural object there is; what little is lost
// is absorption at the droplet, and 0.96 keeps the deep base grey rather than black.
const float cloudScatterAlbedo = 0.96;

// How far along a near-horizon ray the march is allowed to reach. This was 150,000 units — 15 km —
// and that emptied the sky a driver actually looks at. The base sits at 15,000 units, so a ray
// below 5.70 degrees of elevation reaches 15 km before it ever enters the shell: `enter >= exit`,
// and the pass writes no cloud at all. Between 5.70 and 12.67 degrees the shell was cut short in
// depth. From the seat that is a pale empty band lying on the horizon with heaps floating above it,
// and it is the whole of the cockpit view — every capture that judged this feature looked UP at
// forty degrees, over the top of the defect (2026-08-27).
//
// 450,000 units is 45 km and drops the empty band to below 1.90 degrees. The cost is close to
// nothing, which is a property of the two-level march rather than luck: `coarseStep` is the span
// divided by a fixed bin count, so a longer reach makes each coarse bin longer instead of adding
// bins, and only the fine work — which happens where cloud actually is — grows.
//
// The curvature this flat shell still does not have is worth 0.2 degrees at 45 km: the real drop
// is 159 m over that distance, which puts the base at 1.71 degrees against this model's 1.90. That
// is below the map's own row spacing, so a spherical shell would buy nothing here and is not worth
// the sqrt per sample it would cost in the density function.
const float cloudHorizonDistance = 450000.0;

// The drift: a stated constant times the clock, not a wind model — wind stays absent by the
// world-rain decision. 90/40 world units a second is about 10 m/s of altitude wind, mostly along
// +x with a slew across it.
const vec3 cloudDrift = vec3(90.0, 0.0, 40.0);
// The detail volume drifts a little faster than the base, so the erosion crawls across the shapes
// rather than riding them — the cheap stand-in for internal churn.
const float cloudDetailDriftScale = 1.35;

// The tiling: one repeat of the 128-cube base per 24,000 units (2.4 km), which puts a
// Perlin-Worley feature at a few hundred metres; one repeat of the 32-cube detail per 2,800 units
// (280 m), an erosion feature under a hundred. The weather field's cells are 40,000 units — 4 km —
// so a broken sky reads as systems drifting past, not as texture.
const float cloudBaseScale = 1.0 / 24000.0;
// One repeat of the 32-cube detail per 6,000 units (600 m), the finest erosion feature ~40 m.
// The floor is the dome map's own resolving power, not taste: a map texel spans 12-25 m of cloud
// at typical slant distances, and erosion features at or under that span bake into the map as a
// static stipple (seat, 2026-08-26) — the same Nyquist rule the march's distance fade applies
// along the ray, applied across it.
// One repeat of the detail volume per 12,000 units (1.2 km), which is wider than a cloud, so the
// tiling never repeats inside one. The volume behind this is 128 cubed at Worley cells 8/16/32/64
// since 2026-08-27, so the features run 1,500 down to 187 units — 150 m down to 18.7 m.
//
// The floor is the DOME MAP's resolving power and not the volume's, and which of the two binds
// depends entirely on range. At 4 km a map texel spans 24 m and the map truncates first, which is
// what the fade below is sized against. At 2 km a texel spans 12 m, so the map will carry a 25 m
// feature and the old volume — nothing finer than 750 units, 75 m, and two texels wide at 32 cubed
// so unable to hold more — was the binding limit by a factor of ten. Seventy-five metres at that
// range is ninety screen pixels, which is a cloud made of clay (seat, 2026-08-27, "the shape is
// wrong there is not enough detail, they look too soft"). A finer octave was measured as worthless
// on 2026-08-27 and that measurement was taken at 4 km, in the regime where the map binds: right
// number, wrong regime, and near cloud is the one that is looked at.
const float cloudDetailScale = 1.0 / 12000.0;
// The weather field's TOP octave, and it is system scale again as of 2026-08-27.
//
// This was 16,000 units — 1.6 km — chosen deliberately as CLOUD scale after 40,000 put one
// connected system overhead as "a single goofy mass with a coverage-edge skirt" (seat,
// 2026-08-26). That result belonged to the old carve: with the mask saturated the shape remap was
// the identity, so a large weather cell could only ever produce one solid blob, never a group of
// separate clouds inside a system. The carve band breaks a saturated mask into individual clouds
// now, so the objection no longer holds and the cost of the workaround is measurable.
//
// The cost was that 1.6 km is SMALLER than the field's own 3 km mean cloud spacing, so the weather
// placed individual clouds instead of grouping them, and nothing in the model worked at the scale
// that makes systems. From above, the sky was an even speckle — salt scattered on a table, no
// clusters and no open lanes (seat, 2026-08-27, "the distribution just looks wrong"). Measured as
// the spread of local coverage over 10 km blocks against its mean, the shipped field scores 0.32
// where a five-octave field topped at 12.8 km scores 1.99.
//
// The octaves run 12.8, 6.4, 3.2, 1.6 and 0.8 km, so systems, groups and single heaps all come out
// of one field rather than one scale doing every job.
const float cloudWeatherCell = 128000.0;
const int cloudWeatherOctaves = 5;
const float cloudWeatherFalloff = 0.62;
// Averaging more octaves narrows the field's own distribution, and the coverage threshold is cut
// against that distribution — so without this, adding octaves silently halved how much sky a given
// `OSR_CLOUDS` produced. The contrast restores the two-octave spread about the mean, which is what
// keeps the setting meaning what it meant. Measured: at 1.30 the field covers 12.3% of sky, which
// is the shipped figure to a tenth, while clumping goes 0.31 to 1.52 — the same amount of cloud,
// gathered into systems instead of spread evenly.
const float cloudWeatherContrast = 1.30;

// The tower field's own cell, stated separately since 2026-08-27. It was `cloudWeatherCell * 0.75`
// and must NOT follow that constant up to system scale: it decides each cloud's summit, so at
// 96,000 units every heap for ten kilometres would top out at the same height. 12,000 is what the
// old expression evaluated to, so this is unchanged in behaviour.
const float cloudTowerCell = 12000.0;

// The sun march, and its schedule is GEOMETRIC rather than uniform, which is the whole of it. A
// uniform step cannot be both near enough to shadow a billow and long enough to cross the shell,
// and the uniform one it replaces chose reach: eight taps of 2,000 units put the FIRST sample
// 1,000 units — 100 m — from the point being shaded, and handed it bulk shape only. So nothing
// smaller than roughly 200 m could cast a shadow on anything. Every billow the carve and the
// erosion cut was lit flat, which is the seat's "lighting is flat" (2026-08-27): the shape was
// there and the light could not see it.
//
// Growing from 300 units puts the first four taps inside the first 2,300, where they resolve the
// 75-300 m features, and lets the last three carry the far bulk. Reach is 12,400 rather than
// 16,000 and that costs nothing real: at density 0.8 a thousand units of cloud is already an
// optical depth of 4.8, so the first octave is dead long before the old reach, and the later
// octaves divide extinction by up to eight, which multiplies their effective reach by the same.
const int cloudSunTaps = 8;
const float cloudSunFirstStep = 300.0;
const float cloudSunStepGrowth = 1.45;   // 300, 435, 631, 915, 1326, 1923, 2788, 4043 — 12,361 total

// The tap length at which detail stops being sampled. This is the Nyquist rule that put bulk-only
// shape in the shadow march in the first place, kept, but applied PER TAP instead of to the whole
// march: the finest erosion feature is about 750 units, so a tap shorter than that resolves it
// honestly and a longer one would alias it into the wormy interior the blanket rule was killing.
// Under the schedule above the first three taps qualify, and they are the ones that shade a
// billow's own face.
const float cloudSunDetailStep = 750.0;

// The two-lobe phase: a strong forward lobe for the silver lining, a small back lobe for the
// bright-when-backlit floor, mixed 70/30 forward.
const float cloudForwardScattering = 0.7;
const float cloudBackScattering = -0.15;
const float cloudLobeMix = 0.7;

// Hillaire's multiple-scattering octaves: each successive octave halves extinction, contribution
// and phase asymmetry, which is what keeps a thick cloud luminous where single scattering would
// go black.
const int cloudScatterOctaves = 4;   // the fourth octave is what lights a deep body through, per the seat
const float cloudOctaveAttenuation = 0.5;

// The powder term's depth: how much of the sun term a sunlit thin edge gives up, the dark-edge
// look of Schneider's E = 1 - exp(-2d) stated as a floor rather than a curve somebody retunes.
// 0.85 after the seat asked for more pronounced lighting (2026-08-26) — the edges carve deeper.
const float cloudPowderStrength = 0.85;

// Detail erosion fades out along the view ray between these distances: past a few kilometres the
// march's step length outgrows the sub-100 m erosion features, and sampling frequencies the steps
// cannot resolve is exactly what printed as layers from the seat (2026-08-26). Far clouds keep
// the base shapes; near clouds keep the carving.
// Measured 2026-08-27 on a radially averaged power spectrum of the marched silhouette, which is
// the instrument this wanted all along — mean gradient conflates how much cloud there is with how
// much detail each edge carries, and scored a more eroded field as a less detailed one.
//
// At 3,000 units the fade was starting far too early. The shipped field puts 71% of its energy
// below 0.08 cycles per degree — structure larger than 875 m at a 4 km slant — against 26% with
// the fade off, and the 0.16-0.63 bands that carry the 110-440 m features triple. That is the
// "blurry, no fine detail" of 2026-08-27, and it is upstream of the dome map rather than in it:
// the map keeps 99% of the low bands and 93% at 0.32-0.63.
//
// Where the map DOES start costing is 0.63-1.27 (75% kept) and 1.27-2.54 (43%), so the erosion's
// finest 750-unit feature is carried honestly to about 7 km and is marginal past 12. The fade now
// stands there instead of at three: it is the map's Nyquist expressed as a distance, which is what
// it was always meant to be, sized off a measurement rather than off the stipple it was reacting
// to. A finer noise octave was measured too and is worth nothing — 0.45% against 0.44% — because
// this ceiling is the map's, not the volume's.
const float cloudDetailFadeStart = 60000.0;
const float cloudDetailFadeEnd = 120000.0;

// What the base shape is carved against, and the reason it exists at all. `shaped` — the
// Perlin-Worley through Nubis's own remap — is MEASURED off the baked volume at [0.34, 1.0], with
// 91% of its texels between 0.6 and 0.9: the remap's floor is `worleyFbm - 1.0`, which is about
// -0.52, so even a base.r of zero comes out at a third. It cannot reach zero and so it cannot cut
// a silhouette on its own. The shipped chain then carved it with `cloudRemap(shaped, 1.0 - mask,
// 1.0, 0.0, 1.0)`, and wherever the coverage mask saturated — which is most of every cloud — that
// floor was ZERO and the remap was the identity. The 3D noise decided nothing.
//
// What was left deciding was the two XZ fields: the coverage mask, whose bilinear value noise on a
// square lattice has axis-aligned rounded-rectangle level sets, extruded between the base-lift
// plane and the per-cell ceiling plane at a near-constant 0.79 density. That is a box with rounded
// corners, and it is exactly the seat report (2026-08-26, "they look like floating cubes ...
// missing the wisps around the edges and variations in density").
//
// So coverage MOVES the threshold through the noise's own working band instead of collapsing it.
// Dense is where local coverage saturates, thin is where it runs out, and both sit inside the
// measured distribution — so the silhouette is the Perlin-Worley's own iso-surface at every
// coverage: isotropic, no lattice in it, and carrying the noise's raggedness. The far end also
// fixes the coverage-edge skirt: a weather system now frays into sparse thin heaps rather than
// thinning uniformly.
const float cloudCarveDense = 0.60;
const float cloudCarveThin = 0.90;

// How wide a band of the shape sits between "no cloud" and "solid", and it exists because the
// threshold alone was doing two jobs at once. Remapping to a FIXED top of 1.0 divides by
// `1 - threshold`, so moving the threshold up to remove cloud also narrowed the surviving range
// and thinned everything that was left: at 0.50 the carved range is 0.80 wide, at 0.70 it is 0.30,
// and a typical shape value of 0.78 falls from a density of 0.72 to 0.32. Measured off a capture
// at 0.70, 92% of the cloud was near-transparent haze and only the top 8% of the field reached
// solid — flat, scrappy and washed out, whatever the saturation was set to.
//
// A band fixes the gradient instead of the ceiling, so the three knobs are finally independent:
// the threshold decides HOW MUCH cloud, the band decides how SHARP its edge is, and
// cloudSaturation decides how DENSE it gets. Moving one no longer silently moves the others.
const float cloudCarveBand = 0.20;

// The detail erosion's strength, and it is weighted by how deep inside the cloud the sample sits.
// Unweighted it was very nearly inert — at full strength it moved an interior sample from 0.79 to
// 0.69 — because it subtracts the same amount from a core it can never empty as from an edge it
// ought to be shredding. Weighted by `1 - covered` it is absent in the core, which stays solid, and
// at full strength on the fringe, which is where filaments come from. Core density and edge
// wispiness fought over one constant before; they do not now, which is what lets this be 0.90.
const float cloudErosion = 0.55;

// How much of that erosion the CORE is shielded from. A full shield — the plain `1 - covered` this
// replaces — protects core density perfectly and costs the core all of its structure: every sample
// saturates to the same density, and a uniform volume under directionless ambient light has no
// form in it at all. The lit top still read well because the sun march carves it, but the shadowed
// two thirds of every cloud went to smooth clay (seat, 2026-08-27, "they just don't look real").
// A partial shield keeps most of the density and gives the interior something to vary: at 0.65 the
// core still erodes at 0.19 where the fringe erodes at 0.55.
const float cloudErosionCoreShield = 0.65;

// The core saturation, and the curve's SHAPE carries this rather than its gain. `1 - exp(-g d)`
// rises fastest at zero, so it LIFTS a fringe: at g = 2.2 a carved 0.05 reads 0.10 and at 4.4 it
// reads 0.24, and at this extinction a grazing ray through a 300 m fringe at 0.24 is already
// opaque. A curve that is concave from the origin cannot leave a translucent edge, whatever else
// is done upstream — measured on a marched-opacity slice, the shipped chain puts 3% of its cloud
// area in the translucent band and no gain moves it past 5%. A sigmoid does: flat at the origin,
// so the fringe keeps a long low tail, and flat at one, so the core saturates without the contour
// a hard clamp would draw — which was the objection the exponential was chosen over in the first
// place, and it still holds. The same slice puts a sigmoid at 9%, three times the shipped fringe.
// The constant scales the input, so above one more of the carved range reaches full opacity and
// the cores get denser without the fringe paying for it; 1.0 is the softest this can be.
const float cloudSaturation = 1.8;

// A directional source of irradiance E scattered isotropically gives a radiance of E/4pi, and the
// phase below is normalised to that isotropic case — the fog's own convention, restated here
// because both halves of the sun term depend on it.
const float cloudInverseSphere = 0.0795774715;

// The texel's world direction. u is azimuth, the FULL 360 degrees; v is elevation from -10 to +90
// degrees — ten below the horizon so the composite has rows to fade through where terrain usually
// stands, the pole at the top edge. Zero azimuth looks along +z, matching the game's own yaw
// convention.
//
// **The skybox carries the exact inverse of this mapping (cloudDomeUv in
// SkyboxFragmentShader.glsl) and the two MUST NOT DRIFT** — a disagreement moves every cloud in
// the sky by the difference, which reads as a projection bug nobody can localise.
vec3 cloudDomeDirection(vec2 uv)
{
    float azimuth = uv.x * 6.28318530718;
    float elevation = radians(-10.0 + uv.y * 100.0);
    float level = cos(elevation);

    return vec3(sin(azimuth) * level, sin(elevation), cos(azimuth) * level);
}

// The same integer hash the world rain and the wet-surface mask use, the house rule for anything
// procedural: a function of the cell and of nothing else.
float cloudHashCell(ivec3 cell)
{
    uint hashed = uint(cell.x) * 374761393u + uint(cell.y) * 668265263u + uint(cell.z) * 2246822519u;
    hashed = (hashed ^ (hashed >> 13u)) * 1274126177u;
    hashed = hashed ^ (hashed >> 16u);

    return float(hashed & 0x00FFFFFFu) / 16777215.0;
}

// Two octaves of value noise over world XZ, hashed per cell and blended with a smootherstep — the
// weather: which parts of the sky today's coverage actually lands on. In-pass hash noise rather
// than a baked 2D asset, by the world-rain rule; the third hash lane separates the octaves.
float cloudWeatherNoise(vec2 xz)
{
    float amplitude = 1.0;
    float cellSize = cloudWeatherCell;
    float total = 0.0;
    float normalisation = 0.0;

    for (int octave = 0; octave < cloudWeatherOctaves; octave++)
    {
        vec2 grid = xz / cellSize;
        ivec2 cell = ivec2(floor(grid));
        vec2 inside = fract(grid);
        vec2 eased = inside * inside * inside * (inside * (inside * 6.0 - 15.0) + 10.0);

        float c00 = cloudHashCell(ivec3(cell, octave));
        float c10 = cloudHashCell(ivec3(cell + ivec2(1, 0), octave));
        float c01 = cloudHashCell(ivec3(cell + ivec2(0, 1), octave));
        float c11 = cloudHashCell(ivec3(cell + ivec2(1, 1), octave));

        total += amplitude * mix(mix(c00, c10, eased.x), mix(c01, c11, eased.x), eased.y);
        normalisation += amplitude;
        amplitude *= cloudWeatherFalloff;
        cellSize *= 0.5;
    }

    // Normalised by the amplitude sum rather than by amplitudes chosen to add to one, so the octave
    // count is a constant to change rather than a set of weights to re-derive. The contrast then
    // stretches about the mean — see cloudWeatherContrast for why that has to happen here.
    return 0.5 + (total / normalisation - 0.5) * cloudWeatherContrast;
}

// Each weather region's own tower height: one octave of the same value noise on its own hash
// lane, at three quarters of the weather cell so systems carry more than one summit. This is what
// separates a cumulus sky into individual heaps — without it every cloud tops out at the same
// ceiling and the field reads as one torn sheet.
float cloudTowerField(vec2 xz)
{
    vec2 grid = xz / cloudTowerCell;
    ivec2 cell = ivec2(floor(grid));
    vec2 inside = fract(grid);
    vec2 eased = inside * inside * inside * (inside * (inside * 6.0 - 15.0) + 10.0);

    float c00 = cloudHashCell(ivec3(cell, 2));
    float c10 = cloudHashCell(ivec3(cell + ivec2(1, 0), 2));
    float c01 = cloudHashCell(ivec3(cell + ivec2(0, 1), 2));
    float c11 = cloudHashCell(ivec3(cell + ivec2(1, 1), 2));

    return mix(mix(c00, c10, eased.x), mix(c01, c11, eased.x), eased.y);
}

// Nubis's remap, clamped: where v stands in [oldMin, oldMax], restated in [newMin, newMax].
float cloudRemap(float v, float oldMin, float oldMax, float newMin, float newMax)
{
    return clamp(newMin + (v - oldMin) * (newMax - newMin) / max(oldMax - oldMin, 1.0e-4), newMin, newMax);
}

// The carved shape read as a density: a sigmoid, for the reason cloudSaturation states. One place
// only, because the density function has two exits — the faded-out-erosion early return and the
// eroded one — and the shipped chain saturated only the second. That was a real discontinuity: at
// the detail fade's far edge the density stepped 0.72 to 0.79, and since the sun march asks for
// density at `detailFade = 0` it was self-shadowing against a systematically thinner cloud than
// the one being drawn.
float cloudSaturate(float carved)
{
    float t = clamp(carved * cloudSaturation, 0.0, 1.0);

    return t * t * (3.0 - 2.0 * t);
}

// Where in the slab a cloud kind puts its mass, over normalised shell height. Stratus is a low
// flat sheet — up fast, gone by four tenths; cumulus rises from a flat base towards a rounded top
// that thins out by the ceiling. Blended by the scene's type.
float cloudHeightProfile(float h)
{
    // Stratus in metres, not in fractions: when the shell doubled for the cumulus towers the
    // sheet's numbers halved with it, so a rain sky is the same low flat lid it always was.
    float stratus = smoothstep(0.0, 0.04, h) * (1.0 - smoothstep(0.11, 0.21, h));
    // Cumulus is bottom-heavy with a long usable body: a flat base by twelve hundredths, then
    // mass all the way up — the per-cell ceiling in cloudDensity is what rounds each heap's top,
    // so the intrinsic profile only has to feed it.
    float cumulus = smoothstep(0.01, 0.12, h) * (1.0 - smoothstep(0.85, 1.0, h));

    return mix(stratus, cumulus, clamp(frame.cloudParams.y, 0.0, 1.0));
}

// The local coverage mask: the weather field thresholded by the scene's coverage, so raising
// coverage grows the systems that already exist rather than fading a uniform veil in. The band
// narrowed from a quarter to 0.18 and the trailing multiply by coverage is deliberately gone
// (2026-08-26): that multiply thinned every cloud at partial coverage, and the reference heap the
// seat asked for is DENSE at forty percent cover — coverage decides where clouds are, never how
// solid the kept ones get to be.
float cloudCoverageMask(vec2 xz)
{
    float weather = cloudWeatherNoise(xz);
    float coverage = clamp(frame.cloudParams.x, 0.0, 1.0);

    return smoothstep(1.0 - coverage, min(1.0 - coverage + 0.18, 1.0), weather);
}

// The density at a world position: base shape carved by its own Worley FBM, gated by coverage,
// shaped by height, eroded by detail. Zero almost everywhere, which is what the march's `continue`
// lives on.
float cloudDensity(vec3 worldPosition, float detailFade)
{
    float mask = cloudCoverageMask(worldPosition.xz);
    if (mask <= 1.0e-3)
    {
        return 0.0;
    }

    // The underside: real cumulus share a condensation level but not a razor plane, and a shared
    // razor plane is exactly what the seat saw (2026-08-26, "flat on the bottom, same floor,
    // none round over"). Each column's floor lifts with a lobe-scale noise, and lifts harder
    // where coverage thins — bases undulate, and the underside curves up toward every cloud's
    // edge, which is the round-over. The tower field resampled at 3.7x and offset is ~800 m
    // lobes on an independent lattice.
    float lobe = cloudTowerField(worldPosition.xz * 3.7 + vec2(11000.0, 47000.0));
    float baseLift = 0.06 * lobe + 0.14 * (1.0 - mask);
    float shellFraction = (worldPosition.y - cloudShellBase) / (cloudShellTop - cloudShellBase);
    float h = clamp((shellFraction - baseLift) / max(1.0 - baseLift, 1.0e-3), 0.0, 1.0);
    float profile = cloudHeightProfile(h);
    if (profile <= 0.0)
    {
        return 0.0;
    }

    // Each system's own summit: the tower field sets a local ceiling — the full range for
    // cumulus, none for stratus, whose sheet has no towers to grow. The taper is a fifth of the
    // shell, which rounds a heap's top off instead of shearing it flat at its cell's height.
    float ceiling =
        mix(1.0, mix(0.30, 1.0, cloudTowerField(worldPosition.xz)), clamp(frame.cloudParams.y, 0.0, 1.0));
    profile *= 1.0 - smoothstep(ceiling - 0.2, ceiling, h);
    if (profile <= 0.0)
    {
        return 0.0;
    }

    vec3 drifted = worldPosition + cloudDrift * params.weather.x;
    vec4 base = texture(cloudBaseNoise, drifted * cloudBaseScale);

    // R is Perlin-Worley, GBA the three inverted-Worley octaves; the FBM carves the Perlin's
    // connective tissue away so what is left is cellular — Nubis's own construction.
    float worleyFbm = base.g * 0.625 + base.b * 0.25 + base.a * 0.125;
    float shaped = cloudRemap(base.r, worleyFbm - 1.0, 1.0, 0.0, 1.0);

    // The carve, against a threshold the shape's own distribution straddles — see cloudCarveDense.
    // Coverage slides the threshold rather than collapsing it, so the silhouette is the 3D noise's
    // iso-surface and not the weather field's, and the vertical envelope multiplies in AFTER the
    // carve rather than before it: applied first it scaled the shape below the threshold near the
    // base and the top, which would have raised the cloud floor instead of shaping it.
    float threshold = mix(cloudCarveThin, cloudCarveDense, mask);
    float covered = cloudRemap(shaped, threshold, threshold + cloudCarveBand, 0.0, 1.0) * profile;
    if (covered <= 1.0e-3)
    {
        return 0.0;
    }

    // Detail erosion, wispy at the base and billowy above: near the base the detail FBM is
    // subtracted as it stands, higher up its inverse is, which rounds the tops and shreds the
    // bottoms — the same trick, the same reason, as the reference. The wispy-to-billowy flip
    // saturates a third of the way up the slab rather than an eighth: the faster flip drew its
    // own boundary as a stratum. Faded-out erosion skips the fetch and returns the base shape
    // unchanged — cloudRemap with a zero floor is the identity, so the fade is continuous.
    //
    // `1 - covered` is the edge weighting (cloudErosion): full strength on the fringe, absent in
    // the core. A core sample skips the fetch on the same branch a faded-out one does.
    float erosionStrength = cloudErosion * detailFade * (1.0 - cloudErosionCoreShield * covered);
    if (erosionStrength <= 1.0e-3)
    {
        return cloudSaturate(covered);
    }

    vec3 detailPosition = worldPosition + cloudDrift * (params.weather.x * cloudDetailDriftScale);
    // FOUR octaves since the 2026-08-27 re-bake, not three: the volume is 128 cubed at Worley
    // cells 8/16/32/64, so alpha is an octave now rather than the constant 255 it used to hold,
    // and the finest feature is 187 world units — 18.7 m — against the 750 it was. Weights sum to
    // one so the FBM's mean is unchanged and only its frequency content grew.
    vec4 detail = texture(cloudDetailNoise, detailPosition * cloudDetailScale);
    float detailFbm = detail.r * 0.5 + detail.g * 0.25 + detail.b * 0.15 + detail.a * 0.10;
    // The flip runs over the LOCAL cloud's own height, not over a fixed fraction of the shell.
    // `h * 3.0` completed at a shell height of 0.333 — 6,000 units, 600 m above the base — and the
    // clouds are about 620 m tall, so the change from wispy erosion to billowy landed across the
    // middle of every cloud at the same altitude everywhere. It drew as a hard horizontal seam
    // between a lumpy top and a smooth dark bottom (seat, 2026-08-27). Moving the constant is what
    // was tried before, from an eighth to a third, and it only moved the seam: any fixed fraction
    // of the shell is a plane, and a plane through every cloud is a stratum however high it is.
    // Dividing by the cell's own ceiling makes the transition span each cloud from base to top, so
    // it follows the tower field's surface rather than cutting across it.
    float erosion = mix(detailFbm, 1.0 - detailFbm, clamp(h / max(ceiling, 1.0e-3), 0.0, 1.0));

    // Smooth saturation: a core saturates toward fully opaque, so a cloud's interior is a
    // uniform cream instead of printing the noise field as a knitted pattern through partial
    // transparency (seat, 2026-08-26). Still smooth at both ends rather than a gain into min(),
    // because a hard clamp draws its own saturation boundary as a contour — see cloudSaturate for
    // why the curve is now a sigmoid rather than the exponential that argument first bought.
    return cloudSaturate(cloudRemap(covered, erosion * erosionStrength, 1.0, 0.0, 1.0));
}

// Henyey-Greenstein, normalised so isotropic scattering is exactly 1 — the same statement the fog
// makes, kept in the same form so the 1/4pi lives in one place on both shaders.
float cloudPhase(float cosTheta, float anisotropy)
{
    float g = clamp(anisotropy, -0.95, 0.95);
    float gg = g * g;
    float denominator = 1.0 + gg - 2.0 * g * clamp(cosTheta, -1.0, 1.0);

    return (1.0 - gg) / max(pow(max(denominator, 0.0), 1.5), 1.0e-4);
}

// The optical depth toward the sun from a point in the slab: a short fixed march, done for every
// sample the view march keeps, which is what shades a cloud's own base dark under a lit top.
float cloudSunOpticalDepth(vec3 worldPosition, vec3 towardsLight)
{
    float depth = 0.0;
    float along = 0.0;
    float stepLength = cloudSunFirstStep;

    for (int tap = 0; tap < cloudSunTaps; tap++)
    {
        // Mid-tap centres: sampling at whole multiples quantised the self-shadow into strata.
        vec3 samplePosition = worldPosition + towardsLight * (along + 0.5 * stepLength);
        if (samplePosition.y > cloudShellTop)
        {
            break;
        }

        // Detail where this tap is short enough to resolve it, bulk shape where it is not — the
        // per-tap form of the rule cloudSunDetailStep states. A step is a constant of the loop, so
        // the gate is the same for every pixel and cannot draw a boundary of its own.
        float detailFade = stepLength <= cloudSunDetailStep ? 1.0 : 0.0;
        depth += cloudDensity(samplePosition, detailFade) * cloudExtinction * stepLength;

        along += stepLength;
        stepLength *= cloudSunStepGrowth;
    }

    return depth;
}

// The colour of the light the clouds scatter in from everywhere that is not the sun: the global
// probe's band-0 coefficient, the mean of the sphere it photographed. **Copied verbatim from
// fogAmbientRadiance (VolumetricFogFragmentShader.glsl) and the two MUST NOT DRIFT** — they are
// one statement about where ambient light comes from, made in two passes.
vec3 cloudAmbientRadiance()
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

// Interleaved gradient noise — a function of the pixel and of nothing else, the fog's own start
// dither, moving each ray's first sample inside its first step so the slab's entry face does not
// print as a band.
float cloudDither(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main()
{
    // Zero coverage is the clear sky. The pass is only built when the scene states cloud, so this
    // is belt beside braces — and the branch that keeps a mid-session zero honest.
    if (frame.cloudParams.x <= 0.0)
    {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);

        return;
    }

    vec3 rayDirection = cloudDomeDirection(textureCoordinates);
    vec3 cameraPosition = vec3(params.viewRight.w, params.viewUp.w, params.viewBack.w);

    // The shell from below. A level or falling ray never reaches it, and a camera already inside
    // or above the slab is not a view this game has — the car is on the ground — so both write
    // "no cloud, full transmittance" rather than pretending.
    if (rayDirection.y <= 2.0e-3 || cameraPosition.y >= cloudShellBase)
    {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);

        return;
    }

    float enter = (cloudShellBase - cameraPosition.y) / rayDirection.y;
    float exit = min((cloudShellTop - cameraPosition.y) / rayDirection.y, cloudHorizonDistance);
    if (enter >= exit)
    {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);

        return;
    }

    int shadowLight = clamp(frame.shadowParams.y, 0, MAX_LIGHTS - 1);
    vec3 towardsLight = normalize(frame.lights[shadowLight].position.xyz);
    vec3 sunColour = frame.lights[shadowLight].diffuse.rgb;
    float cosTheta = dot(rayDirection, towardsLight);

    vec3 ambient = cloudAmbientRadiance();

    // A two-level march, midpoint-sampled and deliberately NOT dithered. Both were seat findings
    // (2026-08-26): any per-texel phase dither bakes into the map as static grain, and a uniform
    // step cannot be both long enough to cross a 50 km slant span and short enough to resolve the
    // density features — the mismatch printed as onion-ring contours through every cloud body. So
    // clear air is walked in coarse bins and cloud interiors in quarter-length fine steps, with
    // the fine march stepping back up after six consecutive empty samples; the fine sampling
    // lands only where cloud is, which is what a bigger uniform step count could not buy.
    float coarseStep = (exit - enter) / float(CLOUD_MARCH_STEPS / 2);
    float fineStep = coarseStep * 0.25;

    vec3 radiance = vec3(0.0);
    float transmittance = 1.0;

    float along = enter;
    // The furthest point the fine march has already walked: the entry back-up below must never
    // re-walk below it, which is what bounds the iteration count on a broken field — every fine
    // step advances it, so the fine work over the whole ray is at most the span at fine length.
    float refinedUntil = enter;
    bool refined = false;
    // Whether the last sample sat in clear air: the empty-to-cloud transition is where the entry
    // face is bisected and the fine ladder re-anchored, below.
    bool wasEmpty = true;
    int misses = 0;

    for (int iteration = 0; iteration < CLOUD_MARCH_STEPS * 4; iteration++)
    {
        if (along >= exit || transmittance < 0.01)
        {
            break;
        }

        float stepLength = refined ? fineStep : coarseStep;
        vec3 samplePosition = cameraPosition + rayDirection * (along + 0.5 * stepLength);

        float detailFade = 1.0 - smoothstep(cloudDetailFadeStart, cloudDetailFadeEnd, along);
        float density = cloudDensity(samplePosition, detailFade);

        if (!refined)
        {
            if (density > 0.0)
            {
                // Cloud found mid-bin: discard the coarse sample and re-walk at the fine length —
                // from ONE BIN BACK, because the coarse sample sits at its bin's midpoint, so an
                // entry face in the previous bin's far half was sampled past and never walked. The
                // amount skipped varied 0..half a bin between neighbouring texels, and that
                // discontinuity printed the field's level sets as thin terrace rings through every
                // cloud body — the marbled-contour defect, worst on the rain sheet (2026-08-26).
                refined = true;
                wasEmpty = true;
                misses = 0;
                along = max(refinedUntil, along - coarseStep);

                continue;
            }

            along += stepLength;

            continue;
        }

        if (density <= 0.0)
        {
            misses++;
            if (misses >= 6)
            {
                refined = false;
                misses = 0;
            }

            wasEmpty = true;
            along += stepLength;
            refinedUntil = max(refinedUntil, along);

            continue;
        }

        misses = 0;

        if (wasEmpty)
        {
            // First sample inside after clear air: bisect the entry face and re-anchor the fine
            // ladder to it, so every sample position is a continuous function of the face rather
            // than of the bin grid. The grid's phase against the face is what survived the
            // one-bin back-up as fine-spaced terrace rings (2026-08-26); three halvings place
            // the anchor within an eighth of a fine step, and the ladder then starts half a step
            // before the found inside point so the next midpoint re-samples it.
            float outside = along - 0.5 * fineStep;
            float inside = along + 0.5 * fineStep;
            for (int halving = 0; halving < 3; halving++)
            {
                float middle = 0.5 * (outside + inside);
                if (cloudDensity(cameraPosition + rayDirection * middle, detailFade) > 0.0)
                {
                    inside = middle;
                }
                else
                {
                    outside = middle;
                }
            }

            wasEmpty = false;
            along = inside - 0.5 * fineStep;

            continue;
        }

        float sunDepth = cloudSunOpticalDepth(samplePosition, towardsLight);

        // Hillaire's octaves: contribution, extinction and asymmetry all halve together, so a
        // deep sample still receives the softened remnants of multiple scattering after single
        // scattering is spent.
        float sunTerm = 0.0;
        float attenuation = 1.0;
        for (int octave = 0; octave < cloudScatterOctaves; octave++)
        {
            float phase = mix(cloudPhase(cosTheta, cloudBackScattering * attenuation),
                              cloudPhase(cosTheta, cloudForwardScattering * attenuation), cloudLobeMix);
            sunTerm += attenuation * exp(-sunDepth * attenuation) * phase;
            attenuation *= cloudOctaveAttenuation;
        }

        // The powder term: a thin sunlit edge scatters out of its own neighbourhood and darkens,
        // which is the crisp cauliflower edge; deep in, it converges to 1 and changes nothing.
        float powder = 1.0 - cloudPowderStrength * exp(-2.0 * sunDepth);

        // Both halves are radiances: the sun's irradiance through 1/4pi and the normalised phase,
        // and the probe's mean radiance scaled up the slab — a cloud top sees the whole sky, a
        // base sees mostly its own kind.
        float shellHeight = clamp((samplePosition.y - cloudShellBase) / (cloudShellTop - cloudShellBase), 0.0, 1.0);
        vec3 sampleRadiance = cloudScatterAlbedo
            * (sunColour * cloudInverseSphere * sunTerm * powder + ambient * mix(0.18, 1.0, shellHeight));

        // Quilez's separated extinction and in-scattering: the step's own transmittance weights
        // what it adds, and what survives it multiplies through.
        float stepTransmittance = exp(-density * cloudExtinction * stepLength);
        radiance += sampleRadiance * transmittance * (1.0 - stepTransmittance);
        transmittance *= stepTransmittance;

        if (transmittance < 0.01)
        {
            break;
        }

        along += stepLength;
        refinedUntil = max(refinedUntil, along);
    }

    fragColor = vec4(radiance, transmittance);
}
