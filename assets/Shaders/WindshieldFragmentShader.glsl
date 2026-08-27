#version 450
// The inner pane of a windscreen: the grime on the glass, and nothing else.
//
// **This shader is not a glass shader**, and that is the whole reason it is small. Assetto Corsa's
// cars carry the windscreen as two near-coplanar shells — `GlassExt` outside and `GlassInt` inside —
// and the mod states a different shader for each: `ksPerPixelReflection` on the outer pane, which
// reflects the sky, and `ksWindscreen` on the inner one, which does *this*. The outer pane is still
// drawn by `pbr` and still carries the reflection, the Fresnel and the tint; what is left for this
// one is the dirt.
//
// The material this draws is asking for it by name — `extras.shader` on the glTF material, mapped by
// the exporter from AC's own `ksWindscreen` (see Material::declaredShader). Nothing here identifies a
// windscreen; the asset says which surfaces are one.
//
// **The grime is a function of the glass and not a texture on it** (2026-08-25). It used to be AC's
// own `dirty-glass*.dds` riding the base-colour slot — rgb the dirt's colour, alpha its coverage —
// and the contract is unchanged, only where the numbers come from. See `windscreenGrime` for why
// that had to move: a map is stated in UV, a unit of u is 24.374 m on this windscreen and something
// else on every other pane, and a function stated in metres is the right size on all of them.

layout(location = 0) out vec4 fragColor;

// Only what is read, for the reason ColourFragmentShader states: interpolants this never reads would
// sit in the SPIR-V until optimisation stripped them, so they are absent from the source instead.
// The locations are PassThroughVertexShader's own and are deliberately not contiguous.
layout(location = 0) in vec2 textureCoordinates;
layout(location = 1) in vec3 positionInWorldSpace;
layout(location = 2) in vec3 positionInViewSpace;
layout(location = 6) in vec3 normalsInWorldSpace;
// The pane as the *car* sees it: a point on it, its normal, and the car's own two axes, all in the
// model space of this primitive and all resolved in the vertex stage. Between them they are a
// complete body-fixed, metric description of the surface, and the whole of the rain is built on
// them.
//
// **This replaces a chain of three previous attempts, and the reason it is here is worth the
// paragraph.** The water needs a map from a direction (gravity, the airstream) into a coordinate
// system on the glass, and the previous answers all derived that map at shading time: first from
// the screen-space Jacobian, which jitters per quad; then from the interpolated tangent frame,
// which is smooth but is in *view* space and so turned with the camera; then from the tangent frame
// with its handedness resolved per fragment against the derivatives, which is smooth and
// camera-independent but leaves the sign of "down" resting on a screen-space quantity. Every one of
// those feeds a direction into a displacement that has been accumulating since the session started,
// and that product is unforgiving: it turns a wobble into a teleport and a wrong sign into water
// running up the glass. Model space has no such question in it. Up is up because the car says so,
// the coordinate is in metres because the vertices are, and neither depends on where anybody is
// looking from.
layout(location = 8 + MAX_LIGHTS) in vec3 positionInModelSpace;
layout(location = 9 + MAX_LIGHTS) in vec3 bodyUpInModelSpace;
layout(location = 10 + MAX_LIGHTS) in vec3 bodyForwardInModelSpace;
layout(location = 11 + MAX_LIGHTS) in vec3 normalsInModelSpace;

struct Light {
    vec4 position;             // xyz the direction *towards* the light
    vec4 diffuse;
    vec4 specular;
    vec4 ambientAttenuation;
};

// One light probe, for the sky's share of what lights the grime. Same layout as the scene shaders'.
struct Probe {
    vec4 irradiance[SH_COEFFICIENTS];
    vec4 boxMin;
    vec4 boxMax;               // w non-zero for the scene's global probe
    vec4 position;
};

// A prefix of the frame block, as far as the fields this reads. The fog fields are declared only
// to reach past them — this surface sits half a metre from the eye, where a medium measured in
// kilometres has nothing to say — because the rain fields are appended after the air and a prefix
// must carry everything above the field it wants.
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
    ivec4 probeParams;
    Probe probes[MAX_IBL_PROBES];
    vec4 fogDensity;
    vec4 fogScatter;
    vec4 fogAmbient;
    // x the simulated instant in seconds — the engine's tick count through its fixed step, never a
    // wall reading, so frame N of a capture is the same rain on every machine — y how hard it
    // rains, z the ground speed in m/s, and w the airflow phase: the accumulated integral of speed
    // squared, which is what a drop's drift is. The game integrates it a tick at a time because
    // this shader is stateless, and rebuilding a drift from the current speed alone would teleport
    // every drop the moment the pedal moved.
    vec4 timeRain;
    // xyz the car's forward direction in world space. Declared only to reach past the fields below
    // it: this stage takes the car's axes from the model-space varyings above, which are these same
    // two vectors resolved once per primitive rather than rebuilt per fragment.
    vec4 rainWind;
    // The two blades, as the geometry of their arcs and the law their angle follows — pivot in uv
    // with the inner and outer radii, where each parks and how far it sweeps, and the cycle. No
    // blade *angle*: that is a closed-form function of the clock above, and so is the question this
    // shader actually asks of it. See wiperBlade.
    vec4 wiperArcA;
    vec4 wiperArcB;
    vec4 wiperSweep;           // parkA, spanA, parkB, spanB, in radians
    vec4 wiperTiming;          // period, sweep seconds, cycle start, blade half width
    // x how many units of u one unit of v spans on this pane. Stated by the game from the asset
    // rather than recovered here from the screen-space Jacobian, which made the clearing depend on
    // the render resolution — see Wipers::paneAspect.
    vec4 wiperPane;
} frame;

// A prefix of the material block, as far as `textureTransform`. Declared this far so the offsets
// match the ABI even though only `baseColour` is read — it scales the grime, which is the one thing
// a material still gets to say about the dirt on its own glass.
//
// **No texture is bound here any more.** The grime was the base-colour slot's map; it is a function
// of the surface now (see `windscreenGrime`), so this material samples nothing at all. The engine
// still writes the descriptor, which is legal and costs nothing; what it buys is that re-exporting
// the car can no longer arrive with a clean windscreen because somebody forgot to override one
// texture by name.
layout(set = SET_MATERIAL, binding = 0) uniform MaterialData {
    vec4 baseColour;
    vec4 roughMetal;
    ivec4 useTextures;
    ivec4 useTextures2;
    mat4 textureTransform;
} material;

// The cascades, so the glare stops when the car does not have the sun on it. Every scene pipeline
// shares one layout, so this set is already bound for the pass.
layout(set = SET_SHADOW, binding = SHADOW_MAP_BINDING) uniform sampler2DShadow shadowMaps[SHADOW_CASCADES];

// The opaque scene as it stood when the blended draws began: what this pane is being composited
// over, copied out between the two halves of the pass precisely so that a blended surface can read
// it. Its mip chain is the choice of scattering cone — level 0 is the scene exactly, each level up
// is a cone twice as wide.
layout(set = SET_SHADOW, binding = SCENE_BEHIND_BINDING) uniform sampler2D sceneBehind;

// How sharply the grime scatters forwards. High, because that is what grime does and what the effect
// is: light that was already travelling towards the driver carries on towards the driver, so the
// windscreen glares when the car is pointed at the sun and is nearly clear when it is not. At zero
// the dirt would simply be a uniform grey film, which is the thing everybody's first windscreen
// shader looks like.
const float grimeAnisotropy = 0.75;

// A directional source of irradiance E scattered isotropically gives a radiance of E/4pi; the phase
// function below is stated relative to that isotropic case, so this is where the factor lives.
const float grimeInverseSphere = 0.0795774715;

// What the grime's own glare rolls off towards, in the same relative radiance everything else here
// works in. Pointed straight at a low sun the phase function is worth nearly thirty, and without a
// ceiling the pane goes to white and takes the bloom chain with it.
//
// **Down from 4.0, and the ceiling is a roll-off now rather than a clamp** (2026-08-25). Two faults,
// and the second is the one that mattered. The level was calibrated against a layer covering five
// percent of the pane; at the new coverage the same ceiling adds an order more light, which is the
// white sheet. And a hard `min` sets every sun-facing pixel to *exactly* the ceiling, so the dirt's
// own variation is clipped away and what is left is a flat wash — reported from the seat as fogging
// on the glass, twice. A roll-off keeps the ordering, so thick dirt still glares harder than thin
// and the texture survives into the highlight.
const float grimeMaximum = 0.9;

// How far a grazing view lengthens the path through the layer, at most. A ray crossing the pane
// obliquely travels through more grime than one crossing it square, which is why a dirty windscreen
// is always worst at its edges and towards the corners of your vision. Capped, because the honest
// 1/cos runs to infinity at the grazing limit.
const float grimeMaximumPathLength = 4.0;

// How much of the light the dirt blocks carries on through it, scattered, rather than being
// absorbed. High, because dust absorbs very little — it is soot and rubber marks that eat light,
// and they arrive here as the dark texels of the grime map, whose rgb multiplies this term and so
// does double duty as the transmission albedo.
const float grimeScatterFraction = 0.85;

// The scattering cone, as a level of the behind copy's chain. The forward lobe above is sharp but
// the pane sits half a metre from the eye and the world sits metres behind it, so a few degrees of
// cone is many pixels of blur; five reads 1/32nd resolution, and the true width — which depends on
// a background distance this pass does not know — is deliberately one authored number.
const float grimeConeLod = 5.0;

// ---- Rain ---------------------------------------------------------------------------------------
// **The drop model is Martijn Steinrucken's "Heartfelt"** (BigWings, @The_ArtOfCode, 2017; Shadertoy
// XljSD3), adapted from a screen to a pane. It is kept close to the original on purpose: the shapes,
// the trail profile, the beading and the layer weights are his, and every attempt here to author
// those from first principles has been worse. **Licence: CC BY-NC-SA 3.0 — attribution,
// non-commercial, share-alike.** That is a condition on this file and on anything built from it.
//
// What the original does, and why it reads as rain where a field of ellipses does not:
//
//  - **A drop owns a tall narrow column and travels down it**, so its trail is simply the part of
//    its own cell it has already passed through. No neighbourhood walk, no capsule arithmetic, and
//    a trail as long as the cell for the price of one cell.
//  - **The column scrolls while the drop moves within it**, and the two rates differ. The drop
//    creeps for most of its cycle and then runs, which is what water on glass actually does.
//  - **The trail narrows towards its far end and breaks into beads**, and both fall out of the same
//    two smoothsteps rather than being drawn separately.
//  - **A wiggle keyed to the pane's own coordinate**, not to the drop, so every drop passing a given
//    height leans the same way — the glass has channels in it, which is what water finds.
//  - **The refraction is the gradient of the coverage field itself**, taken by finite difference.
//    Drops, trails and beads all bend light correctly with one rule, and no shape needs a normal of
//    its own.
//  - **The glass between the drops is misted**, and water is where it is clear. This is most of why
//    the original reads as weather at all; a clear pane with drops on it reads as a dirty window.
//
// What is ours, and has to stay:
//
//  - The domain is `paneMetres` — the pane in metres in the car's own model space — so a cell is a
//    size on the glass and one number is right on the windscreen and the door glass at once.
//  - The scroll is the accumulated drift the car's physics produces, not a clock, so the water
//    answers to speed: gravity down, the airstream up, and a stall where they cancel.
//  - **The scroll is exactly uniform over a pane**, which is the fix the session before this one
//    landed and must not be undone. The airstream's heading is snapped to whichever of the pane's
//    own axes it lies nearest, so the displacement varies with nothing. A displacement that varies
//    across a curved surface is a shear whose size grows with session time, and it ends as parallel
//    scratches where the drops were.
//  - Hashing is our integer hash of the cell index, not `fract(sin(...))`: the house rule is that
//    anything animated is a function of the cell and the clock through an integer hash, so that a
//    captured frame N is the same rain on every machine and on every driver.

// One unit of the drop coordinate, in metres on the glass. The grid below cuts it into 12 columns
// by 2 rows, so a cell is **30 mm wide and 180 mm tall** — Heartfelt's own 6:1 column, at the size
// rain makes on a windscreen.
// Halved from the 0.36 the port arrived at, on a seat report that the drops were about twice the
// size they should be. Everything in the layer is stated as a fraction of a cell, so one number
// scales the heads, the beads and the trail widths together and keeps Heartfelt's proportions.
const float rainCellUnitMetres = 0.18;

// The second column layer, at Heartfelt's own scale. Its cells are smaller and, because the scroll
// it is handed is in unscaled units, it travels slower — the parallax that stops two layers reading
// as one.
const float rainSecondLayerScale = 1.85;

// The mist that lands and dries without ever running, in cells per unit — 12 mm cells. Halved with
// the unit above so the mist keeps the size it had rather than following the drops down.
const float rainStaticCells = 15.0;

// How long a mist drop's land-and-dry cycle takes, in seconds. The columns take their cycle from
// distance travelled instead, so they stall when the water stalls; this layer is about arriving and
// drying, which goes on whether or not the car is moving.
const float rainStaticCycleSeconds = 6.0;

// The field repeats every this many cells, so the cell index can be masked into a bounded integer
// and the coordinate wrapped into a bounded float. A thousand cells is 30 m across the columns and
// 180 m along them: no pane on any car will ever show the repeat, and the wrap is seamless because
// the hash is periodic on exactly the same count.
const uint rainIdMask = 1023u;
const float rainIdPeriod = 1024.0;

// A drop's two drift terms, **in metres per second along the glass**. Gravity's is fixed; the
// airstream's grows with the *square* of the speed because aerodynamic shear does, and the ratio of
// the two is the crossover: a drop on a windscreen stalls near 11 m/s, climbs above that, and is
// blown off the top at speed. Both are integrated upstream — the clock for gravity, the accumulated
// v² the frame block carries for the air — so the field drifts smoothly through any speed change.
//
// **Neither carries a per-fragment weighting, and giving that up is a real trade.** How much of
// gravity a piece of glass feels genuinely varies across a curved pane: measured on this windscreen
// it runs 0.40 at the top to 0.52 at the bottom. Multiplied into a displacement that has been
// accumulating all session, that 30% spread becomes fifteen cells of difference between the top of
// the pane and the bottom after a hundred seconds — twice the lattice's own gradient, so the drop
// spacing inverts and scrambles. Wrapping bounds it but cannot remove it, and a period short enough
// to make it negligible is short enough to repeat visibly. The underlying fact is that **a rigid
// slide of a stateless field cannot represent a velocity that varies over the surface**, and on
// curved glass it genuinely does. What is lost is that a steeply raked screen no longer runs its
// water slower than a vertical side window; the rate here is the compromise between the two.
const float rainSlideMetresPerSecond = 0.055;
const float rainAirflowResponse = 4.27e-4;

// How far the refracted sample walks, **as a fraction of the frame**, per unit of the coverage
// field's own gradient, and how wide the finite difference that measures that gradient is.
//
// **The unit is the whole point and it was wrong.** This used to be stated in metres on the glass,
// which sounds like the physical thing to measure and is not: a drop deflects a ray by an *angle*,
// and where that angle lands on screen is set by the camera, not by how big the drop is or how much
// of the pane a pixel covers. Measured, the metre form walked the sample **under one pixel** — no
// lens at all — and every drop was reading as nothing more than a sharp window in the misted glass.
// The old analytic model walked it up to about 3.5% of the frame, which is the scale that makes a
// drop show the little inverted world it should.
//
// The pane's own Jacobian is still used, but only for the *direction*: which way on screen a
// gradient across the glass points. Its scale is thrown away, which is what keeps a drop's lens the
// same strength whether the pane is close to the eye or far from it.
const float rainRefractionScreen = 0.35;
const float rainGradientStep = 0.0015;

// The most of the frame one drop may walk its sample. Uncapped, the steepest edges reach a sixth of
// the screen and the rim reads as whatever happened to be standing there — a drop that comes and
// goes with the view behind it rather than with the water.
const float rainRefractionMaximum = 0.055;

// The mist on the glass, and how sharply the water reads through it. `rainFogLod` is the level of
// the behind copy's chain the misted glass reads — each level is a cone twice as wide — and
// `rainDropLod` is what a drop reads, which is nearly the scene itself. A trail cuts the mist, which
// is the third of Heartfelt's three ideas and the one that makes a trail read as *cleared* rather
// than as drawn.
const float rainFogLod = 4.0;
const float rainDropLod = 0.5;
const float rainFogAlpha = 0.34;

// **What stops a drop reading as a black hole when the car points at the sun.** A drop is not a
// perfect lens: some of what it gathers arrives from the whole cone it can see rather than from the
// one point its curvature aims at. Without that share, a drop sitting on a blown-out sky walks its
// sample a few pixels off the sun disc, finds something orders darker, and prints it as a black bead
// — reported from the seat, 2026-08-25. The share is read from a wide level of the same chain, so
// near a bright source a drop *glows*, which is what a real one does.
const float rainDropHaloShare = 0.30;
const float rainDropHaloLod = 3.0;

// How bright a rim the sky puts on a drop's edge. Small, and it used to be four times this: at the
// old value the drops wore white rings hard enough to read as an outline rather than as water
// (seat, 2026-08-25). It is kept at all because a drop over a featureless background refracts that
// background into itself and disappears, and this term is view-independent so an edge survives.
const float rainRimSky = 0.12;

uint rainHash(uvec2 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return v.x ^ v.y;
}

float rain01(uvec2 v)
{
    return float(rainHash(v) & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

// A cell's identity, masked into one period of the field so that the coordinate wrap below lands on
// exactly the same drops. The offset is there so a negative index still hashes.
uvec2 rainCellKey(ivec2 cellIndex, uint seed, uint mask)
{
    uvec2 cell = uvec2(cellIndex + 4096) & uvec2(mask);

    return cell * 1973u + uvec2(seed, seed * 2654435769u);
}

// Three uncorrelated values for one cell — Heartfelt's `N13`, through our integer hash.
vec3 rainCellNoise(ivec2 id, uint seed)
{
    uvec2 key = rainCellKey(id, seed, rainIdMask);

    return vec3(rain01(key), rain01(key + 17u), rain01(key + 101u));
}

// Rise, hold, fall: Heartfelt's `Saw`. Used as a *position* along a column, so a drop crosses its
// cell over the first fraction `b` of its cycle and returns over the rest.
float rainSaw(float b, float t)
{
    return smoothstep(0.0, b, t) * smoothstep(1.0, b, t);
}

// The mist: one drop per cell, landing and drying on its own schedule, never moving. Heartfelt's
// `StaticDrops`.
float rainStaticDrops(vec2 uv, float cycle)
{
    uv *= rainStaticCells;

    ivec2 id = ivec2(floor(uv));
    uv = fract(uv) - 0.5;

    vec3 n = rainCellNoise(id, 181u);
    vec2 p = (n.xy - 0.5) * 0.7;
    float d = length(uv - p);

    float fade = rainSaw(0.025, fract(cycle + n.z));

    return smoothstep(0.3, 0.0, d) * fract(n.z * 10.0) * fade;
}

// One layer of running drops, and this is Heartfelt's `DropLayer2` almost line for line.
//
// `uv` is the pane in units of `rainCellUnitMetres`, unscrolled; `scroll` is how far the water has
// been carried along the columns, in the same units and signed, so that a car at speed runs its
// water up the glass by making this negative. Returns x the drop mask and y the trail mask.
vec2 rainDropColumns(vec2 uv, float scroll, float streaming, uint seed)
{
    // The unscrolled coordinate, kept because two things are properties of the *glass* and not of
    // the water: which way a drop leans as it passes a given height, and where the trail beads.
    vec2 UV = uv;

    uv.y += scroll;

    vec2 a = vec2(6.0, 1.0);
    vec2 grid = a * 2.0;

    // Into one period, so the lattice never leaves the range a float resolves and the masked hash
    // agrees with the coordinate. The wrap is a whole number of cells, so `fract` below does not
    // notice it.
    vec2 period = vec2(rainIdPeriod) / grid;
    uv -= floor(uv / period) * period;

    ivec2 id = ivec2(floor(uv * grid));

    // Each column starts its cycle somewhere else, so the pane never runs a row of drops abreast.
    float columnShift = rain01(rainCellKey(ivec2(id.x, 0), seed + 7u, rainIdMask));
    uv.y += columnShift;
    uv -= floor(uv / period) * period;
    id = ivec2(floor(uv * grid));

    vec3 n = rainCellNoise(id, seed);
    vec2 st = fract(uv * grid) - vec2(0.5, 0.0);

    float x = n.x - 0.5;

    // The wiggle, keyed to the pane rather than to the drop: every drop crossing a given height
    // leans the same way, so the glass reads as having channels in it.
    float y = UV.y * 20.0;
    float wiggle = sin(y + sin(y));
    x += wiggle * (0.5 - abs(x)) * (n.z - 0.5);
    x *= 0.7;

    // Where the drop sits in its column. The cycle is driven by distance travelled and not by a
    // clock, so water that has stalled stays where it is.
    //
    // **The eased saw is what makes a drop creep, hold, and then run**, because a smoothstep has no
    // slope at either end: over most of a cycle the drop gains only a third of a cell on its column
    // and in the last sixth it gains three times that. On a parked or crawling screen that is
    // exactly right, and it is most of what the port was adopted for.
    //
    // At speed it is wrong, and it was reported as such: the airstream does not let a drop settle,
    // it drags it off the glass without pause. So the excursion is damped out as the drift takes
    // over — at full stream the drop holds station in its own column and simply travels with it,
    // which is one uninterrupted motion. `colShift` is what keeps that from reading as a row of
    // drops abreast, since every column is offset by its own amount.
    float ti = fract(scroll / 0.75 + n.z);
    float ease = (rainSaw(0.85, ti) - 0.5) * 0.9;
    y = 0.5 + ease * (1.0 - streaming * 0.85);
    vec2 p = vec2(x, y);

    // The cell is six times taller than it is wide, so the distance is unstretched to make the head
    // round on the glass.
    //
    // **The three radii below are half Heartfelt's**, on a seat report that the drops were still too
    // large. Halving them here rather than halving the cell is deliberate: a smaller cell would take
    // the trail length and the drop spacing down with it, and both of those are what the port was
    // adopted for. So the column keeps its 15 by 90 mm and the water inside it gets smaller — a head
    // 6 mm across where it was 12.
    //
    // It also *strengthens* the lens rather than weakening it, which is the other half of the same
    // report. The refraction is the coverage field's own gradient over a fixed step, so an edge that
    // falls in half the distance has twice the gradient, and a small drop bends light harder than a
    // large one. That is true of real drops for the same reason.
    float d = length((st - p) * a.yx);
    float mainDrop = smoothstep(0.2, 0.0, d);

    // The trail is the part of the column the drop has already crossed. It is widest at the head and
    // closes to nothing at the far end, which is the whole of why it reads as drying.
    float r = sqrt(smoothstep(1.0, y, st.y));
    float cd = abs(st.x - x);
    float trail = smoothstep(0.115 * r, 0.075 * r * r, cd);
    float trailFront = smoothstep(-0.02, 0.02, st.y - y);
    trail *= trailFront * r * r;

    // The beads the trail breaks into, spaced on the pane rather than on the drop.
    y = fract(UV.y * 10.0) + (st.y - 0.5);
    float droplets = smoothstep(0.15, 0.0, length(st - vec2(x, y)));

    return vec2(mainDrop + droplets * r * trailFront, trail);
}

// The layers together: mist, and column layers at two scales. Heartfelt's `Drops`. Returns x how
// much water covers this texel and y how much of it is trail, which is what cuts the mist.
//
// **The columns lie along whichever way the water is actually going, and that is the answer to the
// side windows.** A column carries its drop and its trail along its own long axis, so a set of
// columns running up and down the glass can only draw water that runs up and down the glass. On a
// door window the airstream runs *backwards*, across those columns — so at speed the field could
// only slide sideways as a whole, with no drop overtaking another and no trail pointing the way it
// went. Reported from the seat as the surface's own coordinates shifting, 2026-08-25, and that
// description was exact.
//
// So the same layers are evaluated a second time with the pane's two axes swapped, and the pair is
// crossfaded on how much of the travel runs across the glass. A windscreen never reaches the second
// set — its water is vertical whether it falls or is blown — and pays for a branch, not for the
// work. A door window crosses over as the car gains speed, which takes as long as the drift takes
// to turn, and during it some water is still falling while some has begun to stream: which is what
// a window does at walking pace.
vec2 rainDrops(vec2 uv, vec2 scroll, float acrossShare, float streaming, float staticCycle,
               float mist, float layer1, float layer2)
{
    float s = rainStaticDrops(uv, staticCycle) * mist;

    vec2 down1 = rainDropColumns(uv, scroll.y, streaming, 271u) * layer1;
    vec2 down2 = rainDropColumns(uv * rainSecondLayerScale, scroll.y, streaming, 977u) * layer2;
    vec2 water = vec2(down1.x + down2.x, max(down1.y * mist, down2.y * layer1));

    if (acrossShare > 0.01)
    {
        vec2 back1 = rainDropColumns(uv.yx, scroll.x, streaming, 613u) * layer1;
        vec2 back2 = rainDropColumns(uv.yx * rainSecondLayerScale, scroll.x, streaming, 1471u) * layer2;
        water = mix(water, vec2(back1.x + back2.x, max(back1.y * mist, back2.y * layer1)), acrossShare);
    }

    return vec2(smoothstep(0.3, 1.0, s + water.x), water.y);
}

// ---- Wipers -------------------------------------------------------------------------------------
// A wiper sweeps a fixed arc at a fixed rate, so where the blade stands at time t is a closed-form
// function of t — and so, by inverting the same function, is the question this shader actually needs
// answered: **when was this point of the glass last swept?** Everything the wiper does follows from
// that one number. A drop is drawn if it landed after it; the grime is thinner the more recently it
// happened; and outside the arc it is minus infinity, which is the un-wiped border every dirty
// windscreen carries, for free and without a mask.
//
// The sweep is a raised cosine over `sweepSeconds`, out and back in one pass:
//
//     angle(tau) = park + span * (1 - cos(2 pi tau / sweepSeconds)) / 2
//
// Not linear, because a crank linkage decelerates into its reversals and a linear sweep reads as a
// metronome. Inverting it for a point at arc position `along` in [0,1] gives the two instants per
// cycle the blade crosses it — one outbound, one on the way back — and `acos` is the whole
// inversion. That is the entire mechanism: no accumulation buffer, no stroke history, nothing that
// can drift out of step under a capture.
const float wiperTwoPi = 6.28318530718;

// How dark the blade draws, and how far it is worth reading a fresh film of water for.
const float wiperBladeOpacity = 0.85;
const float wiperFilmSeconds = 0.30;
const float wiperFilmOpacity = 0.10;

// What a pass leaves the grime at, and how long the road's spray takes to put it back.
const float grimeWipedCoverage = 0.1;
const float grimeRegrowSeconds = 20.0;

// How long the glass stays swept behind the blade before water is back on it.
//
// **This is the whole of what clears the rain, and it has to be, because no drop here carries a
// birth time.** Every drop is reconstructed from the distance the water has travelled rather than
// from when it landed, so "did this arrive since the blade passed?" is not a question the field can
// answer. Scaling the water's coverage by how long ago the blade came past is the mask that does
// the job instead: nothing at all at the blade's own edge, back to full over this long.
//
// Raised from 0.55 s on a seat report that the swept band filled in too quickly. It is also the
// honest number: a blade leaves a film that beads before it runs, and the glass behind a wiper stays
// readable for seconds, not for half of one.
const float rainRewetSeconds = 2.5;

// One blade. Returns x the absolute time this fragment was last swept, y how far the blade is from
// it right now along the glass in units of u, and z one when this fragment is inside the arc at all
// and zero when it is not.
//
// **The validity flag is z rather than a sentinel in x, and every range test is written to reject
// NaN**, which is a correction rather than a style (2026-08-25). Written the obvious way round —
// `if (radius < inner || radius > outer) return never;` — a NaN radius passes *both* comparisons
// and walks into the arithmetic below, and `min`/`max` against a sentinel then propagate it or
// silently swallow it depending on argument order. Phrased as `!(radius >= inner && radius <=
// outer)`, NaN fails the test and is rejected, which is the only form that is safe. What it cost to
// find: the clearing worked at one render resolution and not another, and the pane read as
// permanently unwiped rather than as anything obviously broken.
//
// `arc` is the pivot in uv and the inner and outer radii; `aspect` scales a difference in v into u's
// own spacing so that a circle here is a circle on the glass rather than on the texture. The arc
// must not straddle atan's cut at +/-pi — stated where the arc is stated, since a windscreen wiper
// sweeping through "pointing left" is a car nobody builds.
vec4 wiperBlade(vec2 uv, float aspect, vec4 arc, float park, float span, vec4 timing, float t)
{
    float period = timing.x;
    float sweepSeconds = timing.y;

    // A cycle with no time in it and a sweep through no angle are both divisions waiting to happen.
    if (!(period > 0.0) || !(sweepSeconds > 0.0) || !(abs(span) > 1.0e-6)) {
        return vec4(0.0, 0.0, 0.0, 0.0);
    }

    vec2 d = vec2(uv.x - arc.x, (uv.y - arc.y) * aspect);
    float radius = length(d);
    if (!(radius >= arc.z && radius <= arc.w)) {
        return vec4(0.0, 0.0, 0.0, 0.0);
    }

    float angle = atan(d.y, d.x);
    float along = (angle - park) / span;
    if (!(along >= 0.0 && along <= 1.0)) {
        return vec4(0.0, 0.0, 0.0, 0.0);
    }

    float elapsed = t - timing.z;
    float cycle = floor(elapsed / period);
    float phase = elapsed - cycle * period;

    // Where the blade stands now. Past the sweep it is parked, which is the dwell an intermittent
    // setting spends waiting and a continuous one never reaches.
    float now = phase < sweepSeconds
        ? park + span * (0.5 - 0.5 * cos(wiperTwoPi * phase / sweepSeconds))
        : park;

    // The inversion: the outbound crossing, and its mirror on the return.
    float outbound = sweepSeconds * acos(clamp(1.0 - 2.0 * along, -1.0, 1.0)) / wiperTwoPi;
    float back = sweepSeconds - outbound;

    // The most recent of the two that has actually happened. Before either, the last pass belongs
    // to the previous cycle — and it is the *return* stroke that was last, not the outbound one.
    float last = phase >= back ? back : (phase >= outbound ? outbound : back - period);
    float lastPass = timing.z + cycle * period + last;

    // **A pass before the wipers were switched on never happened.** The line above is happy to
    // reach back into the previous cycle, and on the very first stroke there is no previous cycle —
    // so every point of the arc claimed a wipe the instant the stalk moved, and the whole swept
    // pattern appeared at once instead of following the blade across the glass. Reported from the
    // seat, 2026-08-25. `cycleStart` is the earliest instant a blade can have touched anything, so
    // anything older than it means this point is inside the arc and simply has not been reached
    // yet — which is w, kept separate from z precisely so the blade still *draws* there.
    return vec4(lastPass, abs(angle - now) * radius, 1.0, lastPass >= timing.z ? 1.0 : 0.0);
}

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

// The sky's share of what lights the grime: the global probe's band-0 coefficient, which is the mean
// of the sphere it photographed. The same term the fog uses for the same reason — it is the
// isotropic in-scattering colour by definition, and it costs nothing that is not already in the
// block. Without it the dirt would be invisible except when pointed at the sun.
vec3 grimeSkyRadiance()
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

// ---- Dirt ---------------------------------------------------------------------------------------
// **The grime is a function of the glass now, not a photograph of one**, and the reason is the same
// reason the rain is: a texture is stated in UV, and a unit of u is 24.374 m on this windscreen and
// something else on every other pane, so one map cannot be the right size twice. Stated in metres on
// the surface it is right everywhere by construction, it costs no texture slot, and it can be *cut*
// — which a sampled map cannot be, and which is what lets running water take the dirt with it.
//
// It also removes a trap. `uploadMaterialTextures` enumerates a material's texture slots by hand,
// and the grime map rode the base-colour slot; anything that re-exported the car had to remember to
// override that texture or the pane came back clean.

// How coarse the dirt is, in features per metre of glass. **Three scales, because dirt has three**:
// where it gathers, how it spatters within that, and the specks themselves. One scale is a fog —
// reported from the seat, 2026-08-25, and the arithmetic agrees: the blotches used to start at 2.5
// per metre, which makes the base feature wider than the visible pane, so the whole screen landed on
// one mid value and read as an even haze.
//
// The blotches are **stretched across the glass** on purpose. Dirt on a screen that has been wiped
// is banded along the arc, not spotted evenly, so the field runs long in the pane's across axis and
// short up it. That one asymmetry is most of what makes it read as wiped dirt rather than weather.
const float grimeBlotchesPerMetre = 6.0;
const vec2 grimeBlotchStretch = vec2(0.32, 1.9);
const float grimeSpatterPerMetre = 55.0;

// Two speck scales, and **both are sized against the pixel and not against the millimetre**. At 220
// cells per metre a speck came out 0.9 to 2.9 mm, which on a windscreen filling about a thousand
// pixels per metre is one to three pixels — under the sampling rate, so it does not read as grit, it
// reads as nothing at all. That is why adding specks did not add detail. The coarse pass is grit you
// can pick out, the fine pass is the grain under it.
const float grimeSpecksPerMetre = 85.0;
const float grimeFineSpecksPerMetre = 190.0;

// How many of the cells carry a speck at all, and how much of the pane one covers. Specks are what
// stop the dirt reading as a wash however it is shaped: every edge in a field of filtered noise is a
// gradient, and these are the only hard ones in it.
const float grimeSpeckDensity = 0.28;
const float grimeSpeckCoverage = 0.45;
const float grimeFineSpeckDensity = 0.40;
const float grimeFineSpeckCoverage = 0.28;

// Dirt gathers in patches, so the blotch field is pushed through a curve rather than used flat. A
// linear field of noise reads as an even haze, which is the grey film this whole shader exists not
// to be — the curve is what keeps the dirt in patches however much of it there is, and the *narrower*
// the window between the two, the harder the edge of a patch.
//
// **Back to ordinary levels** on Dominic's call, 2026-08-25, after a spell set filthy to find out
// what the model did at the top of its range. That spell is what found the two real faults — the
// near-white tint and the clamped glare — so what is left is the same amount of dirt as before it,
// drawn by a model that survives being turned up.
//
// The window is how much of the glass is dirty at all, and the peak is how thick it gets where it
// gathers. The window stays *narrow* rather than returning to the wide one it started with: narrow
// is what gives a patch an edge, and it is moved up rather than opened out, so less glass is dirty
// without the dirt going back to a gradient.
//
// The peak stops short of one on purpose. `coverage` is the pane's own alpha, so at one the glass
// would be opaque and the sharp scene behind it would be gone entirely rather than showing through
// a veil — and the outer pane's reflection with it, since that draws blended and is absent from the
// copy this samples.
const float grimePatchLow = 0.46;
const float grimePatchHigh = 0.72;
const float grimePeakCoverage = 0.34;

// What road film looks like: warm, desaturated, and **dark**. This is the level as well as the hue,
// and getting it wrong is what made a very dirty screen read as a white one.
//
// The map this replaced sat near white — mean 231 of 255 — because it was authored as a *glare
// gain* for a layer covering five percent of the pane, where the level barely shows. Copied into a
// layer covering nearly forty percent it stops being a gain and starts being an albedo: the pane
// then scatters the sky like a lit white sheet, which is exactly what a filthy windscreen does not
// look like. Road film is soot, rubber and brake dust, and it is a dark grey-brown.
//
// **Only the glare follows this level, not the diffusion.** `grimeTint` divides the peak channel
// out before the transmitted term uses it, so darkening here leaves the blurring of the scene
// behind the glass untouched and takes only the light the dirt adds of its own. That is the right
// lever: a dirty screen is dirty because it *diffuses*, not because it glows.
const vec3 grimeDarkTint = vec3(0.0, 0.0, 0.0);
const vec3 grimeLightTint = vec3(0.0, 0.0, 0.0);

// How much of the dirt the water takes with it: along the channels the drops run in, and only
// there. An `underWater` term stood beside this — grime erased under whatever water was standing
// on the glass *right now*, at full strength — and it was exactly the fault the channel comment
// below warns against: keyed to the instantaneous field, the dirt vanished under each travelling
// drop and healed behind it, so from the seat the whole dirt pattern swam whenever the car moved
// and the water streamed (Dominic's report, 2026-08-25 — visible only with the wipers off,
// because a wiped screen has almost no grime left to swim). Removed rather than weakened: the
// rain composites over the grime later in this shader, so a drop already covers the dirt it
// stands on, and the channels are the durable statement of what the water has taken.
const float grimeWashStreak = 0.85;

// Value noise on the pane, in metres, through the same integer hash the rain uses — never a
// driver's `sin()`, for the reason stated where the rain hash is.
float grimeNoise(vec2 p, uint seed)
{
    vec2 corner = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    ivec2 i = ivec2(corner);
    float a = rain01(rainCellKey(i, seed, 0xFFFFu));
    float b = rain01(rainCellKey(i + ivec2(1, 0), seed, 0xFFFFu));
    float c = rain01(rainCellKey(i + ivec2(0, 1), seed, 0xFFFFu));
    float d = rain01(rainCellKey(i + ivec2(1, 1), seed, 0xFFFFu));

    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float grimeOctaves(vec2 p, uint seed)
{
    float sum = 0.0;
    float amplitude = 0.5;

    for (int octave = 0; octave < 4; octave++)
    {
        sum += amplitude * grimeNoise(p, seed + uint(octave) * 131u);
        p *= 2.07;
        amplitude *= 0.5;
    }

    return sum;
}

// The specks: sparse hard dots, one cell each, most cells empty. **They are the only part of the
// dirt with a hard edge**, and that is what they are for — a field built only from filtered noise
// stays a wash however it is shaped, because every edge in it is a gradient.
float grimeSpecks(vec2 p, uint seed, float density)
{
    ivec2 cell = ivec2(floor(p));
    vec2 f = fract(p);

    uvec2 key = rainCellKey(cell, seed, 0xFFFFu);
    if (rain01(key + 19u) > density)
    {
        return 0.0;
    }

    vec2 centre = vec2(rain01(key), rain01(key + 7u));
    float radius = mix(0.14, 0.40, rain01(key + 31u));

    return smoothstep(radius, radius * 0.35, length(f - centre));
}

// What the dirt looks like and how much of the pane it covers, at a point on the glass measured in
// metres. rgb is the glare gain, alpha the coverage — the same contract the map it replaces had.
vec4 windscreenGrime(vec2 metres)
{
    // Where it gathers, stretched across the glass the way a blade leaves it.
    float blotches = grimeOctaves(metres * grimeBlotchesPerMetre * grimeBlotchStretch, 11u);

    // How it breaks up within a patch, and the two scales of grit that give it an edge.
    float spatter = grimeOctaves(metres * grimeSpatterPerMetre, 53u);
    float grit = grimeSpecks(metres * grimeSpecksPerMetre, 71u, grimeSpeckDensity);
    float grain = grimeSpecks(metres * grimeFineSpecksPerMetre, 149u, grimeFineSpeckDensity);

    // Named `gathered` and not `patch`, which is a reserved word here.
    float gathered = smoothstep(grimePatchLow, grimePatchHigh, blotches);
    float broken = mix(0.15, 1.0, smoothstep(0.34, 0.72, spatter));

    // The grit sits *over* the patches rather than inside them, so a clean stretch of glass still
    // carries a scatter of it — which is what makes the clean parts read as glass and not as a hole
    // in the dirt. The fine grain is scaled by the patch instead, since dust settles where dust is.
    float coverage = grimePeakCoverage * gathered * broken;
    coverage = max(coverage, grit * grimeSpeckCoverage);
    coverage = max(coverage, grain * grimeFineSpeckCoverage * mix(0.35, 1.0, gathered));

    return vec4(mix(grimeDarkTint, grimeLightTint, spatter), clamp(coverage, 0.0, 1.0));
}

// One column of the drop lattice, as how much water it carries. The columns are fixed in the pane —
// only the water inside them travels — so this is a *stable* pattern, which is what a wash has to be
// when the field carrying it holds no memory. Take the wash from where the drops are this instant
// instead and the dirt would come back the moment each drop moved on.
float grimeWashChannel(float along)
{
    float x = along * 12.0;
    float carries = rain01(rainCellKey(ivec2(int(floor(x)), 0), 331u, rainIdMask));
    float across = abs(fract(x) - 0.5) * 2.0;

    return smoothstep(0.95, 0.15, across) * smoothstep(0.35, 0.85, carries);
}

// How much of the dirt the rain has taken off this piece of glass. **Water washes a windscreen in
// streaks and not evenly**, because it runs in channels and the glass between them stays dirty —
// which is what a dirty screen in rain actually looks like, and what an even fade would lose. The
// channels are the whole of it now; the instantaneous under-the-water term is gone, and the note
// above `grimeWashStreak` says what it cost.
float grimeWash(vec2 dropUv, float acrossShare, float intensity)
{
    float channel = mix(grimeWashChannel(dropUv.x), grimeWashChannel(dropUv.y), acrossShare);

    return clamp(intensity, 0.0, 1.0) * channel * grimeWashStreak;
}

void main()
{

    // ---- The pane, as the car sees it -----------------------------------------------------------
    // Everything the rain and the wipers need about the surface, in the model space of this
    // primitive: body-fixed, in metres, and owing nothing to the camera, to the car's attitude or
    // to a screen-space derivative. See the varyings for the three attempts this replaces.
    vec3 paneNormalModel = normalize(normalsInModelSpace);
    vec3 bodyUpModel = normalize(bodyUpInModelSpace);
    vec3 bodyForwardModel = normalize(bodyForwardInModelSpace);

    // Across the glass, then up it. `cross(normal, across)` is the tangential part of the body's up
    // by construction, so **"down the pane" is exactly the negative second axis and cannot come out
    // wrong** — which is the whole reason the domain moved here, and the answer to water running up
    // a parked windscreen. A pane lying flat has no up the glass at all; the body's forward stands
    // in there, so a sunroof drains towards the back of the car, which is what one does.
    vec3 paneAcross = cross(bodyUpModel, paneNormalModel);
    float paneAcrossLength = length(paneAcross);
    paneAcross = paneAcrossLength > 1.0e-3 ? paneAcross / paneAcrossLength
                                           : normalize(cross(bodyForwardModel, paneNormalModel));
    vec3 paneUp = cross(paneNormalModel, paneAcross);

    // The surface in metres. On curved glass this distorts, and the distortion is the honest kind:
    // a drop field on a curved pane genuinely converges and diverges, and the amount is bounded by
    // the curvature rather than by how long the session has been running.
    vec2 paneMetres = vec2(dot(positionInModelSpace, paneAcross), dot(positionInModelSpace, paneUp));

    // The airstream blows backwards along the car's own axis, and only panes standing in it feel
    // it: the windscreen takes the ram flow, the rear window rides in its own wake and keeps
    // gravity, and the door panes have it running straight down them.
    //
    // **The sign is measured, not assumed** (2026-08-25): this asset's interior panes are wound so
    // their normals point *into* the cabin, so a forward-facing pane reads positive in `facing`
    // below. Assuming the other convention is what left the airflow term switched off on the one
    // pane it was written for, invisibly, because a still cannot tell a drop that will not move
    // from one that is not moving yet. The same quantity is what tells the wiper which pane is the
    // windscreen.
    vec3 airflowModel = -bodyForwardModel + paneNormalModel * dot(bodyForwardModel, paneNormalModel);

    // Which side of the car this pane is on: positive faces into the wind, zero is edge-on,
    // negative sits in the wake.
    float facing = -dot(paneNormalModel, bodyForwardModel);

    // How much airstream this piece of glass is standing in. A **wake test, not a cosine gate** —
    // and that distinction is the whole of Dominic's report that side windows ran the wrong way
    // (2026-08-25). What moves a drop along the glass is tangential shear, so a pane lying edge-on
    // to the flow feels the *most* of it, not the least: the side windows have the airstream running
    // straight down them towards the back of the car. Gating the strength on how much a pane *faces*
    // the wind had it exactly inverted, and nearly cancelled the side glass. What the normal is
    // genuinely needed for is the far side of the car — a rear window sits in separated flow and
    // must keep its gravity — so this falls away only for panes that turn their back on the stream.
    float exposure = smoothstep(-0.6, 0.0, facing);

    // Still the plain facing term, and only for deciding which pane is the windscreen.
    float windward = max(facing, 0.0);

    // The airstream's heading across the glass, in the pane's own two axes. Gravity needs no such
    // line: it is the negative second axis, exactly, by how the axes were built.
    vec2 flowPane = vec2(dot(airflowModel, paneAcross), dot(airflowModel, paneUp));
    flowPane = dot(flowPane, flowPane) > 1.0e-12 ? normalize(flowPane) : vec2(0.0, 1.0);

    // Where this fragment reads the copy of the scene it is standing in front of. The two share
    // pixel coordinates by construction — the copy was blitted 1:1 from this very attachment.
    vec2 behindUv = gl_FragCoord.xy / vec2(textureSize(sceneBehind, 0));

    // The wipers, and everything about them falls out of one number: when this point was last
    // swept. One branch on the scene's own statement, and the windward test is what says this pane
    // is the windscreen rather than a side window that happens to land nearby in the atlas.
    // `wiped` is false where no blade reaches, and every reader below branches on it rather than on
    // a sentinel time — which is what keeps "this point has never been swept" a statement instead of
    // a very large number that arithmetic can turn into something else.
    bool wiped = false;
    float lastWipe = 0.0;
    float bladeGap = 1.0e9;
    if (frame.wiperTiming.x > 0.0 && windward > 0.15)
    {
        vec4 bladeA = wiperBlade(textureCoordinates, frame.wiperPane.x, frame.wiperArcA, frame.wiperSweep.x,
                                 frame.wiperSweep.y, frame.wiperTiming, frame.timeRain.x);
        vec4 bladeB = wiperBlade(textureCoordinates, frame.wiperPane.x, frame.wiperArcB, frame.wiperSweep.z,
                                 frame.wiperSweep.w, frame.wiperTiming, frame.timeRain.x);

        // Whichever blade passed most recently owns this point; whichever is nearest draws it. Taken
        // one blade at a time so a fragment only one of them reaches never mixes a real answer with
        // an absent one — and drawing keys on z (inside the arc) where clearing keys on w (actually
        // reached), so on the first stroke the blade is visible ahead of the water it has yet to
        // take.
        if (bladeA.z > 0.5)
        {
            bladeGap = bladeA.y;
        }

        if (bladeB.z > 0.5)
        {
            bladeGap = min(bladeGap, bladeB.y);
        }

        if (bladeA.w > 0.5)
        {
            wiped = true;
            lastWipe = bladeA.x;
        }

        if (bladeB.w > 0.5)
        {
            lastWipe = wiped ? max(lastWipe, bladeB.x) : bladeB.x;
            wiped = true;
        }
    }

    // Outside the arc there is no age, and the readers below take the un-wiped branch: full grime,
    // no film, and nothing cleared — the border every dirty windscreen carries, for free.
    float wipeAge = wiped ? frame.timeRain.x - lastWipe : 0.0;

    // ---- The water ------------------------------------------------------------------------------
    // Evaluated before the dirt, because running water takes dirt with it and the dirt cannot be
    // asked how clean it is until the water has said where it has been. One branch on the scene's
    // own statement, so a dry scene is bit-for-bit the shader that had no rain in it.
    vec2 water = vec2(0.0);
    vec2 waterGradient = vec2(0.0);
    vec2 dropUv = vec2(0.0);
    vec2 refracted = behindUv;
    float acrossShare = 0.0;
    float waterLod = 0.0;
    float rainAlpha = 0.0;

    if (frame.timeRain.y > 0.0)
    {
        float time = frame.timeRain.x;
        float intensity = min(frame.timeRain.y, 1.0);
        float speed = frame.timeRain.z;
        float airflowPhase = frame.timeRain.w;

        // **The airstream's heading on this pane, snapped to whichever of the pane's own two axes it
        // lies nearest.** That snap is what makes the displacement below exactly uniform, and
        // uniform is the whole requirement: it multiplies an accumulation that grows all session, so
        // a heading that varied by even a few degrees across curved glass would shear the field
        // apart over minutes. The snap costs nothing real, because the answer it rounds to is the
        // one the geometry already gives — dead up the windscreen, dead backwards along the door
        // glass — and neither the dominant axis nor its sign can change while the car is the shape
        // it is.
        vec2 flowAxis = abs(flowPane.x) > abs(flowPane.y) ? vec2(sign(flowPane.x), 0.0)
                                                          : vec2(0.0, sign(flowPane.y));

        // How far the water has been carried, in metres on the glass, and what it is doing now.
        // Gravity's direction needs no vector: the pane's second axis points up it by construction,
        // so down is exactly the negative of it, and no arrangement of asset conventions, camera
        // angles or derivative noise can make water climb a parked windscreen.
        //
        // Past the crossover the sum turns and the water climbs off the top of the pane; right at it
        // the displacement stops growing and the drops hold station, which is the stall.
        vec2 driftShift = vec2(0.0, -rainSlideMetresPerSecond * time)
                        + flowAxis * (rainAirflowResponse * exposure * airflowPhase);

        // Heartfelt's coordinate: the pane in metres, in units of one cell block. Each of the two
        // column orientations takes the travel along its own axis, so the field is carried and never
        // merely dragged — see `rainDrops` for why a single orientation could not do it.
        dropUv = paneMetres / rainCellUnitMetres;
        vec2 scroll = -driftShift / rainCellUnitMetres;

        // How much of the travel runs across the glass rather than up and down it, which is what
        // decides between the two orientations. It is a property of the drift *now*, so it changes
        // only as fast as the drift turns, and on a windscreen it is identically zero.
        vec2 driftVec = vec2(0.0, -rainSlideMetresPerSecond)
                      + flowAxis * (rainAirflowResponse * speed * speed * exposure);
        float driftRate = length(driftVec);
        acrossShare = driftRate > 1.0e-6 ? smoothstep(0.2, 0.8, abs(driftVec.x) / driftRate) : 0.0;

        // How hard the water is being dragged, against what gravity alone would do. One is a parked
        // screen, where a drop is free to creep and hold and run; above that the airstream is
        // winning and a drop does not get to settle. Below one the two forces are cancelling, which
        // is the stall, and a drop settles more rather than less — which is right, since at the
        // stall it is barely moving at all.
        float streaming = smoothstep(1.1, 1.8, driftRate / rainSlideMetresPerSecond);

        // Heartfelt's own layer weights against how hard it rains.
        float mist = smoothstep(-0.5, 1.0, intensity) * 2.0;
        float layer1 = smoothstep(0.25, 0.75, intensity);
        float layer2 = smoothstep(0.0, 0.5, intensity);
        float staticCycle = time / rainStaticCycleSeconds;

        water = rainDrops(dropUv, scroll, acrossShare, streaming, staticCycle, mist, layer1, layer2);

        // **The refraction is the coverage field's own gradient**, by finite difference. One rule
        // bends the light through a head, through a trail and through every bead in it, and no shape
        // needs a surface of its own — which is what the arithmetic this replaces was spending most
        // of its length on.
        vec2 step = vec2(rainGradientStep, 0.0);
        float alongU = rainDrops(dropUv + step, scroll, acrossShare, streaming, staticCycle, mist, layer1, layer2).x;
        float alongV = rainDrops(dropUv + step.yx, scroll, acrossShare, streaming, staticCycle, mist, layer1, layer2).x;
        waterGradient = vec2(alongU - water.x, alongV - water.x);

        // That gradient points across the *pane*, and the copy it walks is a screen image, so the
        // one Jacobian this shader still takes turns it into a screen direction. **Only its
        // direction is used.** Its scale would make the lens depend on how much of the pane a pixel
        // covers, which is a property of where the camera is standing and not of the water.
        //
        // It is safe where the drift was not: it steers a bounded quantity rather than an
        // accumulation, so a quad's worth of jitter is worth a jittery refraction and nothing more.
        // A degenerate quad gives no offset at all.
        mat2 paneToScreen = mat2(vec2(dFdx(paneMetres.x), dFdx(paneMetres.y)),
                                 vec2(dFdy(paneMetres.x), dFdy(paneMetres.y)));
        refracted = behindUv;
        if (abs(determinant(paneToScreen)) > 1.0e-12)
        {
            vec2 screenDirection = inverse(paneToScreen) * waterGradient;
            float screenLength = length(screenDirection);
            if (screenLength > 1.0e-9)
            {
                float walk = min(length(waterGradient) * rainRefractionScreen, rainRefractionMaximum);
                refracted = behindUv + screenDirection * (walk / screenLength);
            }
        }

        // **The glass between the drops is misted, and the water is where it is clear.** This is
        // most of why the original reads as weather: a clear pane with beads on it reads as a dirty
        // window instead. The mist thins where a trail has just cut through it, and a drop reads the
        // scene almost sharp.
        float clarity = smoothstep(0.1, 0.2, water.x);
        waterLod = mix(rainFogLod - water.y * (rainFogLod - rainDropLod), rainDropLod, clarity);

        // The blade takes the water and the mist together, and the glass rewets from dry rather than
        // from wherever the field happened to be.
        float rewet = wiped ? smoothstep(0.0, rainRewetSeconds, wipeAge) : 1.0;

        float mistAlpha = rainFogAlpha * intensity * (1.0 - clarity) * (1.0 - water.y * 0.85);
        rainAlpha = clamp(max(water.x, mistAlpha) * rewet, 0.0, 1.0);

    }

    // ---- The dirt -------------------------------------------------------------------------------
    // A function of where on the glass this is, in metres, rather than a photograph sampled in UV —
    // see `windscreenGrime` for why that had to change and what it buys.
    vec4 grime = material.baseColour * windscreenGrime(paneMetres);

    // **And the water takes it with it.** Rain does not fade a dirty screen evenly; it cuts channels
    // down it and leaves the glass between them filthy, which is why this is keyed to the columns
    // the drops run in rather than to where a drop happens to be standing.
    grime.a *= 1.0 - grimeWash(dropUv, acrossShare, min(frame.timeRain.y, 1.0));

    vec3 rayDirection = normalize(positionInWorldSpace - frame.cameraPosition.xyz);

    int shadowLight = clamp(frame.shadowParams.y, 0, MAX_LIGHTS - 1);
    vec3 towardsLight = normalize(frame.lights[shadowLight].position.xyz);

    // The pane's own normal, flipped when seen from behind — from the driver's seat this *is* the
    // back face, since the surface is authored facing out of the car.
    vec3 normal = normalize(gl_FrontFacing ? normalsInWorldSpace : -normalsInWorldSpace);

    // Forward scattering, which is the entire effect: `dot(ray, towards the light)` is 1 when the eye
    // looks straight at the sun through the pane, and the lobe is worth nearly thirty there against
    // a fiftieth of that looking away from it.
    float phase = fogPhase(dot(rayDirection, towardsLight), grimeAnisotropy);

    // Whether the sun is on this pane at all. Without it the grime goes on glaring under the trees,
    // which is exactly where a low sun makes the effect most obvious and most wrong.
    float shadow = shadowFactor(positionInWorldSpace, normal, towardsLight, -positionInViewSpace.z);

    // A grazing ray crosses more grime than a square one.
    float pathLength = min(1.0 / max(abs(dot(normal, rayDirection)), 0.001), grimeMaximumPathLength);

    vec3 sunlight = frame.lights[shadowLight].diffuse.rgb * grimeInverseSphere * phase * shadow;
    vec3 lit = grime.rgb * (sunlight + grimeSkyRadiance()) * pathLength;

    // What the dirt blocks it does not simply delete. Dust forward-scatters most of what it stops,
    // so the share of the transmission that coverage takes away comes back as a blurred read of the
    // same scene — the behind copy, at the level of its chain standing in for the scattering cone.
    // This term is the difference between dirty glass and a grey film: a bright sky or a low sun
    // behind the grime glows *through* it, shaped by the per-texel coverage, while grime over a
    // dark dashboard stays dark. The behind copy and the fragment share pixel coordinates by
    // construction — it was blitted 1:1 from this very attachment.
    //
    // The grime's rgb is a *glare gain*, not a transmission albedo, and it is normalised here for a
    // reason that outlived the map it was found on: AC's `dirty-glass1` had a mean of 0.03, authored
    // against a sun three orders brighter than the scene behind the pane, and multiplied in raw it
    // deleted the very light this term exists to carry. The tint keeps the hue and the per-pixel
    // variation while the peak channel is divided out; how much light the dust passes is
    // grimeScatterFraction's to say, and how much of the pane is dust is alpha's.
    float grimePeak = max(grime.r, max(grime.g, grime.b));
    vec3 grimeTint = grimePeak > 0.0001 ? grime.rgb / grimePeak : vec3(1.0);
    vec3 transmitted = grimeScatterFraction * grimeTint * textureLod(sceneBehind, behindUv, grimeConeLod).rgb;

    // A blade takes the grime with the water, and the road's spray puts it back.
    float coverage =
        grime.a * (wiped ? mix(grimeWipedCoverage, 1.0, clamp(wipeAge / grimeRegrowSeconds, 0.0, 1.0)) : 1.0);

    // Wet glass, for the third of a second before the film drains: what the blade leaves behind
    // transmits a blurred read of the scene rather than a sharp one, which is the cue that says a
    // blade went past even where there was nothing on the glass to clear.
    float film = wiped ? wiperFilmOpacity * exp(-max(wipeAge, 0.0) / wiperFilmSeconds) : 0.0;
    vec3 filmLight = textureLod(sceneBehind, behindUv, 3.0).rgb;

    // Premultiplied, matching the contract the scene pass blends under (ONE, ONE_MINUS_SRC_ALPHA):
    // coverage scales what the surface adds — its own scattered light and the transmission it
    // diffuses — and the pane transmits the rest of what is behind it unscattered. The film takes
    // its own share of what coverage left; with no film this is exactly the line it replaced.
    // Rolled off rather than clipped, so that where the dirt is thick it still glares harder than
    // where it is thin. `min` set every sun-facing pixel to the same ceiling and deleted the
    // variation with it — see grimeMaximum.
    vec3 glare = lit / (1.0 + lit / grimeMaximum);

    fragColor = vec4((glare + transmitted) * coverage + filmLight * film * (1.0 - coverage),
                     coverage + film * (1.0 - coverage));


    if (rainAlpha > 0.0)
    {
        vec3 seen = textureLod(sceneBehind, clamp(refracted, vec2(0.0), vec2(1.0)), waterLod).rgb;

        // **The share a drop gathers from the whole cone rather than from the point it aims
        // at.** Only the water takes it — the mist between the drops is already reading a wide
        // level — and it is what stops a bead on a blown-out sky printing black because its
        // sample walked a few pixels off the sun. Near a bright source it makes the drop glow,
        // which is what a real one does.
        float lensed = clamp(water.x, 0.0, 1.0);
        vec3 halo = textureLod(sceneBehind, behindUv, rainDropHaloLod).rgb;
        seen = mix(seen, mix(seen, halo, rainDropHaloShare), lensed);

        // The rim carries a little of the sky as well as what is behind it. A drop over a
        // featureless background refracts that background into itself and disappears, which is
        // most of why drops seemed to come and go; the sky term is view-independent, so every
        // drop keeps an edge whatever stands behind it. Kept small — at four times this it drew
        // white rings rather than water.
        float rim = clamp(length(waterGradient) * 12.0, 0.0, 1.0);
        seen += grimeSkyRadiance() * rim * rainRimSky;

        // Straight "over", in the premultiplied terms the whole pass blends in.
        fragColor.rgb = fragColor.rgb * (1.0 - rainAlpha) + seen * rainAlpha;
        fragColor.a = fragColor.a * (1.0 - rainAlpha) + rainAlpha;
    }

    // The blade itself, over everything, because it is in front of everything.
    //
    // **This is a stand-in and is the one part of the wipers that is not the real thing**: the car
    // carries actual wiper geometry, but the `WIPER_*` nodes it hangs off are node *names*, which
    // the importer flattens away, so nothing can move that mesh yet — see
    // docs/windshield-rain-wiper-brief.md, stage 5. It is drawn because a wiper that clears the
    // glass while nothing visibly sweeps it does not read as a wiper; it reads as drops glitching
    // out. Sitting on the inner pane rather than in front of the outer one is a few millimetres of
    // lie at half a metre from the eye.
    float blade = 1.0 - smoothstep(frame.wiperTiming.w * 0.55, frame.wiperTiming.w, bladeGap);
    if (blade > 0.0)
    {
        fragColor.rgb = mix(fragColor.rgb, vec3(0.0), blade * wiperBladeOpacity);
        fragColor.a = mix(fragColor.a, 1.0, blade * wiperBladeOpacity);
    }
}
